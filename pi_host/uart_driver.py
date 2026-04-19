"""
uart_driver.py - Pi5 UART driver for KWM host transport

Communicates with the host XIAO ESP32-C5 over a 1 Mbaud UART on
/dev/ttyAMA2 (uart2-pi5, GPIO4/5).

Wiring (3 wires total):
  Pi GPIO4 (pin 7,  TXD) → XIAO GPIO12 (D7/RX)
  Pi GPIO5 (pin 29, RXD) ← XIAO GPIO11 (D6/TX)
  Pi GND   (any GND pin) → XIAO GND

Pi5 one-time setup:
  1. Add to /boot/firmware/config.txt:   dtoverlay=disable-bt-pi5
                                         dtoverlay=uart2-pi5
  2. sudo reboot
  3. sudo apt install python3-serial

Frame format: fixed 256-byte KWM frames (magic + 8-byte header + 246-byte
payload + 2-byte CRC16-CCITT), identical to the former SPI transport.
"""

import struct
import threading
import queue
import time
import logging
from typing import Optional

try:
    import serial
except ImportError:
    raise ImportError("Install pyserial: sudo apt install python3-serial")

log = logging.getLogger(__name__)

# ── Protocol constants (must match kwm_protocol.h) ────────────────────────────

MAGIC_0     = 0xAB
MAGIC_1     = 0xCD
FRAME_LEN   = 256
HEADER_LEN  = 8
CRC_LEN     = 2
PAYLOAD_MAX = FRAME_LEN - HEADER_LEN - CRC_LEN   # 246

CMD_NOOP        = 0x00
CMD_DATA        = 0x01
CMD_CONNECT     = 0x02
CMD_DISCONNECT  = 0x03
CMD_STATUS_REQ  = 0x04
CMD_STATUS_RSP  = 0x05
CMD_RESET       = 0x06
CMD_ACK         = 0x07

FLAG_NONE       = 0x00
FLAG_MORE_DATA  = 0x10


# ── CRC16-CCITT ───────────────────────────────────────────────────────────────

def _crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


# ── Frame builder / parser ────────────────────────────────────────────────────

def build_frame(cmd: int, mcu_id: int, seq: int,
                payload: bytes, flags: int = FLAG_NONE) -> bytes:
    if len(payload) > PAYLOAD_MAX:
        raise ValueError(f"payload {len(payload)} > {PAYLOAD_MAX}")
    mcu_id_flags = (mcu_id & 0x0F) | (flags & 0xF0)
    hdr = struct.pack(">BBBBBBH",
                      MAGIC_0, MAGIC_1, cmd, mcu_id_flags,
                      seq, 0, len(payload))
    padded = payload + bytes(PAYLOAD_MAX - len(payload))
    body   = hdr + padded
    return body + struct.pack(">H", _crc16_ccitt(body))


def parse_frame(raw: bytes) -> Optional[dict]:
    if len(raw) != FRAME_LEN:
        return None
    if raw[0] != MAGIC_0 or raw[1] != MAGIC_1:
        return None
    expected = _crc16_ccitt(raw[:-2])
    received = struct.unpack_from(">H", raw, FRAME_LEN - 2)[0]
    if expected != received:
        log.debug("CRC mismatch: expected=%04x got=%04x", expected, received)
        return None
    payload_len = struct.unpack_from(">H", raw, 6)[0]
    return {
        "cmd":     raw[2],
        "mcu_id":  raw[3] & 0x0F,
        "flags":   raw[3] & 0xF0,
        "seq":     raw[4],
        "payload": bytes(raw[HEADER_LEN : HEADER_LEN + payload_len]),
    }


# ── UART driver ───────────────────────────────────────────────────────────────

class UartDriver:
    """Full-duplex UART driver for Pi5 ↔ host ESP32-C5."""

    def __init__(self, port: str = "/dev/ttyAMA2", baudrate: int = 1_000_000):
        self._port     = port
        self._baudrate = baudrate
        self._ser: Optional[serial.Serial] = None

        self._tx_queue: queue.Queue = queue.Queue(maxsize=64)
        self._rx_queue: queue.Queue = queue.Queue(maxsize=64)
        self._tx_seq  = 0
        self._lock    = threading.Lock()
        self._running = False
        self._tx_thread: Optional[threading.Thread] = None
        self._rx_thread: Optional[threading.Thread] = None

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    def open(self) -> None:
        self._ser = serial.Serial(
            self._port, self._baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.1,
        )
        self._ser.reset_input_buffer()
        log.info("UART opened: %s @ %d baud", self._port, self._baudrate)
        self._running = True
        self._tx_thread = threading.Thread(target=self._tx_loop,
                                           name="kwm-uart-tx", daemon=True)
        self._rx_thread = threading.Thread(target=self._rx_loop,
                                           name="kwm-uart-rx", daemon=True)
        self._tx_thread.start()
        self._rx_thread.start()

    def close(self) -> None:
        self._running = False
        for t in (self._tx_thread, self._rx_thread):
            if t:
                t.join(timeout=2.0)
        if self._ser:
            self._ser.close()

    def __enter__(self): self.open(); return self
    def __exit__(self, *_): self.close()

    # ── Public API ────────────────────────────────────────────────────────────

    def send(self, mcu_id: int, data: bytes,
             flags: int = FLAG_NONE, timeout: float = 1.0) -> None:
        chunks = [data[i:i+PAYLOAD_MAX] for i in range(0, len(data), PAYLOAD_MAX)] or [b""]
        for chunk in chunks:
            with self._lock:
                seq = self._tx_seq
                self._tx_seq = (self._tx_seq + 1) & 0xFF
            frame = build_frame(CMD_DATA, mcu_id, seq, chunk, flags)
            self._tx_queue.put(frame, timeout=timeout)

    def recv(self, timeout: Optional[float] = None) -> Optional[dict]:
        try:
            return self._rx_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    # ── Background loops ──────────────────────────────────────────────────────

    def _tx_loop(self) -> None:
        log.info("UART TX loop started")
        while self._running:
            try:
                frame = self._tx_queue.get(timeout=0.1)
                self._ser.write(frame)
            except queue.Empty:
                pass
            except Exception as e:
                log.warning("UART TX error: %s", e)
        log.info("UART TX loop exiting")

    def _rx_loop(self) -> None:
        """Read fixed 256-byte frames from UART with magic-based resync."""
        log.info("UART RX loop started")
        buf = bytearray()

        while self._running:
            try:
                chunk = self._ser.read(FRAME_LEN - len(buf))
            except Exception as e:
                log.warning("UART RX read error: %s", e)
                time.sleep(0.01)
                continue

            if not chunk:
                continue

            buf.extend(chunk)

            # Discard leading bytes until we see the magic header.
            while len(buf) >= 2 and not (buf[0] == MAGIC_0 and buf[1] == MAGIC_1):
                log.debug("RX resync: discarding 0x%02x", buf[0])
                buf = buf[1:]

            if len(buf) < FRAME_LEN:
                continue

            raw = bytes(buf[:FRAME_LEN])
            buf = buf[FRAME_LEN:]

            frame = parse_frame(raw)
            if frame is None:
                log.debug("RX bad frame (CRC/magic), resyncing")
                # Put remaining bytes back so we re-examine them.
                buf = bytearray(raw[1:]) + buf
                continue

            if frame["cmd"] == CMD_NOOP:
                continue

            log.debug("RX cmd=0x%02x mcu=%d len=%d",
                      frame["cmd"], frame["mcu_id"], len(frame["payload"]))
            try:
                self._rx_queue.put_nowait(frame)
            except queue.Full:
                log.warning("RX queue full, dropping frame from MCU %d",
                            frame["mcu_id"])

        log.info("UART RX loop exiting")
