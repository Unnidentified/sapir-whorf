#!/usr/bin/env python3
"""K6: record the TLS ClientHello a client actually sends.

The point is to compare our fetch path with a real browser without touching a
third party. This is a TCP listener that reads the first bytes of each
connection - the ClientHello - extracts the SNI and the ALPN list, and records
a hash of the whole message. It never completes the handshake, so the client
sees an error; that is fine, the hello is the evidence.

    python3 tests/tls_hello_capture.py --port 8893 --out /tmp/hello.json --count 2
"""
import argparse
import hashlib
import json
import socket
import struct
import sys
import threading


def parse_client_hello(data):
    """Returns {sni, alpn, ciphers, extensions} or None."""
    if len(data) < 5 or data[0] != 0x16:  # handshake
        return None
    # TLS record: type(1) version(2) length(2)
    record_len = struct.unpack(">H", data[3:5])[0]
    body = data[5:5 + record_len]
    if len(body) < 4 or body[0] != 0x01:  # ClientHello
        return None
    hs_len = int.from_bytes(body[1:4], "big")
    hello = body[4:4 + hs_len]
    p = 0
    version = hello[0:2]
    p = 2
    p += 32  # random
    sid_len = hello[p]
    p += 1 + sid_len
    cs_len = struct.unpack(">H", hello[p:p + 2])[0]
    p += 2
    ciphers = list(struct.unpack(">%dH" % (cs_len // 2), hello[p:p + cs_len]))
    p += cs_len
    comp_len = hello[p]
    p += 1 + comp_len

    sni = None
    alpn = []
    extensions = []
    if p + 2 <= len(hello):
        ext_total = struct.unpack(">H", hello[p:p + 2])[0]
        p += 2
        end = min(p + ext_total, len(hello))
        while p + 4 <= end:
            etype, elen = struct.unpack(">HH", hello[p:p + 4])
            p += 4
            ext = hello[p:p + elen]
            p += elen
            extensions.append(etype)
            if etype == 0 and elen > 5:  # SNI
                # list length(2) type(1) name length(2) name
                nlen = struct.unpack(">H", ext[3:5])[0]
                sni = ext[5:5 + nlen].decode("ascii", "replace")
            elif etype == 16 and elen > 2:  # ALPN
                q = 2
                while q < len(ext):
                    n = ext[q]
                    q += 1
                    alpn.append(ext[q:q + n].decode("ascii", "replace"))
                    q += n
    return {
        "version": version.hex(),
        "ciphers": ciphers,
        "extensions": extensions,
        "sni": sni,
        "alpn": alpn,
        "sha256": hashlib.sha256(data[:5 + record_len]).hexdigest(),
        "bytes": len(data[:5 + record_len]),
    }


def serve(port, out, count, timeout):
    results = []
    done = threading.Event()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    srv.settimeout(timeout)

    def loop():
        while len(results) < count and not done.is_set():
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                break
            conn.settimeout(3)
            try:
                data = conn.recv(4096)
            except socket.timeout:
                data = b""
            parsed = parse_client_hello(data)
            if parsed:
                results.append(parsed)
                with open(out, "w") as f:
                    json.dump(results, f, indent=2)
            try:
                conn.close()
            except OSError:
                pass
        done.set()

    t = threading.Thread(target=loop, daemon=True)
    t.start()
    return t, done, results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8893)
    ap.add_argument("--out", default="/tmp/tls_hello.json")
    ap.add_argument("--count", type=int, default=2)
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()
    t, done, results = serve(args.port, args.out, args.count, args.timeout)
    t.join(args.timeout + 2)
    if not results:
        print("no client hello captured", file=sys.stderr)
        return 1
    for r in results:
        print(json.dumps(r, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
