"""PLT-05: WiFi STA connect sequence (net/wifi_connect_seq.cpp) driven with
recording fakes. No board WiFi changes (owner chose B: code + host tests only).

A (sequence extracted verbatim from wifi_manager.cpp, no behaviour change):
  1. the device switches to STA-only for the attempt, dropping the AP that HTTP
     clients and the phone app use for the whole (up to ~60 s) attempt;
  2. on failure the WRONG ssid stays as the runtime STA config, so the
     reconnect worker keeps chasing it instead of the previous network.
B: the AP stays up (AP+STA throughout), and a failed attempt restores the
   previous STA config; credentials are persisted only on success."""

from tests.firmware_host.fwhost import compile_and_run

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "wifi_connect_seq.h"
static bool g_sta_only = false, g_ap_up = true, g_started = true, g_ap_ever_down = false;
static char g_ssid[64] = "HomeNet", g_pass[65] = "goodpass";
static int g_saves = 0; static bool g_reachable = false;
static void upd(void) { bool up = g_started && !g_sta_only; if (!up) g_ap_ever_down = true; g_ap_up = up; }
static void disconnect(void) {}
static void stop(void) { g_started = false; upd(); }
static void start(void) { g_started = true; upd(); }
static void set_mode(bool sta_only) { g_sta_only = sta_only; upd(); }
static void set_sta(const char *s, const char *p) { strncpy(g_ssid, s, 63); strncpy(g_pass, p ? p : "", 64); }
static void get_sta(char *s, size_t ss, char *p, size_t ps) { strncpy(s, g_ssid, ss - 1); s[ss-1] = 0; strncpy(p, g_pass, ps - 1); p[ps-1] = 0; }
static bool try_connect(uint32_t) { return g_reachable && strcmp(g_ssid, "NewNet") == 0; }
static void reapply_ap(void) {}
static bool save(const char *, const char *) { g_saves++; return true; }
static void dly(uint32_t) {}
static const WifiConnectOps OPS = { disconnect, stop, start, set_mode, set_sta, get_sta, try_connect, reapply_ap, save, dly };

int main(void) {
    g_reachable = false; g_ap_ever_down = false;
    bool ok = wifi_connect_seq(&OPS, "NewNet", "x");
    printf("fail ok=%d ap_dropped=%d sta=%s saves=%d\n", ok, g_ap_ever_down, g_ssid, g_saves);
    g_reachable = true; g_ap_ever_down = false;
    ok = wifi_connect_seq(&OPS, "NewNet", "x");
    printf("good ok=%d ap_dropped=%d sta=%s saves=%d\n", ok, g_ap_ever_down, g_ssid, g_saves);
    return 0;
}
"""


def _run() -> list[str]:
    out = compile_and_run(MAIN, cxx=True, sources=["Firmware/ESP32/src/net/wifi_connect_seq.cpp"],
                          include_dirs=["Firmware/ESP32/src/net"])
    return out.strip().splitlines()


def test_failed_connect_keeps_ap_and_restores_previous_sta():
    assert _run()[0] == "fail ok=0 ap_dropped=0 sta=HomeNet saves=0"


def test_successful_connect_keeps_ap_and_saves():
    assert _run()[1] == "good ok=1 ap_dropped=0 sta=NewNet saves=1"
