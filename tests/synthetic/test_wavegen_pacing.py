"""AN-06: the waveform task paced samples with `while (now < next) taskYIELD();`
after a truncated vTaskDelay. taskYIELD only hands the CPU to tasks of equal
or higher priority, so lower-priority tasks on that core starved for the
sub-millisecond remainder of every sample (continuously at the 500 us
minimum interval).

B: a periodic esp_timer notifies the task once per sample; the task blocks in
ulTaskNotifyTake between samples and never spins.
"""

import re


from tests.lib.srcread import REPO_ROOT

SRC = (REPO_ROOT / "Firmware/ESP32/src/tasks.cpp").read_text(encoding="utf-8")


def _wavegen_loop() -> str:
    start = SRC.index("// Generation loop")
    end = SRC.index("esp_timer_stop(pace);", start)
    return re.sub(r"//[^\n]*", "", SRC[start:end])


def test_waveform_pacing_blocks_instead_of_spinning():
    loop = _wavegen_loop()
    assert "while (esp_timer_get_time() < nextSampleTime)" not in loop
    assert "ulTaskNotifyTake" in loop
