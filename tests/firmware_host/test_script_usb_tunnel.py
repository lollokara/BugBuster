"""Host tests for the SCRIPT_AUTORUN JSON tunnel (sub 6 CALL / sub 7 FETCH) in cmd_script.cpp
and the sr_has_work() predicate behind scripting_has_work().

The tunnel block is sliced out of the firmware source and compiled unchanged against stubs for
cJSON, the heap and api_scripts_dispatch (which echoes the request, or emits a bulk response),
so the staging, paging, token and offset rules run as shipped."""

import re

from tests.firmware_host.fwhost import compile_and_run, extract_function
from tests.lib.srcread import read_source

CMD_SCRIPT = "Firmware/ESP32/src/bbp/cmds/cmd_script.cpp"
API_SCRIPTS = "Firmware/ESP32/src/net/api_scripts.cpp"
RUNTIME_SRC = "Firmware/ESP32/src/mp/script_runtime.c"
INC = ["Firmware/ESP32/src/bbp", "Firmware/ESP32/src/mp"]


def _tunnel_block() -> str:
    src = read_source(CMD_SCRIPT)
    start = src.index("#define TUN_REQ_CHUNK_MAX")
    end = src.index("// SCRIPT_AUTORUN  payload")
    end = src.rfind("// ------", start, end)
    return src[start:end]


PREAMBLE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "cmd_errors.h"
#include "bbp_codec.h"

#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static void heap_caps_free(void *p) { free(p); }

typedef struct cJSON { char *text; } cJSON;
static cJSON *cJSON_Parse(const char *s) { cJSON *j = (cJSON *)malloc(sizeof *j); j->text = strdup(s); return j; }
static void cJSON_Delete(cJSON *j) { if (j) { free(j->text); free(j); } }
static void cJSON_free(void *p) { free(p); }

typedef enum { SCRIPT_XFER_USB = 0, SCRIPT_XFER_FILE = 1 } ScriptXferSource;
static int g_active = 0, g_activity_calls = 0;
static void scripting_transfer_activity(ScriptXferSource s, bool active) { (void)s; g_active = active; g_activity_calls++; }

// Echo stub: op 2 answers {"bulk":"xxx..."} sized by the "n" in the request; op >= 17 is unknown.
static char *api_scripts_dispatch(unsigned op, const cJSON *body, bool *known) {
    *known = op < 17;
    if (!*known) return NULL;
    const char *t = body ? body->text : "null";
    if (op == 2) {
        const char *p = strstr(t, "\"n\":");
        int n = p ? atoi(p + 4) : 0;
        char *out = (char *)malloc((size_t)n + 16);
        int w = snprintf(out, (size_t)n + 16, "{\"bulk\":\"");
        for (int i = 0; i < n - 11; i++) out[w++] = (char)('a' + i % 26);
        out[w++] = '"'; out[w++] = '}'; out[w] = 0;
        return out;
    }
    char *out = (char *)malloc(strlen(t) + 32);
    sprintf(out, "{\"op\":%u,\"echo\":%s}", op, t);
    return out;
}
"""

HELPERS = r"""
static uint8_t R[1024]; static size_t RL;
static int call(unsigned op, unsigned total, unsigned off, const char *chunk, size_t n) {
    uint8_t p[1100]; size_t pos = 0;
    bbp_put_u8(p, &pos, 6); bbp_put_u8(p, &pos, (uint8_t)op);
    bbp_put_u16(p, &pos, (uint16_t)total); bbp_put_u16(p, &pos, (uint16_t)off);
    memcpy(p + pos, chunk, n); pos += n;
    return handler(p, pos, R, &RL);
}
static int fetch(unsigned token, unsigned off) {
    uint8_t p[8]; size_t pos = 0;
    bbp_put_u8(p, &pos, 7); bbp_put_u8(p, &pos, (uint8_t)token); bbp_put_u16(p, &pos, (uint16_t)off);
    return handler(p, pos, R, &RL);
}
static void show(const char *tag) {
    size_t pos = 2; uint16_t a = bbp_get_u16(R, &pos); uint16_t b = bbp_get_u16(R, &pos);
    printf("%s state=%u token=%u a=%u b=%u len=%zu\n", tag, R[0], R[1], a, b, RL);
}
"""


def _run(main_body: str) -> dict[str, str]:
    block = _tunnel_block()
    src = f"""{PREAMBLE}
{block}
static int handler(const uint8_t *payload, size_t len, uint8_t *resp, size_t *resp_len) {{
    if (payload[0] == 6) return tunnel_call(payload, len, resp, resp_len);
    return tunnel_fetch(payload, len, resp, resp_len);
}}
{HELPERS}
int main(void) {{
{main_body}
    return 0;
}}
"""
    out = compile_and_run(src, cxx=True, include_dirs=INC)
    lines = {}
    for line in out.splitlines():
        tag, _, rest = line.partition(" ")
        lines[tag] = rest
    return lines


def _kv(line: str) -> dict[str, int]:
    return {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", line)}


def test_single_frame_call_returns_done_with_first_page():
    out = _run(r"""
    const char *j = "{\"name\":\"a.py\"}";
    int rc = call(4, (unsigned)strlen(j), 0, j, strlen(j));
    show("r"); printf("rc %d\n", rc);
    printf("data %.*s\n", (int)(RL - 6), (const char *)R + 6);
    """)
    r = _kv(out["r"])
    assert r["state"] == 1 and r["a"] == r["b"] == len('{"op":4,"echo":{"name":"a.py"}}')
    assert out["data"] == '{"op":4,"echo":{"name":"a.py"}}'


def test_multi_frame_request_stages_then_executes_and_pages_the_reply():
    out = _run(r"""
    char body[2600]; unsigned total = 2500;
    memset(body, 'x', sizeof body);
    body[0] = '"'; body[total - 1] = '"';           /* a valid JSON string literal */
    call(4, total, 0, body, 1000);    show("m1");
    call(4, total, 1000, body + 1000, 1000); show("m2");
    printf("active_mid %d\n", g_active);
    call(4, total, 2000, body + 2000, 500);  show("done");
    printf("active_end %d\n", g_active);
    """)
    assert _kv(out["m1"])["state"] == 0 and _kv(out["m1"])["a"] == 1000
    assert _kv(out["m2"])["state"] == 0 and _kv(out["m2"])["a"] == 2000
    assert out["active_mid"] == "1" and out["active_end"] == "0"
    done = _kv(out["done"])
    assert done["state"] == 1 and done["b"] == 1000 and done["a"] == len('{"op":4,"echo":' + '"' + "x" * 2498 + '"}')


def test_fetch_pages_cover_the_whole_response_with_stable_token():
    out = _run(r"""
    const char *j = "{\"n\":3500}";
    call(2, (unsigned)strlen(j), 0, j, strlen(j)); show("first");
    size_t total = (size_t)((R[2]) | (R[3] << 8)); unsigned token = R[1]; size_t got = R[4] | (R[5] << 8);
    int pages = 1;
    while (got < total) { fetch(token, (unsigned)got); if (R[0] != 1) { printf("bad %u\n", R[0]); break; }
        got += (size_t)(R[4] | (R[5] << 8)); pages++; }
    printf("pages %d got %zu total %zu\n", pages, got, total);
    fetch(token, (unsigned)total); show("end");
    fetch(token + 1, 0); show("stale");
    """)
    assert out["pages"] == "4 got 3500 total 3500"
    assert _kv(out["end"])["state"] == 1 and _kv(out["end"])["b"] == 0
    assert _kv(out["stale"])["state"] == 2 and _kv(out["stale"])["a"] == 6


def test_new_call_invalidates_the_previous_response_token():
    out = _run(r"""
    const char *j = "{}";
    call(4, 2, 0, j, 2); unsigned t1 = R[1];
    call(4, 2, 0, j, 2); unsigned t2 = R[1];
    printf("tokens %u %u\n", t1, t2);
    fetch(t1, 0); show("old");
    fetch(t2, 0); show("new");
    """)
    t1, t2 = out["tokens"].split()
    assert t1 != t2
    assert _kv(out["old"])["a"] == 6
    assert _kv(out["new"])["state"] == 1


def test_framing_errors_are_rejected_not_executed():
    out = _run(r"""
    const char *j = "{}";
    call(4, 100, 50, j, 2); show("noreq");        /* continuation with no request */
    call(4, 100, 0, j, 2);  show("start");
    call(4, 100, 10, j, 2); show("gap");          /* offset != received */
    call(5, 100, 2, j, 2);  show("op");           /* different op mid-request */
    call(4, 100, 2, j, 2);  show("ok2");
    call(99, 2, 0, j, 2);   show("unknown");
    char big[1001] = {0};
    printf("zero %d\n", call(4, 0, 0, j, 0));
    printf("oversize %d\n", call(4, 3000, 0, big, 1001));
    """)
    assert _kv(out["noreq"])["state"] == 2 and _kv(out["noreq"])["a"] == 5
    gap = _kv(out["gap"])
    assert gap["state"] == 2 and gap["a"] == 2 and gap["b"] == 2     # expected offset 2
    assert _kv(out["op"])["a"] == 2
    assert _kv(out["ok2"])["state"] == 0 and _kv(out["ok2"])["a"] == 4
    assert _kv(out["unknown"])["state"] == 2 and _kv(out["unknown"])["a"] == 1
    assert int(out["zero"]) < 0 and int(out["oversize"]) < 0         # -CMD_ERR_BAD_ARG


def test_request_larger_than_declared_total_is_dropped():
    out = _run(r"""
    const char *j = "{\"a\":1}";
    call(4, 10, 0, j, 7); show("m");
    call(4, 10, 7, j, 7); show("over");
    call(4, 10, 7, j, 3); show("after");
    """)
    assert _kv(out["over"])["state"] == 2 and _kv(out["over"])["a"] == 3
    assert _kv(out["after"])["a"] == 5                                # request was dropped


WORK_MAIN = r"""
#include <stdio.h>
#include "script_runtime.h"
int main(void) {
    uint32_t none[2] = {0, 0};
    uint32_t d = sr_xfer_deadline(1000);
    uint32_t one[2] = {0, d};
    printf("idle %d\n", sr_has_work(0, none, 2, 5000));
    printf("cmds %d\n", sr_has_work(1, none, 2, 5000));
    printf("lease_live %d\n", sr_has_work(0, one, 2, 1000 + SR_XFER_LEASE_MS - 1));
    printf("lease_expired %d\n", sr_has_work(0, one, 2, 1000 + SR_XFER_LEASE_MS));
    uint32_t wrap = sr_xfer_deadline(0xFFFFFFFFu - 100);              /* deadline wraps past 0 */
    uint32_t w[1] = {wrap};
    printf("wrap_live %d\n", sr_has_work(0, w, 1, 0xFFFFFFFFu - 50));
    printf("wrap_after %d\n", sr_has_work(0, w, 1, wrap + 1));
    printf("nonzero %d\n", sr_xfer_deadline(0xFFFFFFFFu - (SR_XFER_LEASE_MS - 1)) != 0);
    printf("null_ok %d\n", sr_has_work(0, NULL, 0, 1));
    return 0;
}
"""


def test_work_predicate_covers_cmds_and_leases_not_idle_vm():
    out = compile_and_run(WORK_MAIN, sources=[RUNTIME_SRC], include_dirs=INC)
    got = dict(line.split() for line in out.splitlines())
    assert got == {"idle": "0", "cmds": "1", "lease_live": "1", "lease_expired": "0",
                   "wrap_live": "1", "wrap_after": "0", "nonzero": "1", "null_ok": "0"}


def test_tunnel_op_table_matches_the_desktop_enum():
    api = read_source(API_SCRIPTS)
    table = api[api.index("s_tunnel_ops[]"):]
    table = table[:table.index("};")]
    fw = re.findall(r'\{\s*"([a-z/-]+)",', table)
    rs = read_source("DesktopApp/BugBuster/src-tauri/src/scripts.rs")
    enum = rs[rs.index("pub enum Op {"):]
    enum = enum[:enum.index("}")]
    ids = {name: int(num) for name, num in re.findall(r"(\w+) = (\d+),", enum)}
    expect = {
        "caps": "Caps", "status": "Status", "logs": "Logs", "stop": "Stop", "files": "Files",
        "storage": "Storage", "files/get": "Get", "files/delete": "Delete",
        "files/chunk": "Chunk", "run-file": "Run", "eval": "Eval", "lint": "Lint",
        "autorun/status": "AutorunStatus", "autorun/enable": "AutorunEnable",
        "autorun/disable": "AutorunDisable", "autorun/run": "AutorunRun", "reset": "Reset",
    }
    assert len(fw) == len(expect)
    for index, name in enumerate(fw):
        assert ids[expect[name]] == index, name

GLUE_MAIN = r"""
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "script_runtime.h"
typedef enum { SCRIPT_XFER_USB = 0, SCRIPT_XFER_FILE = 1, SCRIPT_XFER_COUNT } ScriptXferSource;
static uint32_t g_now = 1000;
static uint32_t now_ms(void) { return g_now; }
static uint32_t s_work_cmds = 0;
static uint32_t s_xfer_deadline[SCRIPT_XFER_COUNT] = {0};
%s
%s
int main(void) {
    printf("idle %%d\n", scripting_has_work());
    __atomic_add_fetch(&s_work_cmds, 1, __ATOMIC_SEQ_CST);                 /* submit */
    printf("queued %%d\n", scripting_has_work());
    __atomic_sub_fetch(&s_work_cmds, 1, __ATOMIC_SEQ_CST);                 /* task finished it */
    printf("done %%d\n", scripting_has_work());
    scripting_transfer_activity(SCRIPT_XFER_FILE, true);
    printf("xfer %%d\n", scripting_has_work());
    scripting_transfer_activity(SCRIPT_XFER_USB, true);
    scripting_transfer_activity(SCRIPT_XFER_FILE, false);
    printf("other_source_keeps_it %%d\n", scripting_has_work());
    g_now += SR_XFER_LEASE_MS;                                              /* client vanished */
    printf("lease_expired %%d\n", scripting_has_work());
    scripting_transfer_activity(SCRIPT_XFER_USB, true);
    scripting_transfer_activity(SCRIPT_XFER_USB, false);
    printf("finished %%d\n", scripting_has_work());
    scripting_transfer_activity((ScriptXferSource)9, true);                 /* out of range is ignored */
    printf("bad_source %%d\n", scripting_has_work());
    return 0;
}
"""


def test_scripting_has_work_glue_follows_jobs_and_transfer_leases():
    mp = "Firmware/ESP32/src/mp/scripting.cpp"
    main = GLUE_MAIN % (
        extract_function(mp, r"^bool scripting_has_work\("),
        extract_function(mp, r"^void scripting_transfer_activity\("),
    )
    out = compile_and_run(main, cxx=True, sources=[RUNTIME_SRC], include_dirs=INC)
    got = dict(line.split() for line in out.splitlines())
    assert got == {"idle": "0", "queued": "1", "done": "0", "xfer": "1", "other_source_keeps_it": "1",
                   "lease_expired": "0", "finished": "0", "bad_source": "0"}