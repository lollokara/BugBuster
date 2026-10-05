"""Host tests for the api_scripts.cpp USB-tunnel dispatcher, reset and autorun run-now routes.

The firmware functions are sliced out of api_scripts.cpp unchanged and compiled against a small
cJSON model and stubs for the scripting runtime, so op-id -> route mapping, the unknown-op flag,
the capability reply and the run-now busy/failure shapes run as shipped."""

import re

from tests.firmware_host.fwhost import compile_and_run, extract_function
from tests.lib.srcread import read_source

API = "Firmware/ESP32/src/net/api_scripts.cpp"
INC = ["Firmware/ESP32/src/mp", "Firmware/ESP32/src/net"]

PREAMBLE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "script_runtime.h"
#include "script_storage.h"

#define SCRIPTS_FILE_PAGE 3072u
#define MP_LOG_RESP_MAX 4096u

/* Just enough cJSON: objects/arrays of bool, number and string, printed compactly. */
enum { J_OBJ, J_ARR, J_BOOL, J_NUM, J_STR };
typedef struct cJSON { struct cJSON *next, *child; int type; char *key; char *str; double num; bool b; } cJSON;
static cJSON *mk(int t) { cJSON *j = (cJSON *)calloc(1, sizeof *j); j->type = t; return j; }
static cJSON *cJSON_CreateObject(void) { return mk(J_OBJ); }
static cJSON *cJSON_CreateString(const char *s) { cJSON *j = mk(J_STR); j->str = strdup(s); return j; }
static void add(cJSON *p, const char *k, cJSON *c) {
    c->key = k ? strdup(k) : NULL;
    if (!p->child) p->child = c; else { cJSON *x = p->child; while (x->next) x = x->next; x->next = c; }
}
static cJSON *cJSON_AddBoolToObject(cJSON *o, const char *k, bool b) { cJSON *j = mk(J_BOOL); j->b = b; add(o, k, j); return j; }
static cJSON *cJSON_AddNumberToObject(cJSON *o, const char *k, double n) { cJSON *j = mk(J_NUM); j->num = n; add(o, k, j); return j; }
static cJSON *cJSON_AddStringToObject(cJSON *o, const char *k, const char *s) { cJSON *j = cJSON_CreateString(s); add(o, k, j); return j; }
static cJSON *cJSON_AddArrayToObject(cJSON *o, const char *k) { cJSON *j = mk(J_ARR); add(o, k, j); return j; }
static void cJSON_AddItemToArray(cJSON *a, cJSON *c) { add(a, NULL, c); }
static void cJSON_Delete(cJSON *j) { (void)j; }
static void cJSON_free(void *p) { free(p); }
static void pr(const cJSON *j, char *out, size_t *n) {
    switch (j->type) {
    case J_BOOL: *n += sprintf(out + *n, j->b ? "true" : "false"); break;
    case J_NUM:  *n += sprintf(out + *n, "%g", j->num); break;
    case J_STR:  *n += sprintf(out + *n, "\"%s\"", j->str); break;
    default: {
        out[(*n)++] = j->type == J_OBJ ? '{' : '[';
        for (const cJSON *c = j->child; c; c = c->next) {
            if (c != j->child) out[(*n)++] = ',';
            if (j->type == J_OBJ) *n += sprintf(out + *n, "\"%s\":", c->key);
            pr(c, out, n);
        }
        out[(*n)++] = j->type == J_OBJ ? '}' : ']';
    }}
}
static char *cJSON_PrintUnformatted(const cJSON *j) { char *o = (char *)calloc(1, 4096); size_t n = 0; pr(j, o, &n); return o; }

/* Scripting runtime stubs. */
typedef struct { uint32_t file_slot_id; char file_slot_name[SCRIPT_NAME_MAX + 1]; } ScriptStatus;
static ScriptStatus g_status;
static void scripting_get_status(ScriptStatus *s) { *s = g_status; }
static int g_run_now_calls = 0, g_run_ok = 1, g_run_id = 5;
static const char *g_run_err = "";
static bool autorun_run_now(uint32_t *id, char *err, size_t n) {
    g_run_now_calls++;
    *id = (uint32_t)g_run_id;
    snprintf(err, n, "%s", g_run_err);
    return g_run_ok;
}
static int g_reset_calls = 0;
static void scripting_reset_vm(void) { g_reset_calls++; }
static void scripting_stop(void) {}

/* Every route echoes the path it was dispatched with. */
static char *route_echo(const char *path, const cJSON *body) {
    (void)body;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "path", path);
    return cJSON_PrintUnformatted(r);
}
#define ROUTE(n) static char *n(const char *path, const cJSON *body) { return route_echo(path, body); }
ROUTE(api_scripts_status) ROUTE(api_scripts_logs) ROUTE(api_scripts_stop) ROUTE(api_scripts_files)
ROUTE(api_scripts_storage) ROUTE(api_scripts_file_get) ROUTE(api_scripts_file_delete)
ROUTE(api_scripts_file_chunk) ROUTE(api_scripts_run_file) ROUTE(api_scripts_eval) ROUTE(api_scripts_lint)
ROUTE(api_scripts_autorun_status) ROUTE(api_scripts_autorun_enable) ROUTE(api_scripts_autorun_disable)
"""


def _slice(start_marker: str, end_marker: str) -> str:
    src = read_source(API)
    a = src.index(start_marker)
    return src[a:src.index(end_marker, a)]


def _run(main_body: str) -> dict[str, str]:
    fn_take = extract_function(API, r"^static char \*take\(")
    fn_err = extract_function(API, r"^static char \*err_json\(")
    fn_ok = extract_function(API, r"^static char \*ok_json\(")
    fn_busy = extract_function(API, r"^static char \*busy_json\(")
    fn_reset = extract_function(API, r"^char \*api_scripts_reset\(")
    fn_run = extract_function(API, r"^char \*api_scripts_autorun_run\(")
    table = _slice("typedef char *(*script_route_fn)", "char *api_scripts_dispatch")
    fn_dispatch = extract_function(API, r"^char \*api_scripts_dispatch\(")
    # busy_json reads st.file_slot_*; the stub ScriptStatus carries them.
    src = f"""{PREAMBLE}
{fn_take}
{fn_err}
{fn_ok}
{fn_busy}
{fn_reset}
{fn_run}
{table}
{fn_dispatch}
int main(void) {{
{main_body}
    return 0;
}}
"""
    out = compile_and_run(src, cxx=True, sources=["Firmware/ESP32/src/mp/script_runtime.c"], include_dirs=INC)
    res = {}
    for line in out.splitlines():
        k, _, v = line.partition(" ")
        res[k] = v
    return res


EXPECTED_ROUTES = [
    None, "status", "logs", "stop", "files", "storage", "files/get", "files/delete", "files/chunk",
    "run-file", "eval", "lint", "autorun/status", "autorun/enable", "autorun/disable",
    "autorun/run", "reset",
]


def test_dispatch_maps_each_op_id_to_its_route_path():
    # autorun/run and reset are the real routes, the rest echo their dispatch path.
    out = _run(r"""
    for (unsigned op = 1; op <= 16; op++) {
        bool known = false;
        char *r = api_scripts_dispatch(op, NULL, &known);
        printf("op%u %d %s\n", op, known, r ? r : "NULL");
    }
    bool known = true;
    char *r = api_scripts_dispatch(17, NULL, &known);
    printf("unknown %d %s\n", known, r ? "SOME" : "NULL");
    known = true;
    r = api_scripts_dispatch(255, NULL, &known);
    printf("max %d %s\n", known, r ? "SOME" : "NULL");
    """)
    for op in range(1, 15):
        known, _, body = out[f"op{op}"].partition(" ")
        assert known == "1"
        assert body == '{"ok":true,"path":"/api/scripts/%s"}' % EXPECTED_ROUTES[op], op
    assert out["op15"].startswith("1 {")           # autorun/run: real route (checked below)
    assert out["op16"] == '1 {"ok":true}'          # reset: real route
    assert out["unknown"] == "0 NULL" and out["max"] == "0 NULL"


def test_capability_probe_lists_every_route_and_limits():
    out = _run(r"""
    bool known = false;
    char *r = api_scripts_dispatch(0, NULL, &known);
    printf("caps %d %s\n", known, r);
    """)
    known, _, body = out["caps"].partition(" ")
    assert known == "1"
    assert '"tunnel":1' in body and '"maxScriptBytes":32768' in body
    assert '"filePage":3072' in body and '"logPage":4096' in body
    listed = re.search(r'"ops":\[(.*?)\]', body).group(1).replace('"', "").split(",")
    assert listed == EXPECTED_ROUTES[1:]


def test_reset_route_requests_a_vm_teardown_and_reports_ok():
    out = _run(r"""
    bool known;
    char *r = api_scripts_dispatch(16, NULL, &known);
    printf("reset %s %d\n", r, g_reset_calls);
    """)
    assert out["reset"] == '{"ok":true} 1'


def test_autorun_run_now_is_refused_while_a_file_script_holds_the_slot():
    out = _run(r"""
    g_status.file_slot_id = 7; strcpy(g_status.file_slot_name, "held.py");
    char *r = api_scripts_autorun_run("/api/scripts/autorun/run", NULL);
    printf("busy %s calls=%d\n", r, g_run_now_calls);
    """)
    body, _, calls = out["busy"].rpartition(" ")
    assert '"running":"held.py"' in body and '"id":7' in body and '"ok":false' in body
    assert calls == "calls=0"                         # the script was never launched


def test_autorun_run_now_reports_id_on_success_and_error_text_on_failure():
    out = _run(r"""
    char *ok = api_scripts_autorun_run("/api/scripts/autorun/run", NULL);
    printf("ok %s\n", ok);
    g_run_ok = 0; g_run_err = "ZeroDivisionError"; g_run_id = 9;
    char *bad = api_scripts_autorun_run("/api/scripts/autorun/run", NULL);
    printf("bad %s\n", bad);
    g_run_ok = 0; g_run_err = ""; g_run_id = 0;
    char *none = api_scripts_autorun_run("/api/scripts/autorun/run", NULL);
    printf("none %s\n", none);
    """)
    assert out["ok"] == '{"ok":true,"id":5}'
    assert out["bad"] == '{"ok":false,"id":9,"error":"ZeroDivisionError"}'
    assert out["none"] == '{"ok":false,"error":"autorun script did not complete"}'
