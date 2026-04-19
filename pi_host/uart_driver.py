"""
uart_driver.py - Pi5 UART driver for KWM host transport

Replaces spi_driver.py. Communicates with the host ESP32-C5 over
a 1 Mbaud UART on /dev/ttyAMA0 (Pi5, Bluetooth disabled via
dtoverlay=disable-bt in /boot/firmware/config.txt).

Wiring:
  Pi BCM14 (pin 8,  TXD) → XIAO GPIO12 (D7/RX)
  Pi BCM15 (pin 10, RXD) ← XIAO GPIO11 (D6/TX)
  Pi GND   (pin 6)       → XIAO GND

Frame format (variable-length, same header as SPI):
  [0..1]   Magic: 0xAB 0xCD
  [2]      Command
  [3]      MCU ID (bits 3:0) | Flags (bits 7:4)
  [4]      Sequence number
  [5]      Reserved (0)
  [6..7]   Payload length, big-endian
  [8..N]   Payload
  [N+1..N+2]  CRC16-CCITT of bytes [0..N]

Install:
  sudo apt install python3-serial
  Add dtoverlay=disable-bt to /boot/firmware/config.txt and reboot.

TODO: implement full send/recv — this is a stub.
"""

import struct
import threading
import queue
import logging
from typing import Optional

try:
    import serial
except ImportError:
    raise ImportError("Install pyserial: sudo apt install python3-serial")

log = logging.getLogger(__name__)

# ── Protocol constants (must match kwm_protocol.h) ────────────────────────────

MAGIC_0         = 0xAB
MAGIC_1         = 0xCD
HEADER_LEN      = 8
CRC_LEN         = 2

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

PAYLOAD_MAX     = 4096   # generous cap; typical Klipper frames are <256 bytes


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
        raise ValueError(f"payload too large: {len(payload)}")
    mcu_id_flags = (mcu_id & 0x0F) | (flags & 0xF0)
    hdr = struct.pack(">BBBBBBH",
                      MAGIC_0, MAGIC_1, cmd, mcu_id_flags,
                      seq, 0, len(payload))
    body = hdr + payload
    return body + struct.pack(">H", _crc16_ccitt(body))


def parse_frame(raw: bytes) -> Optional[dict]:
    if len(raw) < HEADER_LEN + CRC_LEN:
        return None
    if raw[0] != MAGIC_0 or raw[1] != MAGIC_1:
        return None
    payload_len = struct.unpack_from(">H", raw, 6)[0]
    expected_len = HEADER_LEN + payload_len + CRC_LEN
    if len(raw) != expected_len:
        return None
    expected_crc = _crc16_ccitt(raw[:-2])
    recv_crc = struct.unpack_from(">H", raw, -2)[0]
    if expected_crc != recv_crc:
        log.debug("CRC mismatch: expected=%04x got=%04x", expected_crc, recv_crc)
        return None
    return {
        "cmd":     raw[2],
        "mcu_id":  raw[3] & 0x0F,
        "flags":   raw[3] & 0xF0,
        "seq":     raw[4],
        "payload": bytes(raw[HEADER_LEN : HEADER_LEN + payload_len]),
    }


# ── UART driver ───────────────────────────────────────────────────────────────

class UartDriver:
    """
    Full-duplex UART driver for Pi5 ↔ host ESP32-C5.

    TODO: Implement _driver_loop with real UART send/recv.
    """

    def __init__(
        self,
        port:     str = "/dev/ttyAMA0",
        baudrate: int = 1_000_000,
    ):
        self._port     = port
        self._baudrate = baudrate
        self._ser: Optional[serial.Serial] = None
        self._tx_queue: queue.Queue = queue.Queue(maxsize=64)
        self._rx_queue: queue.Queue = queue.Queue(maxsize=64)
        self._tx_seq   = 0
        self._lock     = threading.Lock()
        self._running  = False
        self._thread: Optional[threading.Thread] = None

    def open(self) -> None:
        self._ser = serial.Serial(
            self._port, self._baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.1,
        )
        log.info("UART opened: %s @ %d baud", self._port, self._baudrate)
        self._running = True
        self._thread = threading.Thread(target=self._driver_loop,
                                        name="kwm-uart", daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)
        if self._ser:
            self._ser.close()

    def __enter__(self): self.open(); return self
    def __exit__(self, *_): self.close()

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

    def _driver_loop(self) -> None:
        """TODO: read frames from UART, write queued frames to UART."""
        log.warning("UartDriver._driver_loop: NOT YET IMPLEMENTED")
        while self._running:
            import time; time.sleep(1)
        log.info("UART driver loop exiting")
