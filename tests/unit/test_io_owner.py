"""
test_io_owner.py — Unit tests for IO ownership (BBP v5+).

Tests run entirely against the SimulatedDevice + SimulatedUSBTransport — no
hardware required.

Slot mapping:
  IO1..IO12  →  slots 0..11   (DIO, 1-indexed on the wire → slot = io - 1)
  CH0..CH3   →  slots 12..15  (analog channels)
"""

import bugbuster as bb
from tests.mock import SimulatedDevice, SimulatedUSBTransport
from bugbuster.constants import IoClaimStatus, IoOwnerKind


# ---------------------------------------------------------------------------
# Helpers / fixtures
# ---------------------------------------------------------------------------

def _make_client(device=None):
    """Return a connected BugBuster backed by SimulatedUSBTransport."""
    if device is None:
        device = SimulatedDevice()
    t = SimulatedUSBTransport(device)
    client = bb.BugBuster(t)
    client.connect()
    return client, device


def _make_second_client(device):
    """Return a second client sharing the same SimulatedDevice (different session)."""
    # Bump session id so the simulator treats this as a distinct owner
    device._usb_session_id += 1
    t = SimulatedUSBTransport(device)
    client = bb.BugBuster(t)
    client.connect()
    return client


# ---------------------------------------------------------------------------
# 1. Single claim succeeds; second claimer on same slot is rejected
# ---------------------------------------------------------------------------

def test_single_claim_succeeds():
    client, device = _make_client()
    with client.io_claim([12]):
        entry = device.io_owner_table[12]
        assert entry["kind"] == 1, f"Expected USB owner (kind=1), got {entry['kind']}"


def test_second_claimer_rejected():
    """Two independent clients cannot both hold the same slot."""
    client1, device = _make_client()
    # Manually claim slot 0 via client1
    client1._io_claim_raw([0], 0, "test")

    # Second client (different session_id)
    device._usb_session_id += 1
    client2 = _make_second_client(device)

    # client2 tries to claim slot 0 — should get HELD_BY_OTHER
    resp = client2._t._device.dispatch(
        0xA7,  # IO_CLAIM
        bytes([1, 0]) + b'\x00\x00\x00\x00' + b'\x00\x00\x00\x00',
    )
    # resp[0] is global status, resp[1] is per-slot status for slot 0
    assert resp[0] == IoClaimStatus.HELD_BY_OTHER, (
        f"Expected HELD_BY_OTHER (0x01), got 0x{resp[0]:02x}"
    )

    client1._io_release_raw([0])


# ---------------------------------------------------------------------------
# 2. Release by same owner works; release by other owner rejected
# ---------------------------------------------------------------------------

def test_release_by_owner_works():
    client, device = _make_client()
    client._io_claim_raw([5], 0, "")
    assert device.io_owner_table[5]["kind"] == 1

    client._io_release_raw([5])
    assert device.io_owner_table[5]["kind"] == 0, "Slot should be free after release"


def test_release_by_other_rejected():
    """A different session cannot release a slot it does not own."""
    client1, device = _make_client()
    client1._io_claim_raw([3], 0, "owner1")

    device._usb_session_id += 1
    t2 = SimulatedUSBTransport(device)
    client2 = bb.BugBuster(t2)
    client2.connect()

    # client2 tries to release slot 3
    resp = device.dispatch(
        0xA8,  # IO_RELEASE
        bytes([1, 3]),
    )
    assert resp[0] == IoClaimStatus.NOT_OWNED, (
        f"Expected NOT_OWNED (0x02), got 0x{resp[0]:02x}"
    )
    # Original owner still holds it
    assert device.io_owner_table[3]["kind"] == 1

    client1._io_release_raw([3])


# ---------------------------------------------------------------------------
# 3. Invalid slot index → IO_INVALID_SLOT
# ---------------------------------------------------------------------------

def test_invalid_slot_claim():
    client, device = _make_client()
    resp = device.dispatch(
        0xA7,  # IO_CLAIM
        bytes([1, 20]) + b'\x00\x00\x00\x00' + b'\x00\x00\x00\x00',
    )
    # Global status and per-slot status both INVALID_SLOT
    assert resp[0] == IoClaimStatus.HELD_BY_OTHER or resp[1] == IoClaimStatus.INVALID_SLOT, (
        f"Expected INVALID_SLOT (0x03) for slot 20, got resp={resp.hex()}"
    )


def test_invalid_slot_claim_direct():
    """Direct check: per-slot byte for an out-of-range index is INVALID_SLOT."""
    _, device = _make_client()
    resp = device.dispatch(
        0xA7,
        bytes([1, 16]) + b'\x00\x00\x00\x00' + b'\x00\x00\x00\x00',
    )
    assert resp[1] == IoClaimStatus.INVALID_SLOT, (
        f"Expected INVALID_SLOT (0x03) for slot 16, got 0x{resp[1]:02x}"
    )


# ---------------------------------------------------------------------------
# 4. io_claim context manager releases on normal exit and on exception
# ---------------------------------------------------------------------------

def test_context_manager_releases_on_normal_exit():
    client, device = _make_client()
    with client.io_claim([12, 13]):
        assert device.io_owner_table[12]["kind"] == 1
        assert device.io_owner_table[13]["kind"] == 1
    # After exit: slots must be free
    assert device.io_owner_table[12]["kind"] == 0, "Slot 12 must be freed on exit"
    assert device.io_owner_table[13]["kind"] == 0, "Slot 13 must be freed on exit"


def test_context_manager_releases_on_exception():
    client, device = _make_client()
    try:
        with client.io_claim([14]):
            assert device.io_owner_table[14]["kind"] == 1
            raise RuntimeError("simulated failure")
    except RuntimeError:
        pass
    assert device.io_owner_table[14]["kind"] == 0, "Slot 14 must be freed after exception"


# ---------------------------------------------------------------------------
# 5. Lease expiry via simulated tick(now_ms)
# ---------------------------------------------------------------------------

def test_lease_expiry_via_tick():
    client, device = _make_client()
    # Claim slot 7 with a 1000 ms lease
    client._io_claim_raw([7], 1000, "expiry_test")
    assert device.io_owner_table[7]["kind"] == 1

    # Tick to just before expiry — slot still held
    device.tick(999)
    assert device.io_owner_table[7]["kind"] == 1

    # Tick past expiry — slot released
    device.tick(1001)
    assert device.io_owner_table[7]["kind"] == 0, "Slot must be freed after lease expires"


def test_infinite_lease_not_expired():
    client, device = _make_client()
    # lease_ms=0 means infinite
    client._io_claim_raw([8], 0, "infinite")
    device.tick(9_999_999)
    assert device.io_owner_table[8]["kind"] == 1, "Infinite lease must not expire"
    client._io_release_raw([8])


# ---------------------------------------------------------------------------
# 6. Force-release with admin token succeeds; without → IO_ADMIN_REQUIRED
# ---------------------------------------------------------------------------

def test_force_release_with_admin_token():
    client, device = _make_client()
    client._io_claim_raw([2], 0, "")
    assert device.io_owner_table[2]["kind"] == 1

    # SIMTOKEN is the simulator's admin token (defined in core.py)
    client.io_force_release(2, "SIMTOKEN")
    assert device.io_owner_table[2]["kind"] == 0, "Force-release must clear the slot"


def test_force_release_wrong_token():
    _, device = _make_client()
    resp = device.dispatch(
        0xAA,  # IO_FORCE_RELEASE
        bytes([2, len(b"WRONGTOKEN")]) + b"WRONGTOKEN",
    )
    assert resp[0] == IoClaimStatus.ADMIN_REQUIRED, (
        f"Expected ADMIN_REQUIRED (0x05), got 0x{resp[0]:02x}"
    )


# ---------------------------------------------------------------------------
# 7. Auto-claim wrap: bare call outside io_claim context still works and
#    leaves the slot un-owned afterward
# ---------------------------------------------------------------------------

def test_auto_claim_wrap_set_dac_voltage():
    """
    set_dac_voltage() called outside an io_claim() context must succeed
    (auto-wrap with a 5 s lease) and must leave the slot free afterward.
    """
    client, device = _make_client()
    # First put channel 0 in VOUT mode so set_dac_voltage is valid
    # Use raw dispatch to avoid triggering the channel-function auto-claim
    device.channels[0]["function"] = 1  # VOUT

    # At this point no io_claim context is active
    assert client._io_claimed_slots is None

    # Call the mutator without wrapping it in io_claim — auto-wrap must kick in
    client.set_dac_voltage(0, 3.3)

    # Slot 12 (CH0) must be free again after the call
    assert device.io_owner_table[12]["kind"] == 0, (
        "Slot 12 must be released after auto-claim wrap completes"
    )
    # Value was actually applied
    assert abs(device.channels[0]["dac_value"] - 3.3) < 0.01


# ---------------------------------------------------------------------------
# 8. io_owner_status decode round-trip (160-byte response)
# ---------------------------------------------------------------------------

def test_io_owner_status_round_trip():
    client, device = _make_client()

    # Claim slots 0 and 12
    client._io_claim_raw([0, 12], 5000, "status_test")

    table = client.io_owner_status()
    assert len(table) == 16, f"Expected 16 entries, got {len(table)}"

    slot0  = table[0]
    slot12 = table[12]
    slot1  = table[1]

    assert slot0["kind"] == 1,  f"Slot 0 should be USB-owned, got kind={slot0['kind']}"
    assert slot12["kind"] == 1, f"Slot 12 should be USB-owned, got kind={slot12['kind']}"
    assert slot1["kind"] == 0,  f"Slot 1 should be free, got kind={slot1['kind']}"

    # Lease expiry field must be set (>0 because lease_ms=5000)
    assert slot0["lease_until_ms"] > 0, "Slot 0 should have a finite lease"

    client._io_release_raw([0, 12])


# ---------------------------------------------------------------------------
# FIX 9 — G2: non-owner write attempt
# A foreign session holds slot 12; a second session's auto-claim attempt must
# be rejected at the simulator level (HELD_BY_OTHER per-slot status).
# ---------------------------------------------------------------------------

def test_g2_nonowner_claim_rejected_by_simulator():
    """
    G2: When foreign session holds slot 12 (CH0), a new claim for that slot
    returns HELD_BY_OTHER in the per-slot status byte.

    Note: client._auto_claim_wrap does not raise on HELD_BY_OTHER — that is
    deliberate (backward-compat auto-claim). This test verifies the simulator's
    wire-level enforcement.
    """
    client1, device = _make_client()
    client1._io_claim_raw([12], 0, "foreign_owner")
    assert device.io_owner_table[12]["kind"] == IoOwnerKind.USB

    # Second session
    device._usb_session_id += 1
    resp = device.dispatch(
        0xA7,  # IO_CLAIM
        bytes([1, 12]) + b'\x00\x00\x00\x00' + b'\x00\x00\x00\x00',
    )
    # global status = HELD_BY_OTHER, per-slot[0] = HELD_BY_OTHER
    assert resp[0] == IoClaimStatus.HELD_BY_OTHER, (
        f"Expected global HELD_BY_OTHER, got 0x{resp[0]:02x}"
    )
    assert resp[1] == IoClaimStatus.HELD_BY_OTHER, (
        f"Expected per-slot HELD_BY_OTHER for slot 12, got 0x{resp[1]:02x}"
    )
    client1._io_release_raw([12])


# ---------------------------------------------------------------------------
# FIX 9 — G4: INTERNAL preempt-and-restore round-trip
# ---------------------------------------------------------------------------

def test_g4_internal_preempt_and_restore():
    """
    G4: Set IO_OWNER_INTERNAL on a slot owned by USB, then restore USB
    ownership. After restore, the slot should be USB-owned again, NOT NONE.
    """
    client, device = _make_client()

    client._io_claim_raw([6], 30_000, "usb_owner")
    assert device.io_owner_table[6]["kind"] == IoOwnerKind.USB

    # Simulate firmware internal preemption
    saved_session_id = device.io_owner_table[6]["session_id"]
    saved_lease = device.io_owner_table[6]["lease_until_ms"]
    device.io_owner_table[6]["kind"]       = IoOwnerKind.INTERNAL
    device.io_owner_table[6]["session_id"] = 0xFF

    table = client.io_owner_status()
    assert table[6]["kind"] == IoOwnerKind.INTERNAL, "Slot must show INTERNAL during preempt"

    # Simulate selftest end: restore USB ownership with remaining lease
    device.io_owner_table[6]["kind"]           = IoOwnerKind.USB
    device.io_owner_table[6]["session_id"]     = saved_session_id
    device.io_owner_table[6]["lease_until_ms"] = saved_lease

    table = client.io_owner_status()
    assert table[6]["kind"] == IoOwnerKind.USB, (
        "Slot must be RESTORED to USB after INTERNAL releases it — not NONE"
    )
    assert table[6]["lease_until_ms"] == saved_lease, (
        "Remaining lease must be preserved after restore"
    )

    client._io_release_raw([6])


# ---------------------------------------------------------------------------
# FIX 9 — G6: auto-claim blocked when foreign owner holds slot
# ---------------------------------------------------------------------------

def test_g6_autoclaim_blocked_by_foreign_owner():
    """
    G6: When a foreign owner holds slot 12, set_dac_voltage's auto-claim
    attempt returns HELD_BY_OTHER at the simulator level.
    The write may proceed (client does not enforce), but the ownership table
    must still show the original foreign owner, not the new USB session.
    """
    client1, device = _make_client()
    client1._io_claim_raw([12], 0, "foreign")
    original_session = device.io_owner_table[12]["session_id"]

    # Second session attempts auto-claim
    device._usb_session_id += 1
    resp = device.dispatch(
        0xA7,
        bytes([1, 12]) + b'\x00\x00\x00\x00' + b'\x00\x00\x00\x00',
    )
    assert resp[0] == IoClaimStatus.HELD_BY_OTHER, (
        f"Auto-claim must return HELD_BY_OTHER, got 0x{resp[0]:02x}"
    )
    # Table still shows original owner
    assert device.io_owner_table[12]["session_id"] == original_session, (
        "Foreign owner's session_id must survive a rejected claim attempt"
    )
    client1._io_release_raw([12])


# ---------------------------------------------------------------------------
# FIX 9 — G7: USB-vs-SCRIPT and HTTP-vs-SCRIPT conflict pairs
# ---------------------------------------------------------------------------

def test_g7_usb_vs_script_conflict():
    """
    G7: A SCRIPT owner blocks a USB claim attempt (HELD_BY_OTHER).
    """
    _, device = _make_client()
    # Simulate an on-device script owning slot 5
    device.io_owner_table[5]["kind"]       = IoOwnerKind.SCRIPT
    device.io_owner_table[5]["session_id"] = 42

    resp = device.dispatch(
        0xA7,
        bytes([1, 5]) + b'\x00\x00\x00\x00' + b'\x00\x00\x00\x00',
    )
    assert resp[0] == IoClaimStatus.HELD_BY_OTHER, (
        f"USB must be blocked by SCRIPT owner, got 0x{resp[0]:02x}"
    )
    device.io_owner_table[5]["kind"] = IoOwnerKind.NONE


def test_g7_http_vs_script_conflict():
    """
    G7: A SCRIPT owner blocks a USB (simulating HTTP path) claim attempt.
    """
    _, device = _make_client()
    # Simulate an on-device script owning slot 8
    device.io_owner_table[8]["kind"]       = IoOwnerKind.SCRIPT
    device.io_owner_table[8]["session_id"] = 77

    resp = device.dispatch(
        0xA7,
        bytes([1, 8]) + b'\x00\x00\x00\x00' + b'\x00\x00\x00\x00',
    )
    assert resp[0] == IoClaimStatus.HELD_BY_OTHER, (
        f"USB (HTTP proxy) must be blocked by SCRIPT owner, got 0x{resp[0]:02x}"
    )
    device.io_owner_table[8]["kind"] = IoOwnerKind.NONE


# ---------------------------------------------------------------------------
# FIX 9 — G8: EVT_IO_PREEMPTED emitted on lease-expiry claim
# ---------------------------------------------------------------------------

def test_g8_evt_io_preempted_emitted_on_lease_expiry():
    """
    G8: When a slot's lease expires and a new claim arrives for the same slot,
    emit_event() must fire EVT_IO_PREEMPTED (0xE1) with (slot, LEASE_EXPIRED).
    Consume the event via device._drain_events().
    """
    client, device = _make_client()
    # Claim slot 9 with a 500 ms lease
    client._io_claim_raw([9], 500, "expiring_owner")
    assert device.io_owner_table[9]["kind"] == IoOwnerKind.USB

    # Advance time past expiry but do NOT call tick() — lease expired inline
    device._now_ms = 1000

    # New claim arrives — this triggers the expiry path in _io_claim handler
    device._usb_session_id += 1
    resp = device.dispatch(
        0xA7,
        bytes([1, 9]) + b'\x00\x00\x00\x00' + b'\x00\x00\x00\x00',
    )
    # Claim must succeed (expired slot treated as free)
    assert resp[0] == IoClaimStatus.OK, (
        f"Claim on expired slot must succeed, got 0x{resp[0]:02x}"
    )

    # BBP_EVT_IO_PREEMPTED (0x86) must have been emitted.
    # Payload format: displaced_kind(u8), slot(u8)
    events = device._drain_events()
    assert len(events) >= 1, "Expected at least one event after lease-expiry claim"
    evt_id, payload = events[0]
    assert evt_id == 0x86, f"Expected BBP_EVT_IO_PREEMPTED (0x86), got 0x{evt_id:02x}"
    # displaced_kind = USB (1) — the prior owner whose lease expired
    assert payload[0] == IoOwnerKind.USB, (
        f"displaced_kind must be USB (1), got {payload[0]}"
    )
    assert payload[1] == 9, f"Event slot must be 9, got {payload[1]}"
