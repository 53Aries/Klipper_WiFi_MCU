"""
spi_driver.py - Low-level SPI driver for Pi5 ↔ ESP32-C5 host

Manages the SPI bus and GPIO control lines.  Provides blocking
send() / recv() methods that exchange 256-byte KWM frames.

Hardware connections (default, match kwm_protocol.h pins for compact board):
  Pi5 SPI0 header     ESP32-C5 compact host board
  ─────────────────   ──────────────────────────────
  MOSI  (pin 19)  →   GPIO7  (FSPID)
  MISO  (pin 21)  ←   GPIO2  (FSPIQ)
  SCLK  (pin 23)  →   GPIO6  (FSPICLK)
  CE0   (pin 24)  →   GPIO10 (FSPICS0, active-low CS)
  GPIO8 (BCM)     ←   GPIO25 (DATA_READY: ESP→Pi, active-high interrupt)
  GPIO7 (BCM)     →   GPIO26 (HANDSHAKE:  Pi→ESP, active-high)  [optional]

Install dependencies on Pi5:
  sudo apt install python3-spidev python3-libgpiod

Usage:
  from spi_driver import SpiDriver
  drv = SpiDriver()
  drv.open()
  drv.send(mcu_id=0, data=b'hello')
  frame = drv.recv(timeout=1.0)
"""

import struct
import time
import threading
import logging
from typing import Optional, Tuple

try:
    import spidev
except ImportError:
    raise ImportError("Install spidev: sudo apt install python3-spidev")

try:
    import gpiod
    from gpiod.line import Direction, Value
except ImportError:
    raise ImportError("Install gpiod: sudo apt install python3-libgpiod")

log = logging.getLogger(__name__)

# ── Protocol constants (must match kwm_protocol.h) ────────────────────────────

MAGIC_0         = 0xAB
MAGIC_1         = 0xCD
FRAME_LEN       = 256
HEADER_LEN      = 8
CRC_LEN         = 2
PAYLOAD_MAX     = FRAME_LEN - HEADER_LEN - CRC_LEN  # 246

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
    """CRC16-CCITT: poly=0x1021, init=0xFFFF, no final XOR."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
            crc &= 0xFFFF
    return crc


# ── Frame builder/parser ──────────────────────────────────────────────────────

def build_frame(cmd: int, mcu_id: int, seq: int,
                payload: bytes, flags: int = FLAG_NONE) -> bytes:
    """Build a 256-byte SPI frame."""
    if len(payload) > PAYLOAD_MAX:
        raise ValueError(f"payload {len(payload)} > {PAYLOAD_MAX}")
    mcu_id_flags = (mcu_id & 0x0F) | (flags & 0xF0)
    hdr = struct.pack(">BBBBBBh",
                      MAGIC_0, MAGIC_1, cmd, mcu_id_flags,
                      seq, 0, len(payload))
    # hdr is 8 bytes; pad payload to PAYLOAD_MAX
    padded = payload + bytes(PAYLOAD_MAX - len(payload))
    body = hdr + padded
    crc = _crc16_ccitt(body)
    return body + struct.pack(">H", crc)


def parse_frame(raw: bytes) -> Optional[dict]:
    """Parse a 256-byte frame. Returns None on magic/CRC error."""
    if len(raw) != FRAME_LEN:
        return None
    if raw[0] != MAGIC_0 or raw[1] != MAGIC_1:
        return None
    expected_crc = _crc16_ccitt(raw[:-2])
    recv_crc = struct.unpack_from(">H", raw, FRAME_LEN - 2)[0]
    if expected_crc != recv_crc:
        log.debug("CRC mismatch: expected=%04x received=%04x", expected_crc, recv_crc)
        return None
    cmd          = raw[2]
    mcu_id_flags = raw[3]
    mcu_id       = mcu_id_flags & 0x0F
    flags        = mcu_id_flags & 0xF0
    seq          = raw[4]
    payload_len  = struct.unpack_from(">H", raw, 6)[0]
    payload      = raw[HEADER_LEN : HEADER_LEN + payload_len]
    return {
        "cmd": cmd,
        "mcu_id": mcu_id,
        "flags": flags,
        "seq": seq,
        "payload": bytes(payload),
    }


# ── SPI driver ────────────────────────────────────────────────────────────────

class SpiDriver:
    """
    Full-duplex SPI driver for Pi5 ↔ ESP32-C5 host communication.

    Thread-safe: send() and recv() can be called from different threads.
    Internally uses a background thread that polls DATA_READY and drives
    transactions; results are surfaced via thread-safe queues.
    """

    def __init__(
        self,
        spi_bus:      int = 0,
        spi_device:   int = 0,
        spi_speed_hz: int = 10_000_000,
        gpio_chip:    str = "/dev/gpiochip4",  # Pi5 main GPIO chip
        pin_data_ready: int = 8,   # BCM GPIO number for DATA_READY input
        pin_handshake:  int = 7,   # BCM GPIO number for HANDSHAKE output
    ):
        self._spi_bus      = spi_bus
        self._spi_device   = spi_device
        self._spi_speed_hz = spi_speed_hz
        self._gpio_chip    = gpio_chip
        self._pin_dr       = pin_data_ready
        self._pin_hs       = pin_handshake

        self._spi:      Optional[spidev.SpiDev] = None
        self._gpio_req = None

        # Thread-safe queues for TX and RX frames.
        import queue
        self._tx_queue: queue.Queue = queue.Queue(maxsize=16)
        self._rx_queue: queue.Queue = queue.Queue(maxsize=64)

        self._tx_seq   = 0
        self._lock     = threading.Lock()
        self._running  = False
        self._thread: Optional[threading.Thread] = None

        # Pre-built NOOP frame for when we have nothing to send.
        self._noop_frame = build_frame(CMD_NOOP, 0, 0, b"")

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    def open(self) -> None:
        """Open SPI device and GPIO lines, start background thread."""
        # SPI
        self._spi = spidev.SpiDev()
        self._spi.open(self._spi_bus, self._spi_device)
        self._spi.max_speed_hz = self._spi_speed_hz
        self._spi.mode         = 3    # CPOL=1, CPHA=1 – matches ESP32-C5 slave
        self._spi.bits_per_word = 8
        self._spi.no_cs        = False
        log.info("SPI opened: bus=%d dev=%d speed=%d Hz mode=3",
                 self._spi_bus, self._spi_device, self._spi_speed_hz)

        # GPIO (gpiod v2 API)
        self._gpio_req = gpiod.request_lines(
            self._gpio_chip,
            consumer="kwm_bridge",
            config={
                self._pin_dr: gpiod.LineSettings(direction=Direction.INPUT),
                self._pin_hs: gpiod.LineSettings(direction=Direction.OUTPUT,
                                                 output_value=Value.INACTIVE),
            },
        )
        log.info("GPIO ready: DATA_READY=GPIO%d HANDSHAKE=GPIO%d",
                 self._pin_dr, self._pin_hs)

        self._running = True
        self._thread = threading.Thread(target=self._driver_loop,
                                        name="kwm-spi", daemon=True)
        self._thread.start()

    def close(self) -> None:
        """Stop background thread and release resources."""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)
        if self._spi:
            self._spi.close()
        if self._gpio_req:
            self._gpio_req.release()

    def __enter__(self): self.open();  return self
    def __exit__(self, *_): self.close()

    # ── Public API ────────────────────────────────────────────────────────────

    def send(self, mcu_id: int, data: bytes,
             flags: int = FLAG_NONE, timeout: float = 1.0) -> None:
        """
        Queue raw serial data for delivery to the given MCU.
        Raises queue.Full if the TX queue is full after @timeout seconds.
        """
        import queue
        chunks = [data[i:i+PAYLOAD_MAX] for i in range(0, len(data), PAYLOAD_MAX)] or [b""]
        for chunk in chunks:
            with self._lock:
                seq = self._tx_seq
                self._tx_seq = (self._tx_seq + 1) & 0xFF
            frame = build_frame(CMD_DATA, mcu_id, seq, chunk, flags)
            self._tx_queue.put(frame, timeout=timeout)

    def recv(self, timeout: Optional[float] = None) -> Optional[dict]:
        """
        Return the next received frame dict (from any MCU), or None on timeout.
        Frame dict keys: cmd, mcu_id, flags, seq, payload (bytes).
        """
        import queue
        try:
            return self._rx_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def recv_data(self, mcu_id: int, timeout: Optional[float] = None) -> Optional[bytes]:
        """
        Convenience: return only the payload of the next DATA frame for @mcu_id.
        Non-matching frames are re-queued (best-effort; may reorder under load).
        """
        import queue
        deadline = (time.monotonic() + timeout) if timeout is not None else None
        while True:
            remaining = None
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
            frame = self.recv(timeout=remaining)
            if frame is None:
                return None
            if frame["cmd"] == CMD_DATA and frame["mcu_id"] == mcu_id:
                return frame["payload"]
            # Put non-matching frame back for another consumer.
            try:
                self._rx_queue.put_nowait(frame)
            except queue.Full:
                pass  # drop if queue is overwhelmed

    def status_request(self) -> None:
        """Send a STATUS_REQ frame (Pi→ESP)."""
        with self._lock:
            seq = self._tx_seq
            self._tx_seq = (self._tx_seq + 1) & 0xFF
        frame = build_frame(CMD_STATUS_REQ, 0, seq, b"")
        self._tx_queue.put_nowait(frame)

    # ── Background driver loop ─────────────────────────────────────────────────

    def _driver_loop(self) -> None:
        """
        Background thread: poll DATA_READY and drive SPI transactions.

        Policy:
        - If we have data to send OR DATA_READY is asserted → do a transaction.
        - Otherwise sleep briefly to avoid busy-looping.
        """
        import queue
        log.info("SPI driver loop started")
        while self._running:
            data_ready = self._gpio_req.get_value(self._pin_dr) == Value.ACTIVE
            have_tx    = not self._tx_queue.empty()

            if data_ready or have_tx:
                # Pick TX frame (or NOOP).
                try:
                    tx_frame = self._tx_queue.get_nowait()
                except queue.Empty:
                    tx_frame = self._noop_frame

                # Full-duplex transfer.
                try:
                    rx_raw = self._spi.xfer2(list(tx_frame), self._spi_speed_hz)
                except Exception as exc:
                    log.warning("SPI xfer error: %s", exc)
                    time.sleep(0.01)
                    continue

                # Parse received frame.
                rx_bytes = bytes(rx_raw)
                frame = parse_frame(rx_bytes)
                if frame and frame["cmd"] != CMD_NOOP:
                    try:
                        self._rx_queue.put_nowait(frame)
                    except queue.Full:
                        log.warning("RX queue full, dropping frame from MCU %d",
                                    frame["mcu_id"])

                # Slight delay between back-to-back transactions to avoid
                # starving the ESP32-C5 (it needs ~10 µs to refill DMA buffer).
                time.sleep(0.0001)   # 100 µs
            else:
                time.sleep(0.001)    # 1 ms idle sleep

        log.info("SPI driver loop exiting")
