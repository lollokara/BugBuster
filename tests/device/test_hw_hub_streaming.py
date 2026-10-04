"""Hardware tests for sub-project 6 (device to hub streaming).

Verifies the end-to-end telemetry pipeline between the BugBuster ESP32-S3 mainboard
(with P4 DAQ HAT) and the ESPFleet hub (http://192.168.3.87:8080/api/v1):
 - /api/hub/status route on the board reports discovery, url, recent push, and small backlog
 - Board is registered in the hub's device list with matching MAC id
 - MicroPython eval logs appear in the hub within 30 s, and rate limiting catches floods
 - P4 runs appear on the hub with valid coverage ranges and monotonic sample timestamps
 - /api/hub/resync route is idempotent (coverage points do not grow on subsequent syncs)
 - HAT health while streaming: /api/hat timeouts do not grow, and no mutex contention errors

All tests are read-only toward the DAQ and skip cleanly under --sim or when the hub/device is unreachable.
"""

from __future__ import annotations

from collections import Counter
import time
import uuid

import pytest

from tests.device._hub_helper import (
    get_board_hub_status,
    list_p4_runs,
    normalize_mac,
    read_p4_run_meta,
    setup_hub_test_env,
)

pytestmark = [pytest.mark.http_only, pytest.mark.timeout(120)]


@pytest.fixture(scope="module")
def hub_env(request, hub_url):
    """Set up and validate test environment (board and hub reachability)."""
    return setup_hub_test_env(request, hub_url)


def test_hub_status_reports_discovery_and_push(hub_env, hub_url):
    """The hub status route reports discovered status, matching hub URL, recent push, and small backlog."""
    session, device_base, _, _ = hub_env
    res = get_board_hub_status(session, device_base)
    assert res.get("ok") is True, f"status reply ok!=True: {res}"
    hub_info = res.get("hub", {})

    # Discovered / connected status
    discovered = hub_info.get("discovered", hub_info.get("source") not in (None, "", "none") and bool(hub_info.get("url")))
    assert discovered is True or hub_info.get("ok") is True, f"hub not discovered or not ok: {hub_info}"
    assert hub_info.get("source") != "none", f"hub URL source is none: {hub_info}"

    # URL matches configured hub
    reported_url = hub_info.get("url", "").rstrip("/").lower()
    expected_url = hub_url.rstrip("/").lower()
    assert reported_url == expected_url, f"reported hub URL {reported_url!r} != expected {expected_url!r}"

    # Recent last_push timestamp
    last_push = hub_info.get("last_push", 0)
    assert last_push > 0, f"no push has occurred yet (last_push={last_push})"
    assert abs(time.time() - last_push) < 300, f"last_push timestamp not recent: {last_push}"

    # Small backlog
    backlog = hub_info.get("backlog", 0)
    assert backlog < 100, f"hub backlog too large: {backlog}"


def test_board_appears_in_hub_device_list(hub_env):
    """The board appears in the hub's device list matching its hardware MAC id."""
    _, _, hub, mac_id = hub_env
    devices = hub.get_devices()
    assert devices, "hub device list is empty"

    matched = False
    for dev in devices:
        dev_id = normalize_mac(str(dev.get("id", "")))
        dev_mac = normalize_mac(str(dev.get("mac", "")))
        if dev_id == mac_id or dev_mac == mac_id:
            matched = True
            break

    assert matched, f"device with MAC id {mac_id} not found in hub devices: {[d.get('id') for d in devices]}"


def test_mpy_log_eval_and_rate_limiting(hub_env):
    """A marker log from /api/scripts/eval shows up in hub logs within 30 s, and rate limiting catches floods."""
    session, device_base, hub, mac_id = hub_env

    # Check that the script slot is free before attempting eval
    st_resp = session.get(f"{device_base}/api/scripts/status", timeout=8)
    if st_resp.status_code == 200:
        st = st_resp.json()
        if st.get("running", False) or st.get("state") not in ("idle", "done", "error"):
            pytest.skip(f"scripting slot is currently busy (state={st.get('state')!r}); skipping eval")
    elif st_resp.status_code == 404:
        pytest.skip("scripts API not available on device")

    marker = f"bbt_hub_marker_{uuid.uuid4().hex}"
    script = f"print('{marker}')\n"
    eval_resp = session.post(
        f"{device_base}/api/scripts/eval",
        data=script.encode("utf-8"),
        headers={"Content-Type": "text/x-python"},
        timeout=10,
    )
    assert eval_resp.status_code == 200, f"eval failed with HTTP {eval_resp.status_code}: {eval_resp.text}"

    # Poll hub logs for the marker within 30 seconds
    deadline = time.time() + 30.0
    found_marker = False
    while time.time() < deadline:
        logs = hub.get_logs(device=mac_id, source="mpy", from_ts=time.time() - 60, limit=200)
        for entry in logs:
            if marker in entry.get("msg", ""):
                found_marker = True
                assert entry.get("source") == "mpy", f"expected source 'mpy', got {entry.get('source')!r}"
                break
        if found_marker:
            break
        time.sleep(1.5)

    assert found_marker, f"marker {marker} did not appear in hub logs within 30 s"

    # Rate limiting verification: in a 60 s window, no single tag should exceed rate limits (catches floods)
    recent_logs = hub.get_logs(device=mac_id, from_ts=time.time() - 60, limit=500)
    tag_counts = Counter(entry.get("tag", "") for entry in recent_logs)
    for tag, count in tag_counts.items():
        assert count <= 60, f"tag {tag!r} exceeded rate limit with {count} messages in 60 s (flood detected)"


def test_p4_runs_appear_on_hub_with_valid_coverage(hub_env):
    """Every P4 run from LIST_RUNS appears on the hub; coverage is non-empty and non-overlapping; samples are monotonic."""
    session, device_base, hub, mac_id = hub_env

    try:
        run_ids, _ = list_p4_runs(session, device_base)
    except Exception as exc:
        pytest.skip(f"cannot query P4 run list: {exc}")

    # Safety: never touch run 2
    test_runs = [rid for rid in run_ids if rid != 2]
    if not test_runs:
        pytest.skip("no P4 runs available on board to verify (excluding run 2)")

    verified_runs = 0
    for run_id in test_runs:
        meta = read_p4_run_meta(session, device_base, run_id)
        if meta is None or meta.created_epoch == 0:
            # Run clock was not set when created; not syncable to hub
            continue

        run_uid = f"{mac_id}-{run_id}-{meta.created_epoch}"
        hub_run = hub.get_run(run_uid)
        assert hub_run is not None, f"run {run_uid} from board not found on hub"
        assert hub_run.get("run_id") == run_id, f"run_id mismatch: {hub_run.get('run_id')} != {run_id}"

        # Coverage inspection
        cov = hub.get_coverage(run_uid)
        ranges = cov.get("ranges", [])
        assert ranges, f"coverage for run {run_uid} is empty"

        # Check for no overlapping ranges at the same resolution
        by_res: dict[int, list[dict]] = {}
        for r in ranges:
            by_res.setdefault(r["res"], []).append(r)

        for res, res_ranges in by_res.items():
            sorted_ranges = sorted(res_ranges, key=lambda x: x["from"])
            for i in range(len(sorted_ranges) - 1):
                cur_r = sorted_ranges[i]
                next_r = sorted_ranges[i + 1]
                assert cur_r["to"] <= next_r["from"], (
                    f"overlapping coverage ranges at res {res}: [{cur_r['from']}, {cur_r['to']}] "
                    f"and [{next_r['from']}, {next_r['to']}]"
                )

        # Sample monotonicity check
        samples = hub.get_samples(run_uid)
        t_samples = [t for t in samples.get("t", []) if t is not None]
        for i in range(len(t_samples) - 1):
            assert t_samples[i] <= t_samples[i + 1], (
                f"non-monotonic sample timestamps in run {run_uid}: t[{i}]={t_samples[i]} > t[{i+1}]={t_samples[i+1]}"
            )

        verified_runs += 1

    if verified_runs == 0:
        pytest.skip("no synced runs with valid created_epoch found to verify")


def test_hub_resync_route_is_idempotent(hub_env):
    """POST /api/hub/resync is idempotent: coverage point counts do not grow on a second sync pass."""
    session, device_base, hub, mac_id = hub_env

    hub_runs = hub.get_runs(device=mac_id)
    # Never touch run 2
    eligible_runs = [r for r in hub_runs if r.get("run_id") != 2]
    if not eligible_runs:
        pytest.skip("no runs on hub to test resync idempotency")

    target_uid = eligible_runs[0]["run_uid"]
    cov_before = hub.get_coverage(target_uid)
    points_before = sum(r.get("points", 0) for r in cov_before.get("ranges", []))
    assert points_before > 0, f"target run {target_uid} has 0 coverage points before resync"

    # Trigger resync route on the board
    resync_resp = session.post(f"{device_base}/api/hub/resync", timeout=10)
    if resync_resp.status_code == 404:
        pytest.skip("/api/hub/resync endpoint does not exist on device")
    assert resync_resp.status_code == 200, f"resync failed with HTTP {resync_resp.status_code}: {resync_resp.text}"
    assert resync_resp.json().get("ok") is True, f"resync response ok!=True: {resync_resp.json()}"

    # Wait for the device to complete coverage re-check
    time.sleep(5.0)

    cov_after = hub.get_coverage(target_uid)
    points_after = sum(r.get("points", 0) for r in cov_after.get("ranges", []))
    assert points_after == points_before, (
        f"coverage point count grew on second sync: {points_before} -> {points_after} (not idempotent)"
    )


@pytest.mark.timeout(90)
def test_hat_health_while_streaming(hub_env):
    """HAT health while streaming: /api/hat timeouts do not grow, and hub logs show no 'failed to take mutex' over 60 s."""
    session, device_base, hub, mac_id = hub_env

    hat_resp = session.get(f"{device_base}/api/hat", timeout=8)
    if hat_resp.status_code != 200:
        pytest.skip(f"/api/hat returned HTTP {hat_resp.status_code}")
    initial_hat = hat_resp.json()
    if not initial_hat.get("connected", False):
        pytest.skip("HAT expansion board not connected")

    initial_timeouts = initial_hat.get("consecutiveTimeouts", 0)
    initial_last_timeout_ms = initial_hat.get("lastTimeoutMs", 0)
    t_start = time.time()

    # Observe HAT status over 60 seconds (sampling every 10 seconds)
    for _ in range(6):
        time.sleep(10.0)
        curr_resp = session.get(f"{device_base}/api/hat", timeout=8)
        assert curr_resp.status_code == 200, f"/api/hat failed during monitoring: {curr_resp.status_code}"
        curr_hat = curr_resp.json()

        # Timeouts must not grow
        curr_timeouts = curr_hat.get("consecutiveTimeouts", 0)
        assert curr_timeouts <= initial_timeouts, (
            f"consecutiveTimeouts grew during streaming: {curr_timeouts} > {initial_timeouts}"
        )
        assert curr_hat.get("lastTimeoutMs", 0) == initial_last_timeout_ms, (
            "new HAT command timeout occurred during streaming"
        )

    # Verify hub logs for the device show no mutex acquisition contention
    window_logs = hub.get_logs(device=mac_id, from_ts=t_start - 60, limit=500)
    mutex_errors = [
        entry for entry in window_logs
        if "failed to take mutex" in entry.get("msg", "").lower()
    ]
    assert not mutex_errors, f"found mutex acquisition failures in hub logs: {mutex_errors}"
