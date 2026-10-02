"""TR-11b: a failed HTTP operation must surface as an error in every client,
whether the firmware answers 4xx (new) or 200 {"ok": false} (old firmware).

A: Python raised only on 4xx, so 200 + ok:false returned silently.
"""

import pytest

from bugbuster.transport import http as http_mod


class _Resp:
    def __init__(self, status, body):
        self.status_code = status
        self._body = body
        self.text = str(body)
        self.reason = "Bad Request" if status >= 400 else "OK"
        self.url = "http://x/api/y"

    def json(self):
        return self._body

    def raise_for_status(self):
        if self.status_code >= 400:
            import requests
            raise requests.HTTPError(f"{self.status_code} Client Error", response=self)


class _Session:
    def __init__(self, resp):
        self.resp = resp

    def post(self, *a, **k):
        return self.resp

    def delete(self, *a, **k):
        return self.resp


def _transport(resp):
    t = http_mod.HTTPTransport("127.0.0.1")
    t._session = _Session(resp)
    return t


@pytest.mark.parametrize("status", [200, 400])
def test_post_failure_raises_with_device_message(status):
    t = _transport(_Resp(status, {"ok": False, "error": "channel must be 0-3"}))
    with pytest.raises(http_mod.HTTPLogicalError, match="channel must be 0-3"):
        t.post("/channel/9/dac", {"value": 1})


def test_delete_ok_false_raises():
    t = _transport(_Resp(200, {"ok": False, "err": "not found"}))
    with pytest.raises(http_mod.HTTPLogicalError, match="not found"):
        t.delete("/scripts/files")


def test_post_success_passes_through():
    t = _transport(_Resp(200, {"ok": True, "value": 1}))
    assert t.post("/x", {}) == {"ok": True, "value": 1}
