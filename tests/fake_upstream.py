#!/usr/bin/env python3
"""Local stand-in for an upstream engine, so tests never touch the network.

Serves the SearXNG JSON shape. Mode comes from a file so a test can flip it
mid-run:  ok -> results, block -> 503 with a challenge body, empty -> no items.
"""
import json, os, sys, threading
from http.server import BaseHTTPRequestHandler, HTTPServer

MODE_FILE = os.environ.get("FAKE_MODE_FILE", "/tmp/fake_upstream.mode")
PORT = int(os.environ.get("FAKE_PORT", "8899"))


def mode():
    try:
        return open(MODE_FILE).read().strip() or "ok"
    except OSError:
        return "ok"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        # every request is counted so a test can assert that none arrived
        try:
            with open(os.environ.get("FAKE_HITS", "/tmp/fake_upstream.hits"), "a") as f:
                f.write(self.path + "\n")
        except OSError:
            pass
        m = mode()
        if m == "retry6":
            # K3: suspended for a short, measurable time
            self.send_response(429)
            self.send_header("Content-Type", "text/html")
            self.send_header("Retry-After", "6")
            body = b"<html><title>429</title>slow down</html>"
        elif m == "block":
            body = b"<html><title>Captcha</title>Just a moment...</html>"
            self.send_response(503)
            self.send_header("Content-Type", "text/html")
            self.send_header("Retry-After", "120")
        elif m == "junk":
            # H2: ten results that answer nothing. HTTP 200, real markup, no
            # overlap with the query - exactly Bing's failure shape.
            results = [
                {
                    "title": f"Warehouse stock transfer error {7000 + i}",
                    "url": f"http://sap{i}.example/notes/{7000 + i}",
                    "content": "Posting change fails, could not determine ERP "
                               "stock data for stock type.",
                }
                for i in range(10)
            ]
            body = json.dumps({"results": results}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
        elif m == "zero":
            # K12.6: HTTP 200 with nothing in it, the shape Yahoo returns when
            # it decides not to serve a query
            body = b""
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
        elif m == "empty":
            body = json.dumps({"results": []}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
        else:
            results = [
                {
                    "title": f"Fake result {i} about linux kernel",
                    "url": f"http://example{i}.com/linux/kernel/{i}",
                    "content": "A fake page used by the offline test harness "
                               "to exercise ranking without touching the network.",
                }
                for i in range(8)
            ]
            body = json.dumps({"results": results}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
