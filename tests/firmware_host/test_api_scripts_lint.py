"""Host tests for api_scripts_lint argument handling (audit finding #4).

Tests argument handling and validation with a stubbed scripting_lint_string:
- "name": valid script name reading stored file content
- "src": inline source string validation
- "neither": missing both name and src returns error
- "invalid name": path traversal or invalid characters return error
- "missing file": non-existent script name returns not found
"""

import json
from pathlib import Path

from tests.firmware_host.fwhost import compile_and_run, extract_function

API_SCRIPTS = "Firmware/ESP32/src/net/api_scripts.cpp"
STORAGE_SRC = "Firmware/ESP32/src/mp/script_storage.cpp"
RUNTIME_SRC = "Firmware/ESP32/src/mp/script_runtime.c"
INC = ["Firmware/ESP32/src/mp", "Firmware/ESP32/src/net"]

HARNESS_PREAMBLE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "script_runtime.h"
#include "script_storage.h"

#define SCRIPTS_EVAL_MAX 32768u

#define MALLOC_CAP_SPIRAM 0
static inline void *heap_caps_malloc(size_t size, int caps) { (void)caps; return malloc(size); }
static inline void heap_caps_free(void *ptr) { free(ptr); }

// Minimal cJSON stub for host test harness
#define cJSON_Invalid (0)
#define cJSON_False   (1 << 0)
#define cJSON_True    (1 << 1)
#define cJSON_String  (1 << 4)
#define cJSON_Object  (1 << 6)

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *child;
    int type;
    char *string;
    char *valuestring;
} cJSON;

static cJSON *cJSON_CreateObject(void) {
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    if (item) item->type = cJSON_Object;
    return item;
}

static void cJSON_Delete(cJSON *item) {
    if (!item) return;
    cJSON *cur = item->child;
    while (cur) {
        cJSON *next = cur->next;
        cJSON_Delete(cur);
        cur = next;
    }
    if (item->string) free(item->string);
    if (item->valuestring) free(item->valuestring);
    free(item);
}

static void cJSON_AddItemToObject(cJSON *obj, const char *string, cJSON *item) {
    if (!obj || !item) return;
    item->string = string ? strdup(string) : NULL;
    if (!obj->child) {
        obj->child = item;
    } else {
        cJSON *cur = obj->child;
        while (cur->next) cur = cur->next;
        cur->next = item;
    }
}

static cJSON *cJSON_AddBoolToObject(cJSON *obj, const char *name, bool b) {
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    if (item) {
        item->type = b ? cJSON_True : cJSON_False;
        cJSON_AddItemToObject(obj, name, item);
    }
    return item;
}

static cJSON *cJSON_AddStringToObject(cJSON *obj, const char *name, const char *string) {
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    if (item) {
        item->type = cJSON_String;
        item->valuestring = string ? strdup(string) : NULL;
        cJSON_AddItemToObject(obj, name, item);
    }
    return item;
}

static cJSON *cJSON_GetObjectItem(const cJSON *obj, const char *name) {
    if (!obj || !name) return NULL;
    cJSON *cur = obj->child;
    while (cur) {
        if (cur->string && strcmp(cur->string, name) == 0) return cur;
        cur = cur->next;
    }
    return NULL;
}

static bool cJSON_IsString(const cJSON *item) {
    return item && (item->type & cJSON_String) && item->valuestring != NULL;
}

static char *cJSON_PrintUnformatted(const cJSON *item) {
    if (!item) return NULL;
    char *buf = (char *)malloc(2048);
    if (!buf) return NULL;
    buf[0] = '{';
    size_t pos = 1;
    cJSON *cur = item->child;
    bool first = true;
    while (cur) {
        if (!first) {
            buf[pos++] = ',';
        }
        first = false;
        pos += snprintf(buf + pos, 2048 - pos, "\"%s\":", cur->string ? cur->string : "");
        if (cur->type & cJSON_True) {
            pos += snprintf(buf + pos, 2048 - pos, "true");
        } else if (cur->type & cJSON_False) {
            pos += snprintf(buf + pos, 2048 - pos, "false");
        } else if (cur->type & cJSON_String) {
            pos += snprintf(buf + pos, 2048 - pos, "\"%s\"", cur->valuestring ? cur->valuestring : "");
        }
        cur = cur->next;
    }
    buf[pos++] = '}';
    buf[pos] = '\0';
    return buf;
}

// Stubbed scripting_lint_string
static bool g_stub_lint_ok = true;
static char g_stub_lint_err[256] = "";
static char g_last_linted[1024] = "";

bool scripting_lint_string(const char *src, size_t len, char *out_err, size_t max_err) {
    size_t clen = len < sizeof(g_last_linted) - 1 ? len : sizeof(g_last_linted) - 1;
    memcpy(g_last_linted, src, clen);
    g_last_linted[clen] = '\0';
    if (!g_stub_lint_ok && out_err && max_err > 0) {
        strncpy(out_err, g_stub_lint_err, max_err - 1);
        out_err[max_err - 1] = '\0';
    }
    return g_stub_lint_ok;
}
"""


def _run_test(tmp_path: Path, main_body: str) -> str:
    fn_take = extract_function(API_SCRIPTS, r"^static char \*take\(")
    fn_err = extract_function(API_SCRIPTS, r"^static char \*err_json\(")
    fn_str = extract_function(API_SCRIPTS, r"^static bool arg_str\(")
    fn_name = extract_function(API_SCRIPTS, r"^static bool arg_name\(")
    fn_lint = extract_function(API_SCRIPTS, r"^char \*api_scripts_lint\(")

    full_src = f"""{HARNESS_PREAMBLE}
{fn_take}
{fn_err}
{fn_str}
{fn_name}
{fn_lint}

int main(void) {{
{main_body}
    return 0;
}}
"""
    return compile_and_run(
        full_src,
        cxx=True,
        sources=[STORAGE_SRC, RUNTIME_SRC],
        include_dirs=INC,
        defines=[f'SCRIPTS_BASE="{tmp_path.as_posix()}"'],
    )


def test_api_scripts_lint_by_name(tmp_path):
    # Create a stored script file
    (tmp_path / "app.py").write_text("print('hello world')\n", encoding="utf-8")

    main = r"""
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", "app.py");
    char *res = api_scripts_lint("/api/scripts/lint", body);
    printf("RES:%s\nLINTED:%s\n", res, g_last_linted);
    free(res);
    cJSON_Delete(body);
    """
    out = _run_test(tmp_path, main)
    lines = out.strip().split("\n")
    res_json = json.loads(lines[0].replace("RES:", ""))
    assert res_json == {"ok": True}
    assert lines[1] == "LINTED:print('hello world')"


def test_api_scripts_lint_by_src(tmp_path):
    main = r"""
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "src", "def foo(): pass");
    char *res = api_scripts_lint("/api/scripts/lint", body);
    printf("RES:%s\nLINTED:%s\n", res, g_last_linted);
    free(res);
    cJSON_Delete(body);
    """
    out = _run_test(tmp_path, main)
    lines = out.strip().split("\n")
    res_json = json.loads(lines[0].replace("RES:", ""))
    assert res_json == {"ok": True}
    assert lines[1] == "LINTED:def foo(): pass"


def test_api_scripts_lint_by_src_syntax_error(tmp_path):
    main = r"""
    g_stub_lint_ok = false;
    snprintf(g_stub_lint_err, sizeof(g_stub_lint_err), "SyntaxError: invalid syntax");
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "src", "def bad(");
    char *res = api_scripts_lint("/api/scripts/lint", body);
    printf("RES:%s\n", res);
    free(res);
    cJSON_Delete(body);
    """
    out = _run_test(tmp_path, main)
    res_json = json.loads(out.strip().replace("RES:", ""))
    assert res_json == {"ok": False, "err": "SyntaxError: invalid syntax"}


def test_api_scripts_lint_neither_name_nor_src(tmp_path):
    main = r"""
    cJSON *body = cJSON_CreateObject();
    char *res = api_scripts_lint("/api/scripts/lint", body);
    printf("RES:%s\n", res);
    free(res);
    cJSON_Delete(body);

    char *res_null = api_scripts_lint("/api/scripts/lint", NULL);
    printf("RES_NULL:%s\n", res_null);
    free(res_null);
    """
    out = _run_test(tmp_path, main)
    lines = out.strip().split("\n")
    res1 = json.loads(lines[0].replace("RES:", ""))
    res2 = json.loads(lines[1].replace("RES_NULL:", ""))
    assert res1 == {"ok": False, "error": "name or src required"}
    assert res2 == {"ok": False, "error": "name or src required"}


def test_api_scripts_lint_invalid_name(tmp_path):
    main = r"""
    cJSON *body1 = cJSON_CreateObject();
    cJSON_AddStringToObject(body1, "name", "../traversal.py");
    char *res1 = api_scripts_lint("/api/scripts/lint", body1);
    printf("RES1:%s\n", res1);
    free(res1);
    cJSON_Delete(body1);

    cJSON *body2 = cJSON_CreateObject();
    cJSON_AddStringToObject(body2, "name", "no_extension");
    char *res2 = api_scripts_lint("/api/scripts/lint", body2);
    printf("RES2:%s\n", res2);
    free(res2);
    cJSON_Delete(body2);
    """
    out = _run_test(tmp_path, main)
    lines = out.strip().split("\n")
    res1 = json.loads(lines[0].replace("RES1:", ""))
    res2 = json.loads(lines[1].replace("RES2:", ""))
    assert res1 == {"ok": False, "error": "valid name required"}
    assert res2 == {"ok": False, "error": "valid name required"}


def test_api_scripts_lint_missing_file(tmp_path):
    main = r"""
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", "nonexistent.py");
    char *res = api_scripts_lint("/api/scripts/lint", body);
    printf("RES:%s\n", res);
    free(res);
    cJSON_Delete(body);
    """
    out = _run_test(tmp_path, main)
    res_json = json.loads(out.strip().replace("RES:", ""))
    assert res_json == {"ok": False, "error": "script not found"}
