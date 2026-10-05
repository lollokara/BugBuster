"""S3 standby surface: BBP 0x78 codec, JSON, strict parsers, passive classification."""

from tests.firmware_host.fwhost import compile_and_run

POWER = "Firmware/ESP32/src/power"
BBP = "Firmware/ESP32/src/bbp"

MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "standby_api.h"
#include "bbp_codec.h"
#include "cmd_errors.h"

typedef struct { uint32_t now; int locked; uint32_t inh; int persisted; int persist_fail; } Env;
static void lk(void *c) { Env *e = c; assert(!e->locked); e->locked = 1; }
static void ul(void *c) { ((Env *)c)->locked = 0; }
static uint32_t nw(void *c) { return ((Env *)c)->now; }
static uint32_t ih(void *c) { return ((Env *)c)->inh; }
static StandbyHatLink hl(void *c) { return STANDBY_HAT_NONE; }
static StandbyXchg hx(void *c, const bb_standby_request_t *r, bb_standby_reply_t *p) { return STANDBY_XCHG_TIMEOUT; }
static StandbyStepResult ls(void *c, StandbyStep s, uint32_t g) { return STANDBY_STEP_DONE; }
static bool pt(void *c, uint32_t s) {
    Env *e = c;
    if (e->persist_fail) return false;
    e->persisted = (int)s;
    return true;
}

static void setup(Env *e, StandbySystem *s) {
    memset(e, 0, sizeof(*e));
    StandbyOps ops = { e, lk, ul, nw, ih, hl, hx, ls, pt, NULL, NULL, NULL };
    standby_system_init(s, &ops, 300);
}

static int call_src(StandbySystem *s, const uint8_t *p, size_t n, uint8_t *resp, size_t *rl, StandbySource src) {
    *rl = 0;
    return standby_api_bbp(s, p, n, resp, rl, src);
}
static int call(StandbySystem *s, const uint8_t *p, size_t n, uint8_t *resp, size_t *rl) {
    return call_src(s, p, n, resp, rl, STANDBY_SRC_OTHER);
}

int main(void) {
    static StandbySystem s; static Env e;
    uint8_t resp[64]; size_t rl;
    setup(&e, &s);

    /* status layout, byte for byte */
    uint8_t st_req[1] = { STANDBY_SUBOP_STATUS };
    assert(call(&s, st_req, 1, resp, &rl) == 28 && rl == 28);
    assert(resp[0] == 1 && resp[1] == BB_ST_ACTIVE && resp[2] == 1 && resp[3] == 0);
    size_t pos = 4; assert(bbp_get_u32(resp, &pos) == 1);
    pos = 8; assert(bbp_get_u16(resp, &pos) == 300);
    assert(resp[10] == 0 && resp[11] == 0);
    pos = 12; assert(bbp_get_u32(resp, &pos) == 0);
    pos = 16; assert(bbp_get_u16(resp, &pos) == 0 && bbp_get_u16(resp, &pos) == 0 && bbp_get_u16(resp, &pos) == 0);
    assert(bbp_get_u16(resp, &pos) == 0);               /* reserved */
    pos = 24; assert(bbp_get_u32(resp, &pos) == 300000 - 0);
    puts("status-layout");

    /* lengths are exact */
    uint8_t bad[8] = { STANDBY_SUBOP_STATUS, 0 };
    assert(call(&s, bad, 2, resp, &rl) == -CMD_ERR_BAD_ARG);
    assert(call(&s, bad, 0, resp, &rl) == -CMD_ERR_BAD_ARG);
    bad[0] = STANDBY_SUBOP_WAKE; assert(call(&s, bad, 2, resp, &rl) == -CMD_ERR_BAD_ARG);
    bad[0] = STANDBY_SUBOP_SLEEP; assert(call(&s, bad, 3, resp, &rl) == -CMD_ERR_BAD_ARG);
    bad[0] = 9; assert(call(&s, bad, 1, resp, &rl) == -CMD_ERR_BAD_ARG);
    bad[0] = STANDBY_SUBOP_POLICY; assert(call(&s, bad, 2, resp, &rl) == -CMD_ERR_BAD_ARG);
    uint8_t pres_short[5] = { STANDBY_SUBOP_PRESENCE, 1, 0, 0, 0 };
    assert(call(&s, pres_short, 5, resp, &rl) == -CMD_ERR_BAD_ARG);
    puts("exact-lengths");

    /* presence: client u32 + present u8 */
    uint8_t pres[6] = { STANDBY_SUBOP_PRESENCE, 0x78, 0x56, 0x34, 0x12, 1 };
    assert(call(&s, pres, 6, resp, &rl) == 28 && resp[10] == 1);
    assert(s.policy.clients[0].id == 0x12345678u);
    pres[5] = 2; assert(call(&s, pres, 6, resp, &rl) == -CMD_ERR_BAD_ARG);
    uint8_t zero[6] = { STANDBY_SUBOP_PRESENCE, 0, 0, 0, 0, 1 };
    assert(call(&s, zero, 6, resp, &rl) == -CMD_ERR_BAD_ARG);
    pres[5] = 0; assert(call(&s, pres, 6, resp, &rl) == 28 && resp[10] == 0);
    assert(call(&s, pres, 6, resp, &rl) == 28 && resp[10] == 0);      /* releasing an unknown id is idempotent */
    for (uint8_t i = 1; i <= STANDBY_MAX_CLIENTS; ++i) {
        uint8_t p[6] = { STANDBY_SUBOP_PRESENCE, i, 0, 0, 0, 1 };
        assert(call(&s, p, 6, resp, &rl) == 28);
    }
    uint8_t full[6] = { STANDBY_SUBOP_PRESENCE, 99, 0, 0, 0, 1 };
    assert(call(&s, full, 6, resp, &rl) == -CMD_ERR_BUSY);            /* table full: retryable, not a bad channel */
    assert(cmd_error_to_bbp(CMD_ERR_BUSY) == 0x06);
    puts("presence");

    /* only a USB-sourced registration switches the USB epoch; the source is the caller's, not the payload's */
    setup(&e, &s);
    standby_system_usb_session(&s, true);
    uint8_t up[6] = { STANDBY_SUBOP_PRESENCE, 5, 0, 0, 0, 1 };
    assert(call(&s, up, 6, resp, &rl) == 28 && s.usb_mode == STANDBY_USB_LEGACY);
    uint8_t up0[6] = { STANDBY_SUBOP_PRESENCE, 0, 0, 0, 0, 1 };
    assert(call_src(&s, up0, 6, resp, &rl, STANDBY_SRC_USB) == -CMD_ERR_BAD_ARG && s.usb_mode == STANDBY_USB_LEGACY);
    uint8_t upbad[6] = { STANDBY_SUBOP_PRESENCE, 6, 0, 0, 0, 2 };
    assert(call_src(&s, upbad, 6, resp, &rl, STANDBY_SRC_USB) == -CMD_ERR_BAD_ARG && s.usb_mode == STANDBY_USB_LEGACY);
    up[1] = 6;
    assert(call_src(&s, up, 6, resp, &rl, STANDBY_SRC_USB) == 28 && s.usb_mode == STANDBY_USB_EXPLICIT);
    up[5] = 0;
    assert(call_src(&s, up, 6, resp, &rl, STANDBY_SRC_USB) == 28 && resp[10] == 1);   /* client 5 remains */
    assert(standby_api_rc_to_cmd_error(STANDBY_RC_HARDWARE) == -CMD_ERR_HARDWARE);
    assert(standby_api_rc_to_cmd_error(STANDBY_RC_CAPACITY) == -CMD_ERR_BUSY);
    assert(standby_api_rc_to_cmd_error(STANDBY_RC_OK) == 0);
    puts("usb-source");

    /* policy */
    uint8_t pol[3] = { STANDBY_SUBOP_POLICY, 0x84, 0x03 };            /* 900 */
    assert(call(&s, pol, 3, resp, &rl) == 28 && e.persisted == 900);
    pos = 8; assert(bbp_get_u16(resp, &pos) == 900);
    pol[1] = 120; pol[2] = 0;
    assert(call(&s, pol, 3, resp, &rl) == -CMD_ERR_BAD_ARG);
    pol[1] = 0; assert(call(&s, pol, 3, resp, &rl) == 28);            /* off */
    pos = 8; assert(bbp_get_u16(resp, &pos) == 0);
    /* a value that could not be stored is a hardware error and is NOT in force */
    e.persist_fail = 1; pol[1] = 0x3C; pol[2] = 0;                    /* 60 */
    assert(call(&s, pol, 3, resp, &rl) == -CMD_ERR_HARDWARE);
    assert(s.policy.timeout_ms == 0u);
    e.persist_fail = 0;
    puts("policy");

    /* sleep: refused with a real inhibitor, wake idempotent */
    setup(&e, &s);
    e.inh = BB_ST_INH_BUS;
    uint8_t sl[1] = { STANDBY_SUBOP_SLEEP };
    assert(call(&s, sl, 1, resp, &rl) == -CMD_ERR_BUSY);
    e.inh = 0;
    assert(call(&s, sl, 1, resp, &rl) == 28);
    uint8_t wk[1] = { STANDBY_SUBOP_WAKE };
    assert(call(&s, wk, 1, resp, &rl) == 28 && resp[2] == 1);
    puts("sleep-wake");

    /* passive classification */
    static const uint8_t passive[] = { 0x03, 0x05, 0x07, 0x0C, 0x0E, 0x50, 0x52, 0x74,
        0x76, 0x78, 0x91, 0xA0, 0xA9, 0xB0, 0xB4, 0xC0, 0xC5, 0xE1, 0xF0, 0xF1, 0xF6, 0xF7, 0xFA, 0xFE };
    for (size_t i = 0; i < sizeof(passive); ++i) assert(standby_api_bbp_passive(passive[i]));
    /* status / device info / diagnostics carry cached ADC values or clock the converter: real work */
    static const uint8_t work[] = { 0x01, 0x02, 0x04, 0x10, 0x12, 0x1B, 0x1C, 0x43, 0x45, 0x60, 0x62, 0x64, 0x90, 0x92, 0x93,
        0xA1, 0xA3, 0xA5, 0xA7, 0xB1, 0xB2, 0xB8, 0xBE, 0xC6, 0xCA, 0xCF, 0xD0, 0xD2, 0xD5, 0xE2, 0xE4, 0xEF,
        0xF3, 0xF5, 0xF8, 0xF9, 0xFB, 0xFD, 0x06, 0x08, 0x09, 0x0B, 0x0D, 0x70, 0x71, 0x72, 0x73, 0x75, 0x77 };
    for (size_t i = 0; i < sizeof(work); ++i) assert(!standby_api_bbp_passive(work[i]));
    puts("bbp-classification");

    assert(standby_api_http_class(1, "/") == STANDBY_HTTP_PASSIVE);
    assert(standby_api_http_class(1, "/assets/app.js") == STANDBY_HTTP_PASSIVE);
    assert(standby_api_http_class(1, "/api/status") == STANDBY_HTTP_PASSIVE);
    assert(standby_api_http_class(1, "/api/status?x=1") == STANDBY_HTTP_PASSIVE);
    assert(standby_api_http_class(1, "/api/statusx") == STANDBY_HTTP_WORK);
    assert(standby_api_http_class(0, "/api/status") == STANDBY_HTTP_WORK);
    assert(standby_api_http_class(1, "/api/device/info") == STANDBY_HTTP_WORK);
    assert(standby_api_http_class(1, "/api/standby/status") == STANDBY_HTTP_STANDBY);
    assert(standby_api_http_class(0, "/api/standby/presence") == STANDBY_HTTP_STANDBY);
    assert(standby_api_http_class(1, "/api/scope") == STANDBY_HTTP_WORK);
    assert(standby_api_http_class(0, "/api/dac") == STANDBY_HTTP_WORK);
    assert(standby_api_http_class(0, "/api/ota/upload") == STANDBY_HTTP_WORK);
    puts("http-classification");

    /* JSON status */
    StandbyStatus ss; memset(&ss, 0, sizeof(ss));
    ss.state = BB_ST_FAULT_SAFE; ss.stage = 3; ss.generation = 4000000000u; ss.timeout_seconds = 900;
    ss.clients = 2; ss.failed_stage = 5; ss.inhibitors = 0x4001; ss.completed = 6; ss.failed = 32; ss.skipped = 2;
    ss.idle_remaining_ms = 12;
    char js[400];
    size_t n = standby_api_json_status(&ss, js, sizeof(js));
    assert(n > 0 && strlen(js) == n);
    const char *want = "{\"schema\":1,\"state\":\"fault_safe\",\"ready\":false,\"stage\":3,\"generation\":4000000000,"
        "\"timeoutSeconds\":900,\"clients\":2,\"failedStage\":5,\"inhibitors\":16385,\"completed\":6,\"failed\":32,"
        "\"skipped\":2,\"idleRemainingMs\":12}";
    assert(strcmp(js, want) == 0);
    char tiny[10]; assert(standby_api_json_status(&ss, tiny, sizeof(tiny)) == 0 && tiny[0] == 0);
    assert(strcmp(standby_api_state_name(BB_ST_ACTIVE), "active") == 0);
    assert(strcmp(standby_api_state_name(BB_ST_PREPARING), "preparing") == 0);
    assert(strcmp(standby_api_state_name(BB_ST_ASLEEP), "asleep") == 0);
    assert(strcmp(standby_api_state_name(BB_ST_WAKING), "waking") == 0);
    puts("json-status");

    /* strict parsers: exact errors */
    uint32_t id = 0; int present = -1; uint32_t secs = 7;
    #define P(body) standby_api_parse_presence(body, strlen(body), &id, &present)
    assert(P("{\"clientId\":42,\"present\":true}") == NULL && id == 42 && present == 1);
    assert(P(" { \"present\" : false , \"clientId\" : 4294967295 } ") == NULL && id == 4294967295u && present == 0);
    assert(strcmp(P("{\"clientId\":42}"), "missing present") == 0);
    assert(strcmp(P("{\"present\":true}"), "missing clientId") == 0);
    assert(strcmp(P("{\"clientId\":0,\"present\":true}"), "invalid clientId") == 0);
    assert(strcmp(P("{\"clientId\":4294967296,\"present\":true}"), "invalid clientId") == 0);
    assert(strcmp(P("{\"clientId\":1.5,\"present\":true}"), "invalid clientId") == 0);
    assert(strcmp(P("{\"clientId\":true,\"present\":true}"), "invalid clientId") == 0);
    assert(strcmp(P("{\"clientId\":1,\"present\":1}"), "invalid present") == 0);
    assert(strcmp(P("{\"clientId\":1,\"present\":true,\"x\":1}"), "unknown field") == 0);
    assert(strcmp(P("{\"clientId\":1,\"clientId\":2,\"present\":true}"), "duplicate field") == 0);
    assert(strcmp(P(""), "invalid json") == 0);
    assert(strcmp(P("[]"), "invalid json") == 0);
    assert(strcmp(P("{\"clientId\":1,\"present\":true"), "invalid json") == 0);
    assert(strcmp(P("{\"clientId\":1,\"present\":true} x"), "invalid json") == 0);
    assert(strcmp(P("{\"clientId\":-1,\"present\":true}"), "invalid json") == 0);
    assert(strcmp(P("{\"clientId\":1,\"present\":tru}"), "invalid json") == 0);
    assert(strcmp(P("{}"), "missing clientId") == 0);
    assert(standby_api_parse_presence(NULL, 0, &id, &present) != NULL);
    #define Q(body) standby_api_parse_policy(body, strlen(body), &secs)
    assert(Q("{\"timeoutSeconds\":60}") == NULL && secs == 60);
    assert(Q("{\"timeoutSeconds\":0}") == NULL && secs == 0);
    assert(Q("{\"timeoutSeconds\":300}") == NULL && secs == 300);
    assert(Q("{\"timeoutSeconds\":900}") == NULL && secs == 900);
    secs = 7;
    assert(strcmp(Q("{\"timeoutSeconds\":61}"), "invalid timeoutSeconds") == 0 && secs == 7);
    assert(strcmp(Q("{\"timeoutSeconds\":true}"), "invalid timeoutSeconds") == 0);
    assert(strcmp(Q("{}"), "missing timeoutSeconds") == 0);
    assert(strcmp(Q("{\"timeout\":60}"), "unknown field") == 0);
    puts("strict-json");
    return 0;
}
"""


def test_standby_api_surface():
    output = compile_and_run(
        MAIN,
        sources=[
            f"{POWER}/standby_policy.c",
            f"{POWER}/standby_system.c",
            f"{POWER}/standby_api.c",
        ],
        include_dirs=[POWER, BBP],
        extra_flags=["-Werror"],
    )
    assert output.splitlines() == [
        "status-layout",
        "exact-lengths",
        "presence",
        "usb-source",
        "policy",
        "sleep-wake",
        "bbp-classification",
        "http-classification",
        "json-status",
        "strict-json",
    ]
