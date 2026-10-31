#!/usr/bin/env python3
"""H2: measure Bing's junk rate instead of theorising about it.

One request per cell, paced (>=60 s between requests to the same engine), each
SERP recorded in the A3 replay format so the *production ranker* decides what
is junk. A result under the coverage floor is what the floor already drops; a
query where >=90% of results are under it is JUNK.

    tools/junk_rate.py --engine bing --variant baseline --group junk
    tools/junk_rate.py --engine bing --variant all --group all
    tools/junk_rate.py --summarise

Every request is appended to docs/probe-log.tsv, like tools/probe.sh.
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_DIR = os.path.join(ROOT, "tests", "junk-ab")
PROBE_LOG = os.path.join(ROOT, "docs", "probe-log.tsv")
RANKER = os.path.join(ROOT, "tests", "offline_rank")

UA_SAFARI = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
             "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.2 Safari/605.1.15")

# The cells from the review's table. `params` are appended to the SERP URL.
VARIANTS = {
    "baseline":  dict(params="",                 accept_language="en-GB,en;q=0.9"),
    "adlt_off":  dict(params="&adlt=off",        accept_language="en-GB,en;q=0.9"),
    "ensearch":  dict(params="&ensearch=1",      accept_language="en-GB,en;q=0.9"),
    "es_accept": dict(params="",                 accept_language="es-ES,es;q=0.9,en;q=0.8"),
    "setlang":   dict(params="&setlang=en&cc=us", accept_language="en-US,en;q=0.9"),
}

BASES = {
    "bing": "https://www.bing.com/search?q={q}",
    "yahoo": "https://search.yahoo.com/search?p={q}",
    "ddg": "https://html.duckduckgo.com/html/?q={q}",
}


def parse_queries(group):
    rows = []
    with open(os.path.join(HERE, "queries-bing-ab.tsv")) as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 2:
                continue
            g, q = parts[0].strip(), parts[1].strip()
            if group in ("all", g):
                rows.append((g, q))
    return rows


def log_probe(engine, query, code, size, verdict, variant, url):
    new = not os.path.exists(PROBE_LOG)
    with open(PROBE_LOG, "a") as fh:
        if new:
            fh.write("utc\tengine\tquery\thttp\tbytes\tverdict\tua\theaders\turl\n")
        fh.write("\t".join([
            time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            engine, query, str(code), str(size), verdict, "safari",
            variant, url,
        ]) + "\n")


def parse_bing(html):
    """Minimal extraction: enough to feed the ranker, not a production parser."""
    import re
    out = []
    for m in re.finditer(r'<li class="b_algo".*?</li>', html, re.S):
        block = m.group(0)
        href = re.search(r'href="(https?://[^"]+)"', block)
        title = re.search(r'<h2[^>]*>(.*?)</h2>', block, re.S)
        if not href:
            continue
        clean = lambda s: re.sub(r"<[^>]+>", "", s or "").strip()
        snippet = re.search(r'<p[^>]*>(.*?)</p>', block, re.S)
        out.append({
            "url": href.group(1),
            "title": clean(title.group(1)) if title else "",
            "snippet": clean(snippet.group(1)) if snippet else "",
        })
    return out


def parse_yahoo(html):
    import re
    out = []
    for m in re.finditer(r'<div class="algo-sr".*?(?=<div class="algo-sr"|$)', html, re.S):
        block = m.group(0)
        href = re.search(r'href="(https?://[^"]+)"', block)
        if not href:
            continue
        title = re.search(r'<h3[^>]*>(.*?)</h3>', block, re.S)
        snippet = re.search(r'<div class="compText[^"]*"[^>]*>(.*?)</div>', block, re.S)
        clean = lambda s: re.sub(r"<[^>]+>", "", s or "").strip()
        out.append({"url": href.group(1),
                    "title": clean(title.group(1)) if title else "",
                    "snippet": clean(snippet.group(1)) if snippet else ""})
    return out


def parse_ddg(html):
    import re
    out = []
    for m in re.finditer(r'<a[^>]*class="result__a"[^>]*href="([^"]+)"[^>]*>(.*?)</a>', html, re.S):
        url, title = m.group(1), re.sub(r"<[^>]+>", "", m.group(2)).strip()
        out.append({"url": url, "title": title, "snippet": ""})
    return out


PARSERS = {"bing": parse_bing, "yahoo": parse_yahoo, "ddg": parse_ddg}


def fetch(engine, query, variant, pace, curl_seen):
    v = VARIANTS[variant]
    url = BASES[engine].format(q=urllib.parse.quote(query)) + v["params"]
    cmd = ["curl", "-sS", "-L", "-m", "30", "--compressed",
           "-A", UA_SAFARI,
           "-H", f"Accept-Language: {v['accept_language']}",
           "-H", "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
           "-w", "\n%{http_code} %{size_download}",
           "-o", "-", url]
    now = time.time()
    last = curl_seen.get(engine, 0)
    wait = pace - (now - last)
    if last and wait > 0:
        sys.stderr.write(f"  pacing {engine}: {int(wait)}s\n")
        time.sleep(wait)
    curl_seen[engine] = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True)
    body = proc.stdout
    code, size = "000", "0"
    if "\n" in body:
        body, tail = body.rsplit("\n", 1)
        bits = tail.split()
        if len(bits) == 2:
            code, size = bits
    log_probe(engine, query, code, size, f"junk-ab:{variant}", variant, url)
    return body, code, size


LOCK_PATH = "/tmp/omnisearch-junk-ab.lock"


def take_lock():
    """H5: two runners on one cell set is a burst, not a measurement. Refuse to
    start when another one is live."""
    try:
        fd = os.open(LOCK_PATH, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        try:
            pid = int(open(LOCK_PATH).read().strip())
        except Exception:
            pid = 0
        try:
            os.kill(pid, 0)
            print(f"another junk_rate.py is running (pid {pid}); refusing",
                  file=sys.stderr)
            return False
        except OSError:
            os.unlink(LOCK_PATH)  # stale
            return take_lock()
    os.write(fd, str(os.getpid()).encode())
    os.close(fd)
    import atexit
    atexit.register(lambda: os.path.exists(LOCK_PATH) and os.unlink(LOCK_PATH))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default="bing", choices=sorted(BASES))
    ap.add_argument("--variant", default="baseline", choices=sorted(VARIANTS) + ["all"])
    ap.add_argument("--group", default="all", choices=["all", "junk", "good"])
    ap.add_argument("--pace", type=int, default=60, help="seconds between requests to one engine")
    ap.add_argument("--summarise", action="store_true")
    args = ap.parse_args()

    if args.summarise:
        return summarise()

    if not take_lock():
        return 3

    os.makedirs(OUT_DIR, exist_ok=True)
    queries = parse_queries(args.group)
    variants = sorted(VARIANTS) if args.variant == "all" else [args.variant]
    curl_seen = {}

    for variant in variants:
        path = os.path.join(OUT_DIR, f"{args.engine}-{variant}.jsonl")
        done = set()
        if os.path.exists(path):
            with open(path) as fh:
                for line in fh:
                    try:
                        done.add(json.loads(line)["q"])
                    except Exception:
                        pass
        with open(path, "a") as out:
            for group, query in queries:
                if query in done:
                    continue
                sys.stderr.write(f"[{args.engine}/{variant}] {query}\n")
                body, code, size = fetch(args.engine, query, variant, args.pace, curl_seen)
                docs = PARSERS[args.engine](body) if code == "200" else []
                out.write(json.dumps({"q": query, "engine": args.engine,
                                      "page": 1, "results": docs}) + "\n")
                out.flush()
                sys.stderr.write(f"  http={code} bytes={size} docs={len(docs)}\n")
    return summarise()


def summarise():
    if not os.path.isfile(RANKER):
        print(f"missing {RANKER} - run `make tests/offline_rank` first",
              file=sys.stderr)
        return 2
    if not os.path.isdir(OUT_DIR):
        print("no data yet")
        return 0
    rows = []
    for name in sorted(os.listdir(OUT_DIR)):
        if not name.endswith(".jsonl"):
            continue
        path = os.path.join(OUT_DIR, name)
        if os.path.getsize(path) == 0:
            continue
        proc = subprocess.run([RANKER, "--junk", path], capture_output=True, text=True)
        junk = ok = 0
        for line in proc.stdout.splitlines():
            if line.startswith("offline replay") or line.startswith("  "):
                continue
            if "JUNK" in line:
                junk += 1
            elif line.endswith("ok"):
                ok += 1
        if junk + ok:
            rows.append((name[:-6], junk, ok, 100.0 * junk / (junk + ok)))
    if not rows:
        print("no completed cells")
        return 0
    print(f"{'cell':<24}{'junk':>6}{'ok':>6}{'junk %':>9}")
    for cell, junk, ok, pct in rows:
        print(f"{cell:<24}{junk:>6}{ok:>6}{pct:>8.0f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
