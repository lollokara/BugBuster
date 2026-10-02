"""DAQ-02: voltage must be zero-order held onto the current TIMEBASE.

On the P4, WAVE_I start_index counts fused current samples (sample_seq) and
WAVE_V start_index counts voltage samples (volt_seq); both restart at 0 on a
session reset (usb_stream.c). With different rates the two indices are not
comparable: voltage sample j is at t = j / rate_v.
"""

from bugbuster.daq_stream import CaptureAccumulator, WaveIRecord, WaveVRecord

RATE_I = 8_000
RATE_V = 64_000
N_I = 800                       # 100 ms of current
N_V = N_I * RATE_V // RATE_I    # the same 100 ms of voltage
STEP_T = 0.05                   # voltage steps 1 V -> 3 V half-way through


def _capture(rate_v=RATE_V, n_v=N_V):
    acc = CaptureAccumulator()
    step_j = int(STEP_T * rate_v)
    acc.feed(WaveVRecord(start_index=0, timestamp_us=0, sample_rate=rate_v,
                         voltage=[1.0] * step_j + [3.0] * (n_v - step_j)))
    acc.feed(WaveIRecord(start_index=0, timestamp_us=0, sample_rate=RATE_I, decimation=1,
                         current=[0.001] * N_I, meta=bytes(N_I)))
    return acc.finish()


def test_voltage_step_lands_at_the_right_time_when_rates_differ():
    cap = _capture()
    step_i = int(STEP_T * RATE_I)
    assert cap.voltage[step_i - 1] == 1.0
    assert cap.voltage[step_i + 1] == 3.0
    assert cap.voltage[-1] == 3.0


def test_equal_rates_unchanged():
    """Control: with equal rates the indices coincide and the old mapping holds."""
    cap = _capture(rate_v=RATE_I, n_v=N_I)
    step_i = int(STEP_T * RATE_I)
    assert cap.voltage[step_i - 1] == 1.0
    assert cap.voltage[step_i] == 3.0
