"""daq_codec.c (S3, MicroPython `daq` module) must decode exactly what the
reference host decoder python/bugbuster/battsim.py decodes, and encode TLVs
byte-identical to daq_config.tlv_encode. Constants are derived from the P4
registry / battsim_host.h, never retyped."""

import re
import struct

from bugbuster import battsim as bs
from bugbuster.daq_config import DaqKey, DaqType, tlv_encode
from tests.firmware_host.fwhost import compile_and_run
from tests.lib.srcread import read_source

SRC = "Firmware/ESP32/src/mp/daq_codec.c"
HDR = "Firmware/ESP32/src/mp/daq_codec.h"
INC = ["Firmware/ESP32/src/mp"]
REGISTRY = "Firmware/DAQ_HAT/common/daq_config_registry.h"
HOST_H = "Firmware/DAQ_HAT/ESP32P4/src/battsim/battsim_host.h"

HEXARG = r"""
static size_t unhex(const char *s, uint8_t *out) {
    size_t n = 0; unsigned v;
    while (s[0] && s[1] && sscanf(s, "%2x", &v) == 1) { out[n++] = (uint8_t)v; s += 2; }
    return n;
}
"""


def _run(main: str, *args: str) -> str:
    return compile_and_run(main, sources=[SRC], include_dirs=INC, args=args).strip()


def test_constants_match_p4_sources():
    reg = read_source(REGISTRY)
    hdr = read_source(HDR)
    for name, idx in re.findall(r"DAQ_K_BS_(\w+)\s*=\s*DAQ_KEY\(DAQ_GRP_BATT,\s*0x([0-9A-Fa-f]+)\)", reg):
        m = re.search(rf"DAQC_K_BS_{name}\s*=\s*0x([0-9A-Fa-f]+)", hdr)
        assert m and int(m.group(1), 16) == 0x0800 | int(idx, 16), name
    for name, val in re.findall(r"DAQ_ACT_BS_(RUN_\w+)\s*=\s*(\d+)", reg):
        m = re.search(rf"DAQC_ACT_BS_{name}\s*=\s*(\d+)", hdr)
        assert m and int(m.group(1)) == int(val), name
    for name, val in re.findall(r"BS_HOP_(\w+)\s*=\s*(\d+)", read_source(HOST_H)):
        m = re.search(rf"DAQC_BS_OP_{name}\s*=\s*(\d+)", hdr)
        assert m and int(m.group(1)) == int(val), name
    for t in ("BOOL", "U8", "U16", "U32", "ENUM", "STR"):
        m = re.search(rf"DAQC_T_{t}\s*=\s*(\d+)", hdr)
        assert m and int(m.group(1)) == DaqType[t], t


TLV_MAIN = r"""
#include <stdio.h>
#include "daq_codec.h"
static void hex(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", b[i]); printf(" "); }
int main(void) {
    uint8_t b[40]; size_t n;
    n = daqc_tlv_int(b, sizeof b, DAQC_K_BS_CAPACITY_MAH, 2000); hex(b, n);
    n = daqc_tlv_int(b, sizeof b, DAQC_K_BS_SD_ENABLE, 5);       hex(b, n);
    n = daqc_tlv_int(b, sizeof b, DAQC_K_BS_CHEM, 1);            hex(b, n);
    n = daqc_tlv_str(b, sizeof b, DAQC_K_BS_NAME, "smoke");      hex(b, n);
    printf("%zu %zu %zu %zu\n",
        daqc_tlv_int(b, sizeof b, 0x0999, 1),                         /* unknown key   */
        daqc_tlv_int(b, sizeof b, DAQC_K_BS_CELLS, 300),              /* > u8          */
        daqc_tlv_str(b, sizeof b, DAQC_K_BS_NAME, "123456789012345678901234"), /* 24 > 23 */
        daqc_tlv_int(b, 5, DAQC_K_BS_CAPACITY_MAH, 1));               /* no room       */
    return 0;
}
"""


def test_tlv_matches_python_encoder():
    out = _run(TLV_MAIN).split()
    assert out[:4] == [tlv_encode(DaqKey.BS_CAPACITY_MAH, DaqType.U32, 2000).hex(),
                       tlv_encode(DaqKey.BS_SD_ENABLE, DaqType.BOOL, 1).hex(),
                       tlv_encode(DaqKey.BS_CHEM, DaqType.ENUM, 1).hex(),
                       tlv_encode(DaqKey.BS_NAME, DaqType.STR, "smoke").hex()]
    assert out[4:] == ["0", "0", "0", "0"]


STATUS_MAIN = r"""
#include <stdio.h>
#include "daq_codec.h"
""" + HEXARG + r"""
int main(int argc, char **argv) {
    (void)argc;
    uint8_t raw[128]; size_t n = unhex(argv[1], raw);
    daqc_bs_status_t s;
    if (!daqc_bs_status_decode(raw, n, &s)) { printf("fail\n"); return 0; }
    printf("run=%u state=%s soc=%.2f v=%.4f i=%.4f el=%u rem=%lld qdut=%.4f e=%.4f err=%s chem=%s\n",
        (unsigned)s.run_id, daqc_state_name(s.state), s.soc_pct, s.v_meas, s.i_meas,
        (unsigned)s.elapsed_s, (long long)s.remaining_s, s.q_dut_c, s.e_dut_j,
        daqc_error_name(s.last_error), daqc_chem_name(s.chem));
    printf("short=%d\n", daqc_bs_status_decode(raw, 40, &s));
    return 0;
}
"""


def test_status_decode_matches_python():
    raw = struct.pack(bs._STATUS_FMT, 2, 2, bs.FLAG_REMAIN_OK, 3, 7, 1, 2, 2000, 8765, 0,
                      3600, 7200, 3.9, 3.85, 0.25, 0.2, 1_000_000_000, 2_000_000_000,
                      0, 0, 0, 100, 50) + struct.pack("<q", 5_000_000)
    ref = bs.parse_status(raw)
    out = _run(STATUS_MAIN, raw.hex()).splitlines()
    assert out[0] == (f"run={ref.run_id} state=active soc={ref.soc_pct:.2f} v={ref.v_meas:.4f} "
                      f"i={ref.i_meas:.4f} el={ref.elapsed_s} rem={ref.remaining_s} "
                      f"qdut={ref.q_dut_c:.4f} e={ref.e_dut_j:.4f} err=busy chem=lifepo4")
    assert out[1] == "short=0"


META_MAIN = r"""
#include <stdio.h>
#include "daq_codec.h"
""" + HEXARG + r"""
int main(int argc, char **argv) {
    (void)argc;
    uint8_t raw[128]; size_t n = unhex(argv[1], raw);
    daqc_bs_meta_t m;
    if (!daqc_bs_meta_decode(raw, n, &m)) { printf("fail\n"); return 0; }
    printf("%u %u %u [%s] %u %u %d %d %d %u %.1f %u %u %.3f %.2f %u\n", (unsigned)m.run_id,
        (unsigned)m.version, (unsigned)m.created_epoch, m.name, (unsigned)m.params.chem,
        (unsigned)m.params.cells, m.params.sd_enable, m.params.ext_enable, m.params.dither,
        (unsigned)m.params.capacity_mah, m.params.start_soc_pct, (unsigned)m.params.cutoff_mv_cell,
        (unsigned)m.params.rint_uohm_cell, m.params.peukert, m.params.sd_pct_month,
        (unsigned)m.params.ext_load_ua);
    raw[0] ^= 0xFF;
    printf("badmagic=%d\n", daqc_bs_meta_decode(raw, n, &m));
    return 0;
}
"""


def test_meta_decode_matches_python():
    params = struct.pack(bs._PARAMS_FMT, 0, 1, 1, 0, 1, 2000, 1000, 3000, 30000, 1050, 200, 1000)
    raw = struct.pack(bs._META_FMT, 0x4E525342, 2, 7, 1_700_000_000,
                      b"smoke".ljust(24, b"\0"), params, 0)
    ref = bs.parse_meta(raw)
    p = ref.params
    out = _run(META_MAIN, raw.hex()).splitlines()
    assert out[0] == (f"7 2 1700000000 [smoke] {p.chem} {p.cells} 1 0 1 {p.capacity_mah} "
                      f"{p.start_soc_pct:.1f} {p.cutoff_mv_cell} {p.rint_uohm_cell} "
                      f"{p.peukert:.3f} {p.sd_pct_month:.2f} {p.ext_load_ua}")
    assert out[1] == "badmagic=0"


REQ_MAIN = r"""
#include <stdio.h>
#include "daq_codec.h"
""" + HEXARG + r"""
static void hex(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", b[i]); printf(" "); }
int main(int argc, char **argv) {
    (void)argc;
    uint8_t b[16];
    hex(b, daqc_s1_request(b, 7, 4, 14));
    hex(b, daqc_read_request(b, 7, DAQC_BS_FILE_META, 0, 68));
    hex(b, daqc_list_request(b, 2));
    printf("\n");
    uint8_t raw[240]; size_t n = unhex(argv[1], raw);
    daqc_s1_t s[DAQC_S1_MAX]; uint16_t run; bool more;
    int k = daqc_s1_decode(raw, n, &run, s, DAQC_S1_MAX, &more);
    printf("k=%d run=%u more=%d t=%u v=%.3f i=%.6f soc=%.2f f=%u\n", k, (unsigned)run, more,
        (unsigned)s[0].t_s, s[0].v, s[0].i, s[0].soc_pct, (unsigned)s[0].flags);
    printf("trunc=%d\n", daqc_s1_decode(raw, n - 1, &run, s, DAQC_S1_MAX, &more));
    uint8_t lr[] = { 3, 0, 9, 0, 1, 0, 2, 0, 9, 0 };
    uint16_t ids[8], total, active; int cnt;
    daqc_list_decode(lr, sizeof lr, &total, &active, ids, 8, &cnt);
    printf("list total=%u active=%u cnt=%d ids=%u,%u,%u\n", (unsigned)total, (unsigned)active, cnt,
        (unsigned)ids[0], (unsigned)ids[1], (unsigned)ids[2]);
    printf("chem %d %d %d %d\n", daqc_chem_from_name("LiPo"), daqc_chem_from_name("lifepo4"),
        daqc_chem_from_name("lead"), daqc_chem_from_name("plutonium"));
    return 0;
}
"""


def test_requests_and_s1_and_list_and_chem():
    s1_fmt = getattr(bs, "_S1_FMT", "<IHHiHH")
    bs_op_s1_since = getattr(bs.BsOp, "S1_SINCE", 6)
    reply = struct.pack("<HBB", 7, 1, 1) + struct.pack(s1_fmt, 5, 3005, 9995, 1000, 1, 1)
    out = _run(REQ_MAIN, reply.hex()).splitlines()
    assert out[0].split() == [
        (bytes([bs_op_s1_since]) + struct.pack("<HIB", 7, 4, 14)).hex(),
        (bytes([bs.BsOp.READ]) + struct.pack("<HHIB", 7, 0, 0, 68)).hex(),
        (bytes([bs.BsOp.LIST_RUNS]) + struct.pack("<H", 2)).hex()]
    assert out[1] == "k=1 run=7 more=1 t=5 v=3.005 i=0.001000 soc=99.95 f=1"
    assert out[2] == "trunc=-1"
    assert out[3] == "list total=3 active=9 cnt=3 ids=1,2,9"
    assert out[4] == "chem 0 1 3 -1"
