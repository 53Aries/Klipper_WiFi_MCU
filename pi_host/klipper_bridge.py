"""
klipper_bridge.py - Pi5 daemon: SPI ↔ PTY bridge for Klipper

Creates one Linux pseudo-terminal (PTY) per MCU ID (0-7 by default).
Klipper talks to each PTY as if it were a normal serial device.

Klipper printer.cfg example:
  [mcu mcu0]
  serial: /dev/kwm0        # symlink created by this daemon

  [mcu mcu1]
  serial: /dev/kwm1

Run (no arguments needed — all 8 slots created automatically):
  sudo python3 klipper_bridge.py

Or restrict to specific IDs:
  sudo python3 klipper_bridge.py --mcus 0 3

The daemon creates /dev/kwm<N> symlinks pointing to the allocated PTY
slave device (/dev/pts/X). Klipper can then be configured with these
stable symlink paths instead of volatile /dev/pts/X numbers.

Requirements:
  sudo apt install python3-spidev python3-libgpiod
"""

import argparse
import os
import pty
import select
import signal
import sys
import threading
import time
import logging

from spi_driver import SpiDriver, CMD_DATA, CMD_CONNECT, CMD_DISCONNECT, PAYLOAD_MAX

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(name)s %(levelname)s %(message)s",
)
log = logging.getLogger("klipper_bridge")

SYMLINK_PREFIX = "/dev/kwm"   # /dev/kwm0, /dev/kwm1, ...


# ── PTY channel ───────────────────────────────────────────────────────────────

class PtyChannel:
    """
    Wraps a master/slave PTY pair for one MCU.

    - Klipper opens the slave fd (via /dev/kwm<N> symlink).
    - klipper_bridge reads/writes the master fd.
    """

    def __init__(self, mcu_id: int):
        self.mcu_id = mcu_id
        master_fd, slave_fd = pty.openpty()
        self.master_fd = master_fd
        self.slave_fd  = slave_fd
        self.slave_name = os.ttyname(slave_fd)
        os.chmod(self.slave_name, 0o666)  # allow non-root (klipper user) to open
        log.info("MCU %d PTY: %s", mcu_id, self.slave_name)

    def create_symlink(self) -> str:
        path = f"{SYMLINK_PREFIX}{self.mcu_id}"
        try:
            if os.path.lexists(path):
                os.unlink(path)
            os.symlink(self.slave_name, path)
            log.info("MCU %d symlink: %s -> %s", self.mcu_id, path, self.slave_name)
        except OSError as e:
            log.warning("Could not create symlink %s: %s (run as root?)", path, e)
        return path

    def remove_symlink(self) -> None:
        path = f"{SYMLINK_PREFIX}{self.mcu_id}"
        try:
            if os.path.lexists(path):
                os.unlink(path)
        except OSError:
            pass

    def read_nowait(self) -> bytes:
        """Read any available bytes from the master fd (non-blocking)."""
        r, _, _ = select.select([self.master_fd], [], [], 0)
        if r:
            try:
                return os.read(self.master_fd, PAYLOAD_MAX)
            except OSError:
                return b""
        return b""

    def write(self, data: bytes) -> None:
        """Write bytes to the master fd (Klipper reads from slave)."""
        try:
            os.write(self.master_fd, data)
        except OSError as e:
            log.warning("PTY write for MCU %d failed: %s", self.mcu_id, e)

    def close(self) -> None:
        for fd in (self.master_fd, self.slave_fd):
            try:
                os.close(fd)
            except OSError:
                pass


# ── Bridge daemon ─────────────────────────────────────────────────────────────

class KlipperBridge:
    """
    Main bridge: polls PTYs for outgoing data and SpiDriver for incoming data.
    """

    def __init__(self, mcu_ids: list, spi_kwargs: dict):
        self._channels: dict[int, PtyChannel] = {}
        for mid in mcu_ids:
            self._channels[mid] = PtyChannel(mid)

        self._drv = SpiDriver(**spi_kwargs)
        self._running = False

    def start(self) -> None:
        self._drv.open()
        for ch in self._channels.values():
            ch.create_symlink()
        self._running = True

        # Spawn receive thread (SPI→PTY).
        self._rx_thread = threading.Thread(target=self._rx_loop,
                                           name="spi-rx", daemon=True)
        self._rx_thread.start()

        log.info("Bridge running. MCUs: %s", list(self._channels.keys()))

    def stop(self) -> None:
        self._running = False
        self._drv.close()
        for ch in self._channels.values():
            ch.remove_symlink()
            ch.close()
        log.info("Bridge stopped")

    def run_forever(self) -> None:
        """TX loop (main thread): PTY → SPI."""
        try:
            while self._running:
                sent_any = False
                for mid, ch in self._channels.items():
                    data = ch.read_nowait()
                    if data:
                        try:
                            self._drv.send(mcu_id=mid, data=data, timeout=0.5)
                            sent_any = True
                        except Exception as e:
                            log.warning("SPI send for MCU %d failed: %s", mid, e)
                if not sent_any:
                    time.sleep(0.0005)   # 500 µs idle
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()

    def _rx_loop(self) -> None:
        """SPI → PTY receive loop (background thread)."""
        while self._running:
            frame = self._drv.recv(timeout=0.1)
            if frame is None:
                continue
            if frame["cmd"] == CMD_CONNECT:
                log.info("MCU %d connected (MAC: %s)", frame["mcu_id"],
                         frame["payload"].hex(":") if frame["payload"] else "?")
                continue
            if frame["cmd"] == CMD_DISCONNECT:
                log.info("MCU %d disconnected", frame["mcu_id"])
                continue
            if frame["cmd"] != CMD_DATA:
                continue
            mid = frame["mcu_id"]
            ch = self._channels.get(mid)
            if ch is None:
                log.debug("Received data for unconfigured MCU %d, discarding", mid)
                continue
            if frame["payload"]:
                ch.write(frame["payload"])


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Klipper WiFi MCU SPI↔PTY bridge daemon")
    parser.add_argument("--mcus", nargs="+", type=int, default=list(range(8)),
                        metavar="ID",
                        help="MCU IDs to bridge (default: all 8, i.e. 0-7)")
    parser.add_argument("--spi-bus",    type=int, default=0)
    parser.add_argument("--spi-dev",    type=int, default=0)
    parser.add_argument("--spi-speed",  type=int, default=100_000,
                        help="SPI clock Hz (default 100 kHz)")
    parser.add_argument("--gpio-chip",  default="/dev/gpiochip4",
                        help="GPIO chip device (Pi5 default: gpiochip4)")
    parser.add_argument("--pin-dr",     type=int, default=25,
                        help="BCM GPIO for DATA_READY input from ESP32-C5")
    parser.add_argument("--pin-hs",     type=int, default=24,
                        help="BCM GPIO for HANDSHAKE output to ESP32-C5")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    spi_kwargs = {
        "spi_bus":        args.spi_bus,
        "spi_device":     args.spi_dev,
        "spi_speed_hz":   args.spi_speed,
        "gpio_chip":      args.gpio_chip,
        "pin_data_ready": args.pin_dr,
        "pin_handshake":  args.pin_hs,
    }

    bridge = KlipperBridge(mcu_ids=args.mcus, spi_kwargs=spi_kwargs)

    def _shutdown(sig, frame):
        log.info("Signal %d received, shutting down...", sig)
        bridge._running = False

    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT,  _shutdown)

    bridge.start()
    bridge.run_forever()


if __name__ == "__main__":
    main()
