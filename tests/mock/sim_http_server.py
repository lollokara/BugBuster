"""
SimHTTPServer — WSGI wrapper around SimulatedDevice for full-stack HTTP testing.

Starts a real localhost HTTP server backed by SimulatedDevice so the Python
HTTPTransport exercises actual requests serialization, header handling, and
HTTP status-code mapping instead of the in-process bypass used by --sim.

Usage in pytest fixtures (--sim-full mode):

    server = SimHTTPServer(device, admin_token=TOKEN)
    server.start()
    transport = HTTPTransport("127.0.0.1", port=server.port, admin_token=TOKEN)
    # ...
    server.stop()
"""

import json
import logging
import threading
import urllib.parse
from socketserver import ThreadingMixIn
from wsgiref.simple_server import WSGIServer, WSGIRequestHandler

log = logging.getLogger(__name__)


class _SilentHandler(WSGIRequestHandler):
    """Suppress the default wsgiref access-log noise in test output."""

    def log_message(self, fmt, *args):
        pass


class _ThreadingWSGIServer(ThreadingMixIn, WSGIServer):
    """Allow concurrent requests so pytest fixtures don't deadlock."""
    daemon_threads = True


def _make_wsgi_app(device):
    """Return a WSGI callable that delegates to device.http_dispatch()."""

    def app(environ, start_response):
        method = environ["REQUEST_METHOD"]
        path = environ.get("PATH_INFO", "/")

        # Parse query string into a flat dict (first value wins per key).
        qs = urllib.parse.parse_qs(environ.get("QUERY_STRING", ""), keep_blank_values=True)
        params = {k: v[0] for k, v in qs.items()}

        # Read and parse JSON body.
        try:
            length = int(environ.get("CONTENT_LENGTH") or 0)
            raw = environ["wsgi.input"].read(length) if length > 0 else b""
            body = json.loads(raw) if raw else {}
        except (ValueError, json.JSONDecodeError):
            body = {}

        # Reconstruct headers from HTTP_* environ keys.
        headers: dict[str, str] = {}
        for key, val in environ.items():
            if key.startswith("HTTP_"):
                header_name = key[5:].replace("_", "-").title()
                headers[header_name] = val

        try:
            result = device.http_dispatch(method, path, params, body, headers)
        except Exception as exc:
            log.exception("http_dispatch raised: %s", exc)
            result = {"error": str(exc), "code": 500}

        # Map embedded "code" to HTTP status; errors without a code default to 400.
        if "error" in result:
            status_code = result.get("code", 400)
        else:
            status_code = 200

        body_bytes = json.dumps(result).encode()
        status_line = f"{status_code} {'OK' if status_code == 200 else 'Error'}"
        start_response(status_line, [
            ("Content-Type", "application/json"),
            ("Content-Length", str(len(body_bytes))),
        ])
        return [body_bytes]

    return app


class SimHTTPServer:
    """
    Localhost HTTP server backed by a SimulatedDevice.

    The server binds to 127.0.0.1 on an OS-assigned port (port=0) and runs on
    a daemon thread so it is automatically stopped when the process exits even
    if stop() is not called.
    """

    def __init__(self, device, admin_token: str | None = None):
        self._device = device
        self._admin_token = admin_token
        self._server: _ThreadingWSGIServer | None = None
        self._thread: threading.Thread | None = None
        self._port: int | None = None

    @property
    def port(self) -> int:
        if self._port is None:
            raise RuntimeError("Server has not been started yet")
        return self._port

    @property
    def base_url(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    def start(self) -> str:
        """Start the server on a background thread. Returns the base URL."""
        if self._admin_token is not None:
            self._device.admin_token = self._admin_token

        app = _make_wsgi_app(self._device)
        self._server = _ThreadingWSGIServer(("127.0.0.1", 0), _SilentHandler)
        self._server.set_app(app)
        self._port = self._server.server_address[1]

        self._thread = threading.Thread(
            target=self._server.serve_forever,
            name=f"SimHTTPServer-{self._port}",
            daemon=True,
        )
        self._thread.start()
        log.debug("SimHTTPServer started at %s", self.base_url)
        return self.base_url

    def stop(self) -> None:
        """Shut down the server and wait for the thread to exit."""
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        log.debug("SimHTTPServer stopped")
