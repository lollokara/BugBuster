"""TR-2: responses whose size is fixed at compile time must fit one BBP frame.

sendMsg() frames [type u8][seq u16][cmd u8] + payload + [crc u16] into
BBP_MAX_PAYLOAD bytes. SCRIPT_LOGS replies with a u16 count plus up to
SCRIPT_LOG_CHUNK bytes; a full log buffer used to exceed the frame and the
response was silently dropped (host timeout).
"""

from tests.firmware_host.fwhost import extract_defines

BBP_H = "Firmware/ESP32/src/bbp/bbp.h"
CMD_SCRIPT = "Firmware/ESP32/src/bbp/cmds/cmd_script.cpp"
FRAME_OVERHEAD = 4 + 2  # header + CRC


def _define(path: str, name: str) -> int:
    text = extract_defines(path, [name])
    return int(text.split(name, 1)[1].split("//", 1)[0].strip(), 0)


def test_script_logs_full_chunk_fits_a_frame():
    max_payload = _define(BBP_H, "BBP_MAX_PAYLOAD")
    chunk = _define(CMD_SCRIPT, "SCRIPT_LOG_CHUNK")
    frame = FRAME_OVERHEAD + 2 + chunk
    assert frame <= max_payload, f"{frame} > BBP_MAX_PAYLOAD {max_payload}"
