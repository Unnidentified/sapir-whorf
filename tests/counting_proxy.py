#!/usr/bin/env python3
"""A proxy that makes no requests and records every host asked for.

The toggle test needs to know what would have left the machine, not what came
back. Pointing the server at this proxy means every outbound connection is one
line in a log, and nothing external is touched - so the test also runs offline.

Handles CONNECT (https) and absolute-URI GET (http), which is all curl uses.
"""
import os
from http.server import BaseHTTPRequestHandler, HTTPServer

LOG = os.environ.get("PROXY_LOG", "/tmp/counting_proxy.log")
PORT = int(os.environ.get("PROXY_PORT", "8898"))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def _record(self, host):
        with open(LOG, "a") as f:
            f.write(host + "\n")

    def do_CONNECT(self):
        self._record(self.path.split(":")[0])
        self.send_response(502)
        self.end_headers()

    def do_GET(self):
        host = ""
        if self.path.startswith("http://"):
            host = self.path[7:].split("/")[0].split(":")[0]
        else:
            host = (self.headers.get("Host") or "").split(":")[0]
        self._record(host)
        self.send_response(502)
        self.send_header("Content-Length", "0")
        self.end_headers()


if __name__ == "__main__":
    if os.path.exists(LOG):
        os.remove(LOG)
    open(LOG, "w").close()
    srv = HTTPServer(("127.0.0.1", PORT), Handler)
    print(f"counting proxy on 127.0.0.1:{PORT} -> {LOG}", flush=True)
    srv.serve_forever()
