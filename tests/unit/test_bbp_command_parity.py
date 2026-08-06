"""Cross-surface parity for the BBP command set.

Three independent implementations of one wire protocol drift silently:
the firmware defines an opcode, the Python client forgets to send it, the
desktop app spells it differently. Nothing at runtime notices, because an
unused opcode simply never appears on the wire.

This is not hypothetical. `STOP_SCOPE_STREAM` (0x63) was defined in bbp.h,
enumerated in constants.py, implemented in the firmware AND in the simulator,
and referenced by `on_scope_data`'s own docstring -- but no client method ever
sent it, so a scope stream could only be stopped by resetting the device.
Found 2026-08-06 by the reachability check below.
"""

import re

from bugbuster.constants import CmdId
from tests.lib.srcread import read_source

BBP_H = read_source("Firmware/ESP32/src/bbp/bbp.h")
CLIENT_PY = read_source("python/bugbuster/client.py")
DAQ_CONFIG_PY = read_source("python/bugbuster/daq_config.py")
OTA_PY = read_source("python/bugbuster/ota.py")


def test_simulator_adc_rate_codes_match_the_firmware_enum():
    """The simulator rejects DSP requests carrying an invalid CONV_RATE code,
    which means it hardcodes the valid set. AdcRate is NOT contiguous (2, 5, 7,
    10, 11 are absent), so a retyped literal is easy to get wrong and nothing
    at runtime would notice -- the simulator would simply accept a code the
    device rejects."""
    regs_h = read_source("Firmware/ESP32/src/hal/ad74416h_regs.h")
    body = regs_h.split("typedef enum {", 1)[1].split("} AdcRate;", 1)[0]
    firmware = {
        int(v) for _, v in re.findall(r"(ADC_RATE_\w+)\s*=\s*(\d+)", body)
    }
    assert firmware, "could not parse AdcRate out of ad74416h_regs.h"

    streaming = read_source("tests/mock/handlers/streaming.py")
    m = re.search(r"_VALID_ADC_RATES\s*=\s*frozenset\(\{([^}]*)\}\)", streaming)
    assert m, "_VALID_ADC_RATES not found in the simulator"
    simulated = {int(x) for x in m.group(1).replace("\n", "").split(",") if x.strip()}

    assert simulated == firmware, (
        f"simulator accepts {sorted(simulated)} but the firmware's AdcRate enum "
        f"is {sorted(firmware)}"
    )

# Opcodes the device sends to the host. The client subscribes to these via
# on_event() rather than sending them.
_EVENT_CMDS = {
    "ADC_DATA_EVT", "SCOPE_DATA_EVT", "ALERT_EVT", "DIN_EVT",
    "PCA_FAULT_EVT", "ADC_DSP_EVT", "HAT_LA_LOG_EVT",
}

# Opcodes the firmware reserves but the Python client legitimately never
# sends. Each entry needs a reason -- an unexplained entry here is how a real
# gap gets hidden.
_HOST_NEVER_SENDS = {
    # OTA is driven entirely over HTTP (python/bugbuster/ota.py); the BBP
    # opcode exists so the ID stays reserved.
    "OTA",
    # Sent by the transport layer during handshake teardown, not by a client
    # method.
    "DISCONNECT",
}

# constants.py spells a few opcodes differently from the firmware macro.
_ALIASES = {
    "SET_CHANNEL_FUNC":  "SET_CH_FUNC",
    "SET_CURRENT_LIMIT": "SET_ILIMIT",
    "SET_AVDD_SELECT":   "SET_AVDD_SEL",
    "REGISTER_READ":     "REG_READ",
    "REGISTER_WRITE":    "REG_WRITE",
    "HAT_SET_IO_VOLT":   "HAT_SET_IO_VOLTAGE",
    "CLEAR_CHAN_ALERT":  "CLEAR_CH_ALERT",
}


def _firmware_opcodes() -> dict[str, int]:
    return {
        name: int(val, 16)
        for name, val in re.findall(
            r"#define\s+BBP_CMD_(\w+)\s+0x([0-9A-Fa-f]+)", BBP_H)
    }


def test_every_python_cmdid_exists_in_the_firmware():
    """A host-only opcode is a command the device will reject as INVALID_CMD."""
    fw = _firmware_opcodes()
    missing = []
    for cmd in CmdId:
        name = _ALIASES.get(cmd.name, cmd.name)
        if name in _EVENT_CMDS:
            continue
        if name not in fw:
            missing.append(f"{cmd.name} (0x{int(cmd):02X})")
    assert not missing, (
        "CmdId entries with no BBP_CMD_* in bbp.h — the device would reject "
        f"these: {sorted(missing)}")


def test_opcode_values_agree_between_firmware_and_python():
    fw = _firmware_opcodes()
    mismatched = []
    for cmd in CmdId:
        name = _ALIASES.get(cmd.name, cmd.name)
        if name in fw and fw[name] != int(cmd):
            mismatched.append(
                f"{cmd.name}: python 0x{int(cmd):02X} vs firmware 0x{fw[name]:02X}")
    assert not mismatched, (
        "opcode value divergence — these route to the wrong handler: "
        f"{sorted(mismatched)}")


def test_no_two_firmware_commands_share_an_opcode():
    """Two commands on one byte are indistinguishable on the wire."""
    fw = _firmware_opcodes()
    by_value: dict[int, list[str]] = {}
    for name, val in fw.items():
        by_value.setdefault(val, []).append(name)
    dupes = {f"0x{v:02X}": sorted(n) for v, n in by_value.items() if len(n) > 1}
    assert not dupes, f"duplicate BBP opcodes: {dupes}"


def test_every_command_is_reachable_from_the_python_client():
    """An opcode no client method sends is a feature the library cannot use.

    Catches the STOP_SCOPE_STREAM class of bug: everything defined, nothing
    wired, and no other test notices because the command simply never appears.
    """
    sources = CLIENT_PY + DAQ_CONFIG_PY + OTA_PY
    used = set(re.findall(r"CmdId\.(\w+)", sources))

    unreachable = [
        cmd.name for cmd in CmdId
        if cmd.name not in used
        and cmd.name not in _EVENT_CMDS
        and cmd.name not in _HOST_NEVER_SENDS
    ]
    assert not unreachable, (
        "these BBP commands are defined but no bugbuster client method ever "
        f"sends them: {sorted(unreachable)}. Wire them up — an unreachable "
        "command is a half-landed feature.")


def test_exemption_list_has_no_stale_entries():
    """If an exempted command gets wired up, tighten the guard."""
    sources = CLIENT_PY + DAQ_CONFIG_PY + OTA_PY
    used = set(re.findall(r"CmdId\.(\w+)", sources))
    stale = sorted(_HOST_NEVER_SENDS & used)
    assert not stale, (
        f"{stale} are now sent by the client — remove them from "
        "_HOST_NEVER_SENDS so the guard covers them")


def test_event_commands_are_subscribed_not_sent():
    """Events must go through on_event(); sending one is a protocol error."""
    for name in _EVENT_CMDS:
        if name not in {c.name for c in CmdId}:
            continue
        assert not re.search(rf"_usb_cmd\(\s*CmdId\.{name}\b", CLIENT_PY), (
            f"{name} is a device-to-host event but the client sends it as a command")
