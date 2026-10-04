from tests.lib.srcread import read_source

SRC = "Firmware/ESP32/src/hub/hub_live.cpp"


def test_spec_numbers():
    src = read_source(SRC)
    assert "#define BACKLOG_ROWS 7200u" in src          # >= 2 h at 1 Hz
    assert "#define FLUSH_MS     10000u" in src          # batch 10 s
    assert "hub_bs(6 /* BS_HOP_S1_SINCE */" in src


def test_backlog_is_psram_and_drops_oldest():
    src = read_source(SRC)
    assert "BACKLOG_ROWS * sizeof(hub_sample_t), MALLOC_CAP_SPIRAM" in src
    assert "s_head = (s_head + 1) % BACKLOG_ROWS; s_count--;" in src


def test_resume_point_comes_from_hub_coverage():
    src = read_source(SRC)
    assert "res->valueint == 1" in src and "hub_wallmap_run_time(&L.wm" in src


def test_failed_flush_backs_off_and_keeps_rows():
    src = read_source(SRC)
    assert "L.backoff = hub_backoff_next(L.backoff);" in src
    assert "pop(n);" in src
