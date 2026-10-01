"""C6-25: an interrupted C6 staging run wedged every later C6 update.

relay_stage persists STAGING/PUSHING to NVS so a P4 reset can resume, and
relay_stage_begin() refused any new run while that state was set. After a P4
reboot the S3's OTA_ABORT is routed by the P4's RAM-only `s_ota_target`
(back to P4-self), so nothing could clear it: every S3-driven C6 update failed
until someone erased NVS. The same happened within one boot when the S3 died
mid-upload and never sent ABORT.

B: a session not touched in this boot, or idle for RELAY_STALE_MS, is stale and
a new begin replaces it. A live session is still protected, and a resumed write
after a reboot still works.

Host-compiled against RAM-backed NVS / partition stubs; a "reboot" is a second
relay_stage_init(), which reloads state from the NVS stub.
"""

from pathlib import Path


from tests.firmware_host.fwhost import compile_and_run

OTA = "Firmware/DAQ_HAT/ESP32P4/src/ota"

STUBS = {
    "esp_err.h": r"""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_CRC 0x109
""",
    "nvs.h": r"""#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
extern uint8_t g_nvs_blob[256]; extern size_t g_nvs_len;
static inline esp_err_t nvs_open(const char *n, nvs_open_mode_t m, nvs_handle_t *h) { (void)n; (void)m; *h = 1; return ESP_OK; }
static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t l) { (void)h; (void)k; memcpy(g_nvs_blob, v, l); g_nvs_len = l; return ESP_OK; }
static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *l) { (void)h; (void)k; if (!g_nvs_len) return ESP_FAIL; memcpy(v, g_nvs_blob, *l < g_nvs_len ? *l : g_nvs_len); return ESP_OK; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
""",
    "esp_partition.h": r"""#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
typedef struct { uint32_t size; } esp_partition_t;
#define ESP_PARTITION_TYPE_DATA 1
static uint8_t g_flash[65536];
static const esp_partition_t g_part = { sizeof(g_flash) };
static inline const esp_partition_t *esp_partition_find_first(int t, int st, const char *l) { (void)t; (void)st; (void)l; return &g_part; }
static inline esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t o, size_t n) { (void)p; memset(g_flash + o, 0xFF, n); return ESP_OK; }
static inline esp_err_t esp_partition_write(const esp_partition_t *p, size_t o, const void *d, size_t n) { (void)p; memcpy(g_flash + o, d, n); return ESP_OK; }
static inline esp_err_t esp_partition_read(const esp_partition_t *p, size_t o, void *d, size_t n) { (void)p; memcpy(d, g_flash + o, n); return ESP_OK; }
""",
    "mbedtls/sha256.h": r"""#pragma once
#include <stddef.h>
#include <string.h>
typedef struct { int x; } mbedtls_sha256_context;
static inline void mbedtls_sha256_init(mbedtls_sha256_context *c) { (void)c; }
static inline int mbedtls_sha256_starts(mbedtls_sha256_context *c, int is224) { (void)c; (void)is224; return 0; }
static inline int mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *b, size_t n) { (void)c; (void)b; (void)n; return 0; }
static inline int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char *o) { (void)c; memset(o, 0, 32); return 0; }
static inline void mbedtls_sha256_free(mbedtls_sha256_context *c) { (void)c; }
""",
    "freertos/semphr.h": r"""#pragma once
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFFu
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int m; return &m; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) { (void)s; (void)t; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
""",
    "esp_timer.h": r"""#pragma once
#include <stdint.h>
extern int64_t g_now_us;
static inline int64_t esp_timer_get_time(void) { return g_now_us; }
""",
}

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "relay_stage.h"
uint8_t g_nvs_blob[256]; size_t g_nvs_len;
int64_t g_now_us = 1000000;
static uint8_t chunk[4096];

static int start(void) {
    ota_meta_t m; memset(&m, 0, sizeof(m)); m.image_size = 16384;
    return relay_stage_begin(RELAY_TARGET_C6, &m);
}
static void fresh(void) { g_nvs_len = 0; g_now_us = 1000000; relay_stage_init(); }

int main(void) {
    /* 1. Interrupted, then the P4 reboots: the next begin must succeed. */
    fresh(); start(); relay_stage_write(0, chunk, sizeof(chunk));
    relay_stage_init();                                  /* reboot */
    int after_reboot = start();

    /* 2. Same boot, the S3 died mid-upload (no ABORT): stale after 120 s. */
    fresh(); start(); relay_stage_write(0, chunk, sizeof(chunk));
    g_now_us += 180LL * 1000000;
    int after_idle = start();

    /* 3. A live session (1 s since the last chunk) is still protected. */
    fresh(); start(); relay_stage_write(0, chunk, sizeof(chunk));
    g_now_us += 1000000;
    int live = start();

    /* 4. Resume after reboot still works without a new begin, from the
     *    persisted offset (persisted every 64 KB, so 0 here). */
    fresh(); start(); relay_stage_write(0, chunk, sizeof(chunk));
    relay_stage_init();
    relay_status_t st; relay_stage_get_status(&st);
    int resume = relay_stage_write(st.staged_bytes, chunk, sizeof(chunk));

    printf("after_reboot=%d after_idle=%d live=%d resume=%d\n",
           after_reboot, after_idle, live, resume);
    return 0;
}
"""


def _run(tmp_path: Path) -> dict:
    for name, text in STUBS.items():
        p = tmp_path / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    out = compile_and_run(MAIN, cxx=False, sources=[f"{OTA}/relay_stage.c"],
                          include_dirs=[tmp_path, OTA]).strip()
    return {k: int(v, 0) for k, v in (kv.split("=") for kv in out.split())}


def test_begin_replaces_a_session_orphaned_by_a_p4_reboot(tmp_path):
    assert _run(tmp_path)["after_reboot"] == 0


def test_begin_replaces_a_session_idle_past_the_timeout(tmp_path):
    assert _run(tmp_path)["after_idle"] == 0


def test_live_session_is_still_protected(tmp_path):
    assert _run(tmp_path)["live"] == 0x103   # ESP_ERR_INVALID_STATE


def test_resume_after_reboot_still_accepts_the_next_chunk(tmp_path):
    assert _run(tmp_path)["resume"] == 0
