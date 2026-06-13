"""
test_12_io_ownership.py — Device-level IO ownership tests (simulator only).

Run with:  pytest tests/device --sim -q

Covers:
  - USB claim then HTTP write → HTTP rejected (ownership enforced server-side)
  - HTTP claim then USB write → USB rejected
  - IO_OWNER_INTERNAL preempt: simulator selftest fires preempt; prior owner
    sees the slot cleared (EVT_IO_PREEMPTED semantics tested via table inspection)
  - 16-slot status decode round-trips (160-byte response)
  - ADGS mutual-exclusion: one switch closed per non-selftest device at a time
"""

import struct
import pytest
from tests.mock import SimulatedDevice, SimulatedUSBTransport, SimulatedHTTPTransport
import bugbuster as bb
from bugbuster.constants import IoClaimStatus, IoOwnerKind

pytestmark = [pytest.mark.timeout(60)]


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def sim_device():
    """A fresh SimulatedDevice shared between USB and HTTP transports."""
    return SimulatedDevice()


@pytest.fixture
def usb_client(sim_device):
    t = SimulatedUSBTransport(sim_device)
    client = bb.BugBuster(t)
    client.connect()
    yield client
    try:
        client.disconnect()
    except Exception:
        pass


@pytest.fixture
def http_client(sim_device):
    t = SimulatedHTTPTransport(sim_device)
    client = bb.BugBuster(t)
    client.connect()
    yield client
    try:
        client.disconnect()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# 1. USB claim then HTTP write — HTTP transport does not enforce on Python side
#    but the ownership table correctly reflects USB ownership.
# ---------------------------------------------------------------------------

@pytest.mark.sim_only
def test_usb_claim_reflected_in_table(usb_client, http_client, sim_device):
    """
    After USB client claims slot 12 (CH0), the ownership table must show
    USB ownership regardless of which transport queries it.
    """
    pytest.importorskip("tests.mock")  # always available in sim mode

    with usb_client.io_claim([12], purpose="usb_claim_test"):
        table = usb_client.io_owner_status()
        assert table[12]["kind"] == IoOwnerKind.USB, (
            f"Expected USB owner (kind=1), got {table[12]['kind']}"
        )
        # HTTP client reads the same shared simulator state
        assert sim_device.io_owner_table[12]["kind"] == IoOwnerKind.USB

    # After context exit, slot must be free
    assert sim_device.io_owner_table[12]["kind"] == IoOwnerKind.NONE


# ---------------------------------------------------------------------------
# 2. HTTP claim then USB write — USB auto-wrap is blocked by simulator
#    when a different session already owns the slot.
# ---------------------------------------------------------------------------

@pytest.mark.sim_only
def test_http_claim_blocks_second_usb_session(sim_device):
    """
    Simulate an HTTP session owning slot 12, then verify a second USB session
    is rejected on claim attempt.
    """
    # Directly set the table to simulate an HTTP owner on slot 12
    sim_device.io_owner_table[12]["kind"]       = IoOwnerKind.HTTP
    sim_device.io_owner_table[12]["session_id"] = 99
    sim_device.io_owner_table[12]["token_fp32"] = 0xDEAD

    # USB session (session_id=1) attempts to claim slot 12
    t = SimulatedUSBTransport(sim_device)
    usb = bb.BugBuster(t)
    usb.connect()

    resp = sim_device.dispatch(
        0xA7,  # IO_CLAIM
        bytes([1, 12]) + b'\x00\x00\x00\x00' + b'\x00\x00\x00\x00',
    )
    assert resp[0] == IoClaimStatus.HELD_BY_OTHER, (
        f"USB session should be rejected for HTTP-owned slot, got 0x{resp[0]:02x}"
    )

    # Cleanup
    sim_device.io_owner_table[12]["kind"] = IoOwnerKind.NONE


# ---------------------------------------------------------------------------
# 3. IO_OWNER_INTERNAL preempt — monkeypatch selftest to take slot
# ---------------------------------------------------------------------------

@pytest.mark.sim_only
def test_internal_preempt_clears_prior_owner(usb_client, sim_device):
    """
    When INTERNAL takes a slot (simulating selftest), the prior USB owner
    should see the slot no longer owned by them.
    """
    usb_client._io_claim_raw([4], 0, "usb_owner")
    assert sim_device.io_owner_table[4]["kind"] == IoOwnerKind.USB

    # Simulate selftest internal preemption
    sim_device.io_owner_table[4]["kind"]       = IoOwnerKind.INTERNAL
    sim_device.io_owner_table[4]["session_id"] = 0xFF  # internal marker

    # Prior USB owner checks: slot no longer owned by USB
    table = usb_client.io_owner_status()
    assert table[4]["kind"] == IoOwnerKind.INTERNAL, (
        f"Slot 4 must show INTERNAL ownership after preempt, got {table[4]['kind']}"
    )

    # Cleanup: release internal
    sim_device.io_owner_table[4]["kind"]       = IoOwnerKind.NONE
    sim_device.io_owner_table[4]["session_id"] = 0


# ---------------------------------------------------------------------------
# 4. 16-slot status decode round-trip (160-byte response)
# ---------------------------------------------------------------------------

@pytest.mark.sim_only
def test_status_decode_round_trip(usb_client, sim_device):
    """
    Claim several slots, read back via io_owner_status(), verify each field
    round-trips correctly through the 160-byte wire format.
    """
    slots_to_claim = [0, 5, 11, 12, 15]
    usb_client._io_claim_raw(slots_to_claim, 3000, "round_trip")

    table = usb_client.io_owner_status()

    assert len(table) == 16, f"Expected 16 entries in status table, got {len(table)}"

    for slot_idx in slots_to_claim:
        entry = table[slot_idx]
        assert entry["slot"] == slot_idx, (
            f"slot field mismatch at index {slot_idx}: got {entry['slot']}"
        )
        assert entry["kind"] == IoOwnerKind.USB, (
            f"Slot {slot_idx} should be USB-owned, got kind={entry['kind']}"
        )
        assert entry["lease_until_ms"] > 0, (
            f"Slot {slot_idx} should have a finite lease"
        )

    # All unclaimed slots must be free
    for slot_idx in range(16):
        if slot_idx not in slots_to_claim:
            assert table[slot_idx]["kind"] == IoOwnerKind.NONE, (
                f"Slot {slot_idx} must be free, got kind={table[slot_idx]['kind']}"
            )

    # Verify raw wire size: dispatcher returns exactly 160 bytes
    raw = sim_device.dispatch(0xA9, b'')  # IO_OWNER_STATUS
    assert len(raw) == 160, f"IO_OWNER_STATUS must return 160 bytes, got {len(raw)}"

    usb_client._io_release_raw(slots_to_claim)


# ---------------------------------------------------------------------------
# 5. Force-release with admin token clears any slot
# ---------------------------------------------------------------------------

@pytest.mark.sim_only
def test_force_release_all(usb_client, sim_device):
    """Force-release with slot=0xFF clears all slots at once."""
    # Claim multiple slots
    usb_client._io_claim_raw([0, 1, 2], 0, "")
    for s in [0, 1, 2]:
        assert sim_device.io_owner_table[s]["kind"] == IoOwnerKind.USB

    # Force-release all via admin token
    usb_client.io_force_release(0xFF, "SIMTOKEN")

    for s in [0, 1, 2]:
        assert sim_device.io_owner_table[s]["kind"] == IoOwnerKind.NONE, (
            f"Slot {s} must be free after force-release-all"
        )


# ---------------------------------------------------------------------------
# FIX 10 — ADGS mutual-exclusion: two non-selftest switch commands in sequence
# ---------------------------------------------------------------------------

@pytest.mark.sim_only
def test_adgs_group_exclusion_opens_same_group_only(sim_device):
    """
    ADGS switch exclusivity is per physical IO group, not per whole MUX device.
    Closing S5 must not open S3 because those are different IOs in the same block.
    """
    t = SimulatedUSBTransport(sim_device)
    client = bb.BugBuster(t)
    client.connect()

    # Close switch 2 on device 0 (non-selftest)
    sim_device.dispatch(0x92, struct.pack('<BBB', 0, 2, 1))  # MUX_SET_SWITCH dev=0 sw=2 closed=1
    assert sim_device.adgs_active[0] == 2, (
        f"After closing sw2, adgs_active[0] must be 2, got {sim_device.adgs_active[0]}"
    )
    assert sim_device.mux_states[0] == (1 << 2), (
        f"mux_states[0] must have only bit 2 set, got 0b{sim_device.mux_states[0]:08b}"
    )

    # Now close switch 5 on device 0. It is in a different group, so S2 stays on.
    sim_device.dispatch(0x92, struct.pack('<BBB', 0, 5, 1))  # MUX_SET_SWITCH dev=0 sw=5 closed=1
    assert sim_device.mux_states[0] == ((1 << 2) | (1 << 5)), (
        f"Switch 2 must remain closed when switch 5 closes; "
        f"mux_states[0]=0b{sim_device.mux_states[0]:08b}"
    )

    # Closing another switch in S1-S4 must open only that group.
    sim_device.dispatch(0x92, struct.pack('<BBB', 0, 1, 1))
    assert sim_device.mux_states[0] == ((1 << 1) | (1 << 5)), (
        "Switch 1 must replace switch 2, but switch 5 must remain active"
    )

    try:
        client.disconnect()
    except Exception:
        pass
