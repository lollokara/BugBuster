from tests.lib.srcread import read_source

SRC = "Firmware/ESP32/src/hub/hub_sync.cpp"


def test_tiers_are_sent_finest_first_with_their_own_res():
    src = read_source(SRC)
    assert "S.files[b].rank < S.files[b - 1].rank" in src
    assert '"/api/v1/ingest/runs/%s/samples?res=%u"' in src
    assert "hub_covered(S.cov, (size_t)S.n_cov, s.ts, f->res)" in src


def test_only_whole_records_and_wall_clock_timestamps():
    src = read_source(SRC)
    assert "hub_rec_count(S.version," in src
    assert "hub_wallmap_unix(&S.wm, cur.t)" in src


def test_undated_runs_wait_for_the_clock_and_are_never_sent():
    src = read_source(SRC)
    assert "if (r->created == 0) continue;" in src
    assert "hub_push_epoch_to_p4()" in src


def test_old_hub_without_coverage_means_send_everything():
    src = read_source(SRC)
    assert "S.n_cov = 0;" in src and "st != 200" in src
