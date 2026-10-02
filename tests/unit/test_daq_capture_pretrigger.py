"""DAQ-10: triggered captures keep pre-trigger data and can trigger on a
current threshold (host side; the stream is continuous, so the pre-roll comes
from records already received). A: samples before the trigger were discarded
and only a TRIGGER marker from the S3 IO engine could start a capture."""


from bugbuster import daq_stream as ds


class FakeStream(ds.DaqStream):
    def __init__(self, batches):
        super().__init__()
        self._batches = list(batches)

    def start(self):
        pass

    def stop(self):
        pass

    def read_records(self, timeout_ms=200):
        return self._batches.pop(0) if self._batches else []


def _wave(start, values, rate=1000):
    return ds.WaveIRecord(start, start * 1000, rate, 1, list(values), bytes(len(values)))


def test_marker_trigger_keeps_pre_trigger_samples():
    trig = ds.MarkerRecord(sample_index=150, timestamp_us=0, channel=3, edge=1, kind=ds.MARK_KIND_TRIGGER)
    batches = [[_wave(0, [0.0] * 100)], [_wave(100, [1.0] * 100), trig], [_wave(200, [2.0] * 100)]]
    cap = FakeStream(batches).capture(duration_s=0.05, wait_for_trigger=True,
                                      pre_trigger_s=0.02, trigger_timeout_s=5, start_stream=False)
    assert cap.start_index == 130          # 20 ms at 1 kSPS before index 150
    assert cap.trigger_offset == 20
    assert cap.current[:20] == [1.0] * 20


def test_current_threshold_trigger():
    batches = [[_wave(0, [0.01] * 100)], [_wave(100, [0.01] * 30 + [0.5] * 70)], [_wave(200, [0.5] * 100)]]
    cap = FakeStream(batches).capture(duration_s=0.05, trigger_current_a=0.1, pre_trigger_s=0.01,
                                      trigger_timeout_s=5, start_stream=False)
    assert cap.start_index == 120
    assert cap.trigger_offset == 10
    assert cap.current[cap.trigger_offset] == 0.5
    assert cap.current[cap.trigger_offset - 1] == 0.01
