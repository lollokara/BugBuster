"""BugBuster client methods: external target I2C, SPI and UART.

One layer of :class:`bugbuster.client.BugBuster` (core -> system -> scripting ->
channels -> bus -> power -> hat). ``bugbuster.client`` imports the chain at its
end, so import ``bugbuster.client`` (or ``bugbuster``) first.
"""

from typing import Optional
from typing import Union
import struct

from .constants import CmdId
from .transport.usb import DeviceError
from .client import (
    _i2c_addr7,
    _require_resp_len,
)
from .client_channels import _ClientChannels


class _ClientBus(_ClientChannels):
    # ------------------------------------------------------------------
    # ── External target I2C bus (routed IO pins) ───────────────────────
    # ------------------------------------------------------------------

    def ext_i2c_setup(
        self,
        *,
        sda_gpio: int,
        scl_gpio: int,
        frequency_hz: int = 400_000,
        pullups: str = "external",
    ) -> dict:
        internal_pullups = pullups.lower() == "internal"
        if not self._usb:
            return self._http_post("/bus/i2c/setup", {
                "sdaGpio": sda_gpio,
                "sclGpio": scl_gpio,
                "frequencyHz": int(frequency_hz),
                "internalPullups": internal_pullups,
            })
        payload = struct.pack("<BBIB", sda_gpio, scl_gpio, int(frequency_hz), int(internal_pullups))
        resp = self._usb_cmd(CmdId.EXT_I2C_SETUP, payload)
        return {
            "sda_gpio": resp[0],
            "scl_gpio": resp[1],
            "frequency_hz": struct.unpack_from("<I", resp, 2)[0],
            "internal_pullups": bool(resp[6]),
        }

    def ext_i2c_scan(
        self,
        *,
        start_addr: int = 0x08,
        stop_addr: int = 0x77,
        skip_reserved: bool = True,
        timeout_ms: int = 50,
    ) -> list[int]:
        """Scan the configured external I2C bus."""
        if not self._usb:
            raw = self._http_post("/bus/i2c/scan", {
                "startAddr": start_addr,
                "stopAddr": stop_addr,
                "skipReserved": skip_reserved,
                "timeoutMs": timeout_ms,
            })
            return [int(addr) for addr in raw.get("addresses", [])]
        payload = struct.pack("<BBBH", start_addr & 0x7F, stop_addr & 0x7F, int(skip_reserved), timeout_ms & 0xFFFF)
        resp = self._usb_cmd(CmdId.EXT_I2C_SCAN, payload)
        count = resp[0]
        return list(resp[1:1 + count])

    def ext_i2c_write(self, address: int, data: Union[bytes, bytearray, list[int]], *, timeout_ms: int = 100) -> int:
        """Write bytes to the configured external I2C bus."""
        address = _i2c_addr7(address)
        raw = bytes(data)
        if len(raw) > 255:
            raise ValueError("I2C write payload must be <=255 bytes")
        if not self._usb:
            http_resp = self._http_post("/bus/i2c/write", {
                "address": address & 0x7F,
                "timeoutMs": timeout_ms,
                "data": list(raw),
            })
            return int(http_resp.get("written", 0))
        payload = struct.pack("<BHB", address & 0x7F, timeout_ms & 0xFFFF, len(raw)) + raw
        resp = self._ext_i2c_cmd(CmdId.EXT_I2C_WRITE, payload, address)
        return resp[0]

    def ext_i2c_read(self, address: int, length: int, *, timeout_ms: int = 100) -> bytes:
        """Read bytes from the configured external I2C bus."""
        address = _i2c_addr7(address)
        if not (1 <= length <= 255):
            raise ValueError("I2C read length must be 1-255 bytes")
        if not self._usb:
            http_resp = self._http_post("/bus/i2c/read", {
                "address": address & 0x7F,
                "length": length,
                "timeoutMs": timeout_ms,
            })
            return bytes(http_resp.get("data", []))
        payload = struct.pack("<BHB", address & 0x7F, timeout_ms & 0xFFFF, length)
        resp = self._ext_i2c_cmd(CmdId.EXT_I2C_READ, payload, address)
        count = resp[0]
        return bytes(resp[1:1 + count])

    def ext_i2c_write_read(
        self,
        address: int,
        write_data: Union[bytes, bytearray, list[int]],
        read_length: int,
        *,
        timeout_ms: int = 100,
    ) -> bytes:
        """Write bytes then repeated-start read from the configured external I2C bus."""
        address = _i2c_addr7(address)
        raw = bytes(write_data)
        if not (1 <= len(raw) <= 255):
            raise ValueError("I2C write-read write payload must be 1-255 bytes")
        if not (1 <= read_length <= 255):
            raise ValueError("I2C write-read length must be 1-255 bytes")
        if not self._usb:
            http_resp = self._http_post("/bus/i2c/write_read", {
                "address": address & 0x7F,
                "writeData": list(raw),
                "readLength": read_length,
                "timeoutMs": timeout_ms,
            })
            return bytes(http_resp.get("data", []))
        payload = struct.pack("<BHBB", address & 0x7F, timeout_ms & 0xFFFF, len(raw), read_length) + raw
        resp = self._ext_i2c_cmd(CmdId.EXT_I2C_WRITE_READ, payload, address)
        count = resp[0]
        return bytes(resp[1:1 + count])

    def _ext_i2c_cmd(self, cmd: int, payload: bytes, address: int) -> bytes:
        """BUS-017: the firmware reports a NACK as a hardware fault, which BBP
        carries as SPI_FAIL; say what it is. TIMEOUT (stuck bus) and BUSY pass
        through unchanged."""
        try:
            return self._usb_cmd(cmd, payload)
        except DeviceError as exc:
            if exc.code != 0x04:  # BBP_ERR_SPI_FAIL
                raise
            nack = DeviceError(exc.code, exc.seq)
            nack.args = (f"I2C NACK: no device acknowledged address 0x{address:02X} (seq={exc.seq})",)
            raise nack from exc

    # ------------------------------------------------------------------
    # ── External target SPI bus (routed IO pins) ───────────────────────
    # ------------------------------------------------------------------

    def ext_spi_setup(
        self,
        *,
        sck_gpio: int,
        mosi_gpio: Optional[int] = None,
        miso_gpio: Optional[int] = None,
        cs_gpio: Optional[int] = None,
        frequency_hz: int = 1_000_000,
        mode: int = 0,
    ) -> dict:
        """Configure the ESP32 external SPI peripheral on already-routed GPIOs."""
        if not self._usb:
            return self._http_post("/bus/spi/setup", {
                "sckGpio": sck_gpio,
                "mosiGpio": mosi_gpio,
                "misoGpio": miso_gpio,
                "csGpio": cs_gpio,
                "frequencyHz": int(frequency_hz),
                "mode": mode,
            })
        nc = 0xFF
        payload = struct.pack(
            "<BBBBIB",
            sck_gpio,
            nc if mosi_gpio is None else mosi_gpio,
            nc if miso_gpio is None else miso_gpio,
            nc if cs_gpio is None else cs_gpio,
            int(frequency_hz),
            mode & 0x03,
        )
        resp = self._usb_cmd(CmdId.EXT_SPI_SETUP, payload)
        return {
            "sck_gpio": resp[0],
            "mosi_gpio": None if resp[1] == nc else resp[1],
            "miso_gpio": None if resp[2] == nc else resp[2],
            "cs_gpio": None if resp[3] == nc else resp[3],
            "frequency_hz": struct.unpack_from("<I", resp, 4)[0],
            "mode": resp[8],
        }

    def ext_spi_transfer(self, data: Union[bytes, bytearray, list[int]], *, timeout_ms: int = 100) -> bytes:
        """Run a bounded full-duplex transfer on the configured external SPI bus."""
        raw = bytes(data)
        if not (1 <= len(raw) <= 512):
            raise ValueError("SPI transfer length must be 1-512 bytes")
        if not self._usb:
            http_resp = self._http_post("/bus/spi/transfer", {
                "data": list(raw),
                "timeoutMs": timeout_ms,
            })
            return bytes(http_resp.get("data", []))
        payload = struct.pack("<HH", timeout_ms & 0xFFFF, len(raw)) + raw
        resp = self._usb_cmd(CmdId.EXT_SPI_TRANSFER, payload)
        count = struct.unpack_from("<H", resp, 0)[0]
        return bytes(resp[2:2 + count])

    def ext_job_submit_i2c_read(self, address: int, length: int, *, timeout_ms: int = 100) -> int:
        """Queue a deferred I2C read in ESP32 RAM/PSRAM and return its job id."""
        address = _i2c_addr7(address)
        if not self._usb:
            raise NotImplementedError("deferred bus jobs are currently available over USB BBP only")
        if not (1 <= length <= 255):
            raise ValueError("I2C deferred read length must be 1-255 bytes")
        payload = struct.pack("<BHBB", 1, timeout_ms & 0xFFFF, address & 0x7F, length)
        resp = self._usb_cmd(CmdId.EXT_JOB_SUBMIT, payload)
        return struct.unpack_from("<I", resp, 0)[0]

    def ext_job_submit_i2c_write_read(
        self,
        address: int,
        write_data: Union[bytes, bytearray, list[int]],
        read_length: int,
        *,
        timeout_ms: int = 100,
    ) -> int:
        """Queue a deferred I2C write/read transaction and return its job id."""
        address = _i2c_addr7(address)
        if not self._usb:
            raise NotImplementedError("deferred bus jobs are currently available over USB BBP only")
        raw = bytes(write_data)
        if not (1 <= len(raw) <= 255):
            raise ValueError("I2C deferred write payload must be 1-255 bytes")
        if not (1 <= read_length <= 255):
            raise ValueError("I2C deferred read length must be 1-255 bytes")
        payload = struct.pack(
            "<BHBBB",
            2,
            timeout_ms & 0xFFFF,
            address & 0x7F,
            len(raw),
            read_length,
        ) + raw
        resp = self._usb_cmd(CmdId.EXT_JOB_SUBMIT, payload)
        return struct.unpack_from("<I", resp, 0)[0]

    def ext_job_submit_spi_transfer(self, data: Union[bytes, bytearray, list[int]], *, timeout_ms: int = 100) -> int:
        """Queue a deferred SPI transfer and return its job id."""
        if not self._usb:
            raise NotImplementedError("deferred bus jobs are currently available over USB BBP only")
        raw = bytes(data)
        if not (1 <= len(raw) <= 512):
            raise ValueError("SPI deferred transfer length must be 1-512 bytes")
        payload = struct.pack("<BHH", 3, timeout_ms & 0xFFFF, len(raw)) + raw
        resp = self._usb_cmd(CmdId.EXT_JOB_SUBMIT, payload)
        return struct.unpack_from("<I", resp, 0)[0]

    def ext_job_get(self, job_id: int) -> dict:
        """Poll a deferred external bus job."""
        if not self._usb:
            raise NotImplementedError("deferred bus jobs are currently available over USB BBP only")
        resp = self._usb_cmd(CmdId.EXT_JOB_GET, struct.pack("<I", job_id & 0xFFFFFFFF))
        _require_resp_len(resp, 8, "EXT_JOB_GET")
        result_len = struct.unpack_from("<H", resp, 6)[0]
        _require_resp_len(resp, 8 + result_len, "EXT_JOB_GET")
        return {
            "job_id": struct.unpack_from("<I", resp, 0)[0],
            "status": resp[4],
            "status_name": {
                0: "empty",
                1: "queued",
                2: "running",
                3: "done",
                4: "error",
            }.get(resp[4], "unknown"),
            "kind": resp[5],
            "kind_name": {
                1: "i2c_read",
                2: "i2c_write_read",
                3: "spi_transfer",
            }.get(resp[5], "unknown"),
            "data": bytes(resp[8:8 + result_len]),
        }

    # ------------------------------------------------------------------
    # ── UART bridge ────────────────────────────────────────────────────
    # ------------------------------------------------------------------

    def get_uart_config(self) -> list[dict]:
        """
        Read the configuration of all UART bridges.

        Returns a list of dicts, one per bridge::

            [
                {
                    "bridge_id": 0,
                    "uart_num":  1,        # ESP32 UART peripheral (0–2)
                    "tx_pin":    17,       # ESP32 GPIO for TX
                    "rx_pin":    18,       # ESP32 GPIO for RX
                    "baudrate":  115200,
                    "data_bits": 8,
                    "parity":    0,        # 0=none, 1=odd, 2=even
                    "stop_bits": 0,        # 0=1, 1=1.5, 2=2
                    "enabled":   False,
                },
                …
            ]
        """
        if not self._usb:
            http_resp = self._http_get("/uart/config")
            bridges = http_resp.get("bridges", [])
            return [
                {
                    "bridge_id": b.get("id", 0),
                    "uart_num":  b.get("uartNum", 0),
                    "tx_pin":    b.get("txPin", -1),
                    "rx_pin":    b.get("rxPin", -1),
                    "baudrate":  b.get("baudrate", 115200),
                    "data_bits": b.get("dataBits", 8),
                    "parity":    b.get("parity", 0),
                    "stop_bits": b.get("stopBits", 0),
                    "enabled":   bool(b.get("enabled", False)),
                    "connected": bool(b.get("connected", False)),
                }
                for b in bridges
            ]

        resp   = self._usb_cmd(CmdId.GET_UART_CONFIG)
        count  = resp[0]
        result = []
        off    = 1
        for _ in range(count):
            bridge_id = resp[off]
            uart_num  = resp[off + 1]
            tx_pin    = resp[off + 2]
            rx_pin    = resp[off + 3]
            baudrate, = struct.unpack_from('<I', resp, off + 4)
            data_bits = resp[off + 8]
            parity    = resp[off + 9]
            stop_bits = resp[off + 10]
            enabled   = bool(resp[off + 11])
            off      += 12
            result.append({
                "bridge_id": bridge_id,
                "uart_num":  uart_num,
                "tx_pin":    tx_pin,
                "rx_pin":    rx_pin,
                "baudrate":  baudrate,
                "data_bits": data_bits,
                "parity":    parity,
                "stop_bits": stop_bits,
                "enabled":   enabled,
            })
        return result

    def set_uart_config(
        self,
        bridge_id: int   = 0,
        uart_num:  int   = 1,
        tx_pin:    int   = 1,
        rx_pin:    int   = 2,
        baudrate:  int   = 115200,
        data_bits: int   = 8,
        parity:    int   = 0,
        stop_bits: int   = 0,
        enabled:   bool  = False,
    ) -> None:
        """
        Configure a UART bridge.

        Routes a secondary serial port to the specified ESP32 GPIO pins.
        The MUX must be configured to connect these ESP32 GPIOs to the
        desired physical IO terminals.

        Parameters
        ----------
        bridge_id : int
            Bridge index (0 or 1).
        uart_num : int
            ESP32 UART peripheral (0–2; default 1).
        tx_pin : int
            ESP32 GPIO pin number for TX output.
        rx_pin : int
            ESP32 GPIO pin number for RX input.
        baudrate : int
            Baud rate in bps (300–3,000,000; default 115200).
        data_bits : int
            Data bits per frame (5, 6, 7, or 8; default 8).
        parity : int
            Parity mode — 0=none, 1=odd, 2=even (default 0).
        stop_bits : int
            Stop bits — 0=1 stop bit, 1=1.5, 2=2 (default 0).
        enabled : bool
            Activate the bridge immediately (default False).
        """
        if not self._usb:
            self._http_post(f"/uart/{bridge_id}/config", {
                "uartNum":  uart_num,
                "txPin":    tx_pin,
                "rxPin":    rx_pin,
                "baudrate":  baudrate,
                "dataBits": data_bits,
                "parity":   parity,
                "stopBits": stop_bits,
                "enabled":  enabled,
            })
            return

        payload = struct.pack(
            '<BBBBIBBBB',
            bridge_id, uart_num, tx_pin, rx_pin,
            baudrate, data_bits, parity, stop_bits, int(enabled),
        )
        self._usb_cmd(CmdId.SET_UART_CONFIG, payload)

    def get_uart_pins(self) -> list[int]:
        """
        Return a list of ESP32 GPIO pin numbers available for UART routing.

        Pins currently in use by other functions (SPI, I2C, USB, etc.) are
        excluded.
        """
        if not self._usb:
            http_resp = self._http_get("/uart/pins")
            return list(http_resp.get("available", []))

        resp  = self._usb_cmd(CmdId.GET_UART_PINS)
        count = resp[0]
        return list(resp[1:1 + count])
