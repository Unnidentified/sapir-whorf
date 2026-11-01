#!/usr/bin/env python3
"""A local stand-in for Reddit's OAuth endpoints, so L5's tests never touch it.

Serves POST /api/v1/access_token and GET /search (and /r/<sub>/search). The mode
file flips the behaviour mid-run:

  ok        a token and 25 posts
  expired   a token whose expires_in is 1 s, so the next search renews
  onetime   counts token requests: a single-flight test reads that count
  unauthorized  401 on the search, twice (so one refresh and one retry both fail)
  ratelimit 429 with X-Ratelimit-Remaining: 0 and Reset: 30
  errorjson 200 with {"error": ...} - a failure, not a result
  html      200 with an HTML body - a block page shapelessly disguised
  sub       a token and posts, recording the URL it was asked for

Every request is appended to FAKE_HITS as "METHOD path" plus, for the token
request, nothing else: the header carrying the credential is never written.
"""
import json, os, time

from http.server import BaseHTTPRequestHandler, HTTPServer

MODE_FILE = os.environ.get("FAKE_MODE_FILE", "/tmp/reddit.mode")
PORT = int(os.environ.get("FAKE_PORT", "8891"))
HITS = os.environ.get("FAKE_HITS", "/tmp/reddit.hits")


def mode():
    try:
        return open(MODE_FILE).read().strip() or "ok"
    except OSError:
        return "ok"


def hit(line):
    try:
        with open(HITS, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass


def posts(n=25, sub="rust"):
    return [
        {
            "kind": "t3",
            "data": {
                "title": f"What is the best IDE for Rust? ({i})",
                "permalink": f"/r/{sub}/comments/abc{i}/what_is_the_best_ide/",
                "subreddit_name_prefixed": f"r/{sub}",
                "author": f"user{i}",
                "selftext": "A discussion of editors and tooling, with opinions "
                            "and a few benchmarks." if i % 2 == 0 else "",
                "domain": "reddit.com" if i % 2 == 0 else "blog.example",
                "score": 120 + i,
                "num_comments": 40 + i,
                "created_utc": 1730000000 + i * 3600,
                "over_18": False,
            },
        }
        for i in range(n)
    ]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, status, body, ctype="application/json", extra=None):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        hit("POST " + self.path)
        # the Authorization header is deliberately not recorded: it carries the
        # base64 of the client id and, in a logged-in app, the secret
        if "installed_client" not in body:
            self._send(400, '{"error":"unsupported_grant_type"}')
            return
        m = mode()
        ttl = 1 if m == "expired" else 3600
        self._send(200, json.dumps({
            "access_token": "fake-access-token",
            "token_type": "bearer",
            "expires_in": ttl,
            "scope": "read",
        }))

    def do_GET(self):
        hit("GET " + self.path)
        m = mode()
        if m == "unauthorized":
            self._send(401, '{"error":"invalid_token"}')
            return
        if m == "ratelimit":
            self._send(429, '{"error":"rate limited"}',
                       extra={"X-Ratelimit-Remaining": "0",
                              "X-Ratelimit-Reset": "30"})
            return
        if m == "errorjson":
            self._send(200, '{"error":"something went wrong"}')
            return
        if m == "html":
            self._send(200, "<html><body>blocked</body></html>", "text/html")
            return
        self._send(200, json.dumps({
            "kind": "Listing",
            "data": {"children": posts(), "after": None},
        }), extra={"X-Ratelimit-Remaining": "59",
                   "X-Ratelimit-Used": "1",
                   "X-Ratelimit-Reset": "58"})


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
