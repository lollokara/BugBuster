"""
External bus handlers (I2C, SPI, deferred job queue) for SimulatedDevice.

Commands handled:
  EXT_I2C_SETUP      (0xB8) — configure I2C peripheral
  EXT_I2C_SCAN       (0xB9) — scan bus for device addresses
  EXT_I2C_WRITE      (0xBA) — write bytes to an I2C device
  EXT_I2C_READ       (0xBB) — read bytes from an I2C device
  EXT_I2C_WRITE_READ (0xBC) — write then read (repeated start)
  EXT_SPI_SETUP      (0xBD) — configure SPI peripheral
  EXT_SPI_TRANSFER   (0xBE) — full-duplex SPI transfer (loopback)
  EXT_JOB_SUBMIT     (0x75) — submit a deferred bus job
  EXT_JOB_GET        (0x76) — poll a deferred bus job result
"""

import struct
from bugbuster.constants import CmdId


def register(device) -> None:
    # I2C state: addr (int) -> register bytes
    if not hasattr(device, "i2c_devices"):
        device.i2c_devices: dict = {}
    # I2C config echo
    if not hasattr(device, "_i2c_config"):
        device._i2c_config = {"sda": 0, "scl": 0, "freq_hz": 100_000, "pullups": False}
    # SPI config echo
    if not hasattr(device, "_spi_config"):
        device._spi_config = {"sck": 0, "mosi": 0xFF, "miso": 0xFF, "cs": 0xFF, "freq_hz": 1_000_000, "mode": 0}
    # Deferred job queue
    if not hasattr(device, "job_queue"):
        device.job_queue: dict = {}
    if not hasattr(device, "_next_job_id"):
        device._next_job_id: int = 1

    device.register_handler(CmdId.EXT_I2C_SETUP,      _ext_i2c_setup(device))
    device.register_handler(CmdId.EXT_I2C_SCAN,       _ext_i2c_scan(device))
    device.register_handler(CmdId.EXT_I2C_WRITE,      _ext_i2c_write(device))
    device.register_handler(CmdId.EXT_I2C_READ,       _ext_i2c_read(device))
    device.register_handler(CmdId.EXT_I2C_WRITE_READ, _ext_i2c_write_read(device))
    device.register_handler(CmdId.EXT_SPI_SETUP,      _ext_spi_setup(device))
    device.register_handler(CmdId.EXT_SPI_TRANSFER,   _ext_spi_transfer(device))
    device.register_handler(CmdId.EXT_JOB_SUBMIT,     _ext_job_submit(device))
    device.register_handler(CmdId.EXT_JOB_GET,        _ext_job_get(device))


# ---------------------------------------------------------------------------
# EXT_I2C_SETUP (0xB8)
# client sends: struct.pack("<BBIB", sda, scl, freq_hz, pullups)
# client parses: resp[0]=sda, resp[1]=scl, resp[2:6]=freq_hz, resp[6]=pullups
# ---------------------------------------------------------------------------

def _ext_i2c_setup(device):
    def handler(payload: bytes) -> bytes:
        sda, scl, freq_hz, pullups = struct.unpack_from("<BBIB", payload)
        device._i2c_config = {"sda": sda, "scl": scl, "freq_hz": freq_hz, "pullups": bool(pullups)}
        return struct.pack("<BBIB", sda, scl, freq_hz, int(pullups))
    return handler


# ---------------------------------------------------------------------------
# EXT_I2C_SCAN (0xB9)
# client sends: struct.pack("<BBBH", start_addr, stop_addr, skip_reserved, timeout_ms)
# client parses: resp[0]=count, resp[1:1+count]=addresses
# ---------------------------------------------------------------------------

def _ext_i2c_scan(device):
    def handler(payload: bytes) -> bytes:
        start_addr, stop_addr, skip_reserved, timeout_ms = struct.unpack_from("<BBBH", payload)
        addrs = [
            addr for addr in device.i2c_devices
            if start_addr <= addr <= stop_addr
        ]
        count = len(addrs)
        return struct.pack("<B", count) + bytes(addrs)
    return handler


# ---------------------------------------------------------------------------
# EXT_I2C_WRITE (0xBA)
# client sends: struct.pack("<BHB", addr, timeout_ms, len) + data
# client parses: resp[0] = bytes written
# ---------------------------------------------------------------------------

def _ext_i2c_write(device):
    def handler(payload: bytes) -> bytes:
        addr, timeout_ms, data_len = struct.unpack_from("<BHB", payload)
        data = payload[4:4 + data_len]
        device.i2c_devices[addr] = bytes(data)
        return struct.pack("<B", len(data))
    return handler


# ---------------------------------------------------------------------------
# EXT_I2C_READ (0xBB)
# client sends: struct.pack("<BHB", addr, timeout_ms, length)
# client parses: resp[0]=count, resp[1:1+count]=data
# ---------------------------------------------------------------------------

def _ext_i2c_read(device):
    def handler(payload: bytes) -> bytes:
        addr, timeout_ms, length = struct.unpack_from("<BHB", payload)
        stored = device.i2c_devices.get(addr, b"")
        data = (stored + bytes(length))[:length]
        return struct.pack("<B", len(data)) + data
    return handler


# ---------------------------------------------------------------------------
# EXT_I2C_WRITE_READ (0xBC)
# client sends: struct.pack("<BHBB", addr, timeout_ms, write_len, read_len) + write_data
# client parses: resp[0]=count, resp[1:1+count]=data
# ---------------------------------------------------------------------------

def _ext_i2c_write_read(device):
    def handler(payload: bytes) -> bytes:
        addr, timeout_ms, write_len, read_len = struct.unpack_from("<BHBB", payload)
        write_data = payload[5:5 + write_len]
        device.i2c_devices[addr] = bytes(write_data)
        stored = device.i2c_devices.get(addr, b"")
        data = (stored + bytes(read_len))[:read_len]
        return struct.pack("<B", len(data)) + data
    return handler


# ---------------------------------------------------------------------------
# EXT_SPI_SETUP (0xBD)
# client sends: struct.pack("<BBBBIB", sck, mosi, miso, cs, freq_hz, mode)
# client parses: resp[0]=sck, resp[1]=mosi, resp[2]=miso, resp[3]=cs,
#                resp[4:8]=freq_hz, resp[8]=mode
# ---------------------------------------------------------------------------

def _ext_spi_setup(device):
    def handler(payload: bytes) -> bytes:
        sck, mosi, miso, cs, freq_hz, mode = struct.unpack_from("<BBBBIB", payload)
        device._spi_config = {"sck": sck, "mosi": mosi, "miso": miso, "cs": cs,
                               "freq_hz": freq_hz, "mode": mode}
        return struct.pack("<BBBBIB", sck, mosi, miso, cs, freq_hz, mode)
    return handler


# ---------------------------------------------------------------------------
# EXT_SPI_TRANSFER (0xBE)
# client sends: struct.pack("<HH", timeout_ms, len) + data
# client parses: count = struct.unpack_from("<H", resp, 0)[0]; resp[2:2+count]
# ---------------------------------------------------------------------------

def _ext_spi_transfer(device):
    def handler(payload: bytes) -> bytes:
        timeout_ms, data_len = struct.unpack_from("<HH", payload)
        tx_data = payload[4:4 + data_len]
        # loopback: echo TX as RX
        rx_data = bytes(tx_data)
        return struct.pack("<H", len(rx_data)) + rx_data
    return handler


# ---------------------------------------------------------------------------
# EXT_JOB_SUBMIT (0x75)
# client sends: struct.pack("<B", kind) + kind-specific fields
#   kind=1 (i2c_read):       <BHBB  (kind, timeout_ms, addr, length)
#   kind=2 (i2c_write_read): <BHBBB (kind, timeout_ms, addr, write_len, read_len) + write_data
#   kind=3 (spi_transfer):   <BHH   (kind, timeout_ms, tx_len) + tx_data
# client parses: job_id = struct.unpack_from("<I", resp, 0)[0]
# ---------------------------------------------------------------------------

def _ext_job_submit(device):
    def handler(payload: bytes) -> bytes:
        kind = payload[0]
        job_id = device._next_job_id
        device._next_job_id += 1

        result_data = b""
        if kind == 1:
            # i2c_read: <BHBB (kind, timeout_ms, addr, length)
            _kind, timeout_ms, addr, length = struct.unpack_from("<BHBB", payload)
            stored = device.i2c_devices.get(addr, b"")
            result_data = (stored + bytes(length))[:length]
        elif kind == 2:
            # i2c_write_read: <BHBBB + write_data
            _kind, timeout_ms, addr, write_len, read_len = struct.unpack_from("<BHBBB", payload)
            write_data = payload[6:6 + write_len]
            device.i2c_devices[addr] = bytes(write_data)
            stored = device.i2c_devices.get(addr, b"")
            result_data = (stored + bytes(read_len))[:read_len]
        elif kind == 3:
            # spi_transfer: <BHH + tx_data
            _kind, timeout_ms, tx_len = struct.unpack_from("<BHH", payload)
            tx_data = payload[5:5 + tx_len]
            result_data = bytes(tx_data)  # loopback

        device.job_queue[job_id] = {"kind": kind, "status": 3, "data": result_data}
        return struct.pack("<I", job_id)
    return handler


# ---------------------------------------------------------------------------
# EXT_JOB_GET (0x76)
# client sends: struct.pack("<I", job_id)
# client parses: job_id I, status B, kind B, result_len H, _pad H, data[result_len]
#   resp[0:4]=job_id, resp[4]=status, resp[5]=kind, resp[6:8]=result_len, resp[8:]=data
# ---------------------------------------------------------------------------

def _ext_job_get(device):
    def handler(payload: bytes) -> bytes:
        job_id = struct.unpack_from("<I", payload)[0]
        job = device.job_queue.get(job_id)
        if job is None:
            # Unknown job: return empty/error status
            return struct.pack("<IBBH", job_id, 0, 0, 0)
        data = job["data"]
        result_len = len(data)
        # Header: job_id(4) + status(1) + kind(1) + result_len(2) = 8 bytes, then data
        return struct.pack("<IBBH", job_id, job["status"], job["kind"], result_len) + data
    return handler
