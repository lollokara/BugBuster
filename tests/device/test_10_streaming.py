"""
test_10_streaming.py — ADC and scope data streaming tests (USB only).

The firmware pushes ADC_DATA_EVT events at up to 9.6 kSPS per channel.
SCOPE_DATA_EVT events contain pre-computed min/max/avg per channel every 10 ms.

All tests in this file require USB transport.
HTTP transport does not support streaming — that is tested separately.
"""

import time
import threading
import pytest
from conftest import assert_no_faults

pytestmark = [
    pytest.mark.usb_only,
    pytest.mark.streaming,
    pytest.mark.timeout(30),
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _collect_adc_events(dev, channels, n_events, timeout_s=5.0):
    """
    Start ADC stream, collect *n_events* callbacks, stop and return data.
    Returns list of (mask, samples) tuples.
    """
    events = []
    event_ready = threading.Event()

    def on_data(mask, samples):
        events.append((mask, samples))
        if len(events) >= n_events:
            event_ready.set()

    dev.start_adc_stream(channels, divider=1, callback=on_data)
    event_ready.wait(timeout=timeout_s)
    dev.stop_adc_stream()
    return events


# ---------------------------------------------------------------------------
# ADC streaming start/stop
# ---------------------------------------------------------------------------

def test_adc_stream_starts_and_stops(usb_device):
    """
    Start ADC streaming on all 4 channels, wait for at least 5 events,
    then stop.  Verifies the stream can be started and stopped cleanly.
    """
    events = _collect_adc_events(usb_device, [0, 1, 2, 3], n_events=5, timeout_s=5.0)

    assert len(events) >= 1, (
        f"Expected at least 1 ADC stream event within 5 s, got {len(events)}"
    )
    assert_no_faults(usb_device)


def test_adc_stream_events_have_data(usb_device):
    """
    Each ADC stream event should contain at least one sample per active channel.
    The callback receives (channel_mask, samples) where samples is a list of
    raw 24-bit ADC codes.
    """
    events = _collect_adc_events(usb_device, [0, 1], n_events=3, timeout_s=5.0)

    assert len(events) >= 1, "Expected at least 1 ADC event"

    for mask, samples in events:
        assert isinstance(mask, int), f"mask must be int, got {type(mask)}"
        assert isinstance(samples, list), f"samples must be list, got {type(samples)}"
        assert len(samples) >= 1, "Each event must have at least 1 sample"
        for s in samples:
            assert isinstance(s, int), f"Each sample must be int (raw code), got {type(s)}"
            assert 0 <= s <= 0xFFFFFF, f"24-bit ADC sample out of range: {s:#08x}"
    assert_no_faults(usb_device)


def test_adc_stream_mask_matches_channels(usb_device):
    """
    When streaming channels [0, 1], the event mask should have bits 0 and 1 set.
    """
    events = _collect_adc_events(usb_device, [0, 1], n_events=3, timeout_s=5.0)

    expected_mask = (1 << 0) | (1 << 1)
    for mask, _ in events:
        assert mask & expected_mask, (
            f"Event mask {mask:#04x} does not include expected channels "
            f"(expected bits {expected_mask:#04x})"
        )
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# ADC stream sample rate
# ---------------------------------------------------------------------------

@pytest.mark.slow
def test_adc_stream_sample_rate(usb_device):
    """
    Measure actual ADC events received over 1 second.
    With default settings, we expect at least 5 events per second
    (conservative — actual rate depends on firmware ADC configuration).
    """
    events = []
    start = time.monotonic()

    def on_data(mask, samples):
        events.append(time.monotonic())

    usb_device.start_adc_stream([0], divider=1, callback=on_data)
    time.sleep(1.0)
    usb_device.stop_adc_stream()

    elapsed = time.monotonic() - start
    rate = len(events) / elapsed

    assert rate >= 5.0, (
        f"ADC event rate too low: {rate:.1f} events/s (expected >= 5)"
    )
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Scope data (on_scope_data events)
# ---------------------------------------------------------------------------

def test_scope_stream_starts_and_stops(usb_device):
    """
    Register a scope data callback and verify at least 1 SCOPE_DATA_EVT
    is received within 500 ms.  The firmware sends scope buckets every 10 ms.
    """
    events = []
    event_ready = threading.Event()

    def on_scope(data):
        events.append(data)
        if len(events) >= 1:
            event_ready.set()

    usb_device.on_scope_data(on_scope)
    event_ready.wait(timeout=0.5)

    assert len(events) >= 1, (
        "Expected at least 1 scope event within 500 ms (scope pushes every ~10 ms)"
    )
    assert_no_faults(usb_device)


def test_scope_data_format(usb_device):
    """
    Each scope data event should be a dict with:
      seq, timestamp_ms, count, channels (list of 4 dicts with avg/min/max).
    """
    events = []
    event_ready = threading.Event()

    def on_scope(data):
        events.append(data)
        if len(events) >= 3:
            event_ready.set()

    usb_device.on_scope_data(on_scope)
    event_ready.wait(timeout=0.5)

    assert len(events) >= 1, "No scope events received"

    for ev in events:
        assert isinstance(ev, dict), f"Scope event must be dict, got {type(ev)}"
        assert "seq" in ev, f"Scope event missing 'seq': {list(ev.keys())}"
        assert "channels" in ev, f"Scope event missing 'channels': {list(ev.keys())}"

        channels = ev["channels"]
        assert isinstance(channels, list), "'channels' must be list"
        assert len(channels) == 4, f"Expected 4 scope channels, got {len(channels)}"

        for i, ch in enumerate(channels):
            assert "avg" in ch, f"Scope channel[{i}] missing 'avg'"
            assert "min" in ch, f"Scope channel[{i}] missing 'min'"
            assert "max" in ch, f"Scope channel[{i}] missing 'max'"
            assert isinstance(ch["avg"], (int, float)), "avg must be numeric"
            assert ch["min"] <= ch["avg"] <= ch["max"] or ch["min"] == ch["max"] == ch["avg"], (
                f"Channel[{i}]: min <= avg <= max violated: {ch}"
            )
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Streaming not supported over HTTP
# ---------------------------------------------------------------------------

def test_streaming_not_supported_over_http(http_device):
    """
    Attempting to start ADC streaming over HTTP should raise NotImplementedError.
    HTTP transport is polling-only.
    """
    with pytest.raises(NotImplementedError):
        http_device.start_adc_stream([0], divider=1)
    assert_no_faults(http_device)


# ---------------------------------------------------------------------------
# Multiple stream start/stop cycles
# ---------------------------------------------------------------------------

def test_adc_stream_restart(usb_device):
    """
    Start, stop, and restart ADC streaming multiple times.
    Verifies the firmware handles repeated start/stop without getting stuck.
    """
    for cycle in range(3):
        events = []
        done = threading.Event()

        def on_data(mask, samples, _cycle=cycle):
            events.append(len(samples))
            if len(events) >= 2:
                done.set()

        usb_device.start_adc_stream([0], divider=1, callback=on_data)
        done.wait(timeout=3.0)
        usb_device.stop_adc_stream()
        time.sleep(0.05)

        assert len(events) >= 1, f"Cycle {cycle}: no events received"
    assert_no_faults(usb_device)
