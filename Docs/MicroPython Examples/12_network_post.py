# 12_network_post.py — On-device script: read ADC and POST the value to an HTTP endpoint.
#
# Purpose: demonstrate bugbuster.http_post() from a running on-device script.
#   Reads channel 0 ADC voltage and ships the reading to httpbin.org/post as JSON.
#
# Prerequisite: channel 0 must already be configured as FUNC_VIN before this
#   eval runs. You can do that in a prior persistent-mode eval:
#     s.eval("import bugbuster; ch = bugbuster.Channel(0); ch.set_function(bugbuster.FUNC_VIN)")
#   If running standalone (non-persistent), add those lines before the read below.
#
# Run: POST /api/scripts/eval
#   curl -X POST http://<ip>/api/scripts/eval \
#        -H "Content-Type: text/plain" \
#        -H "X-BugBuster-Admin-Token: TOKEN" \
#        --data-binary @12_network_post.py
#
#   or via the Python client:
#     python -m bugbuster.script --host <ip> --token TOKEN run 12_network_post.py
#
# Expect:
#   status=200
#   body_slice={"args": {}, "data": "{\"channel\":0,\"voltage\":
#   (Exact voltage depends on hardware state; no load → near-zero expected.)
#
# Network notes:
#   - TLS works out of the box: the firmware bundles the Mozilla CA bundle so
#     HTTPS URLs are verified without extra configuration.
#   - bugbuster.http_get(url, headers=None, timeout_ms=10000) is also available.
#   - Cancellation is timeout-only; there is no per-call cancel mechanism.
#   - The device must have an active WiFi connection (see cmd_wifi / WebUI).

import bugbuster

ch = bugbuster.Channel(0)
ch.set_function(bugbuster.FUNC_VIN)
bugbuster.sleep(50)
voltage = ch.read_voltage()

# No json module on MicroPython device — build payload with string formatting.
payload = bytes('{"channel":0,"voltage":%.5f}' % voltage, "utf-8")

try:
    r = bugbuster.http_post(
        "https://httpbin.org/post",
        body=payload,
        headers={"Content-Type": "application/json"},
    )
    print("status=%d" % r.status)
    print("body_slice=%s" % str(r.body[:80], "utf-8"))
except OSError as e:
    print("network error: %s" % e)
finally:
    ch.set_function(bugbuster.FUNC_HIGH_IMP)
