"""POST channel 0's voltage as JSON to a web server.

Edit URL for your server (the board must be on a network that reaches it). A
failed request is reported, not fatal.
"""
import bugbuster

URL = "http://192.168.1.10:8080/readings"

ch = bugbuster.Channel(0)
ch.set_function(bugbuster.FUNC_VIN)
bugbuster.sleep(100)
body = '{"ch0": %.4f}' % ch.read_voltage()
ch.set_function(bugbuster.FUNC_HIGH_IMP)
try:
    r = bugbuster.http_post(URL, body.encode(), headers={"Content-Type": "application/json"},
                            timeout_ms=5000)
    print("status", r.status, r.body[:80])
except OSError as e:
    print("request failed:", e)
