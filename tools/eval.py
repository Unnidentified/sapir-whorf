#!/usr/bin/env python3
"""Quality and latency measurement for a running sp-engine instance.

    ./tools/eval.py [base-url] [--strict] [--json] [--pace SECONDS]

--pace sleeps between queries so a benchmark run does not hammer the upstream
engines. Start the server with OMNI_NOBREAKER=1 when benchmarking: the run
otherwise trips the persisted breakers and leaves the instance believing every
source is broken for hours.

Reads tools/queries.tsv (category, query, optional expected host) and reports:
zero-result rate, unique-site diversity, title coverage, duplicate rate,
authoritative hits and latency percentiles. With --strict, exits non-zero when
a threshold is missed.
"""

import json
import re
import statistics
import sys
import time
import urllib.parse
import urllib.request

THRESHOLDS = {
    "zero_rate": 0.02,
    "authority_rate": 0.70,
    "duplicate_rate": 0.01,
    "title_coverage": 0.80,
    "p95_latency": 2.5,
}

AUTHORITY_SUFFIXES = (
    ".gov", ".edu", "wikipedia.org", "rust-lang.org", "stackoverflow.com",
    "python.org", "docs.python.org", "developer.mozilla.org", "postgresql.org",
    "sqlite.org", "cmake.org", "kernel.org", "openbsd.org", "github.com",
    "arxiv.org", "doi.org", "nature.com", "acm.org", "ieee.org", "nih.gov",
    "who.int", "cdc.gov", "nhs.uk",
)

AUTHORITY_CATEGORIES = {"code", "academic", "entity", "operators"}

TWO_LEVEL = {"co.uk", "com.au", "co.jp", "com.br", "co.in", "com.mx", "co.nz",
             "co.za", "com.tr", "com.cn", "com.tw", "co.kr", "com.sg", "ac.uk"}

STOPWORDS = {"the", "and", "for", "with", "how", "what", "when", "where", "who",
             "why", "that", "this", "from", "into", "your", "are", "was", "best",
             "latest", "does", "much", "many"}


def registrable(host):
    host = host.lower().strip(".")
    parts = host.split(".")
    if len(parts) <= 2:
        return host
    suffix = ".".join(parts[-2:])
    if suffix in TWO_LEVEL:
        return ".".join(parts[-3:])
    return suffix


def load_queries(path):
    rows = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            rows.append((parts[0], parts[1], parts[2] if len(parts) > 2 else ""))
    return rows


def fetch(base, query):
    url = base + "/search?q=" + urllib.parse.quote(query)
    start = time.time()
    with urllib.request.urlopen(url, timeout=90) as response:
        body = response.read().decode("utf-8", "ignore")
    return body, time.time() - start


def parse_page(body):
    hosts, titles, urls = [], [], []
    for match in re.finditer(
            r'<span class="url">\s*(.*?)\s*</span>\s*'
            r'<div class="result-title-row">\s*<a href="([^"]*)"[^>]*>(.*?)</a>',
            body, re.S):
        pretty = re.sub(r"\s+", " ", match.group(1)).strip()
        host = pretty.split(" > ")[0].strip()
        if not host or host.startswith("http"):
            host = urllib.parse.urlparse(match.group(2)).hostname or host
        hosts.append(host)
        urls.append(match.group(2))
        titles.append(re.sub(r"\s+", " ", match.group(3)).strip())
    return hosts, titles, urls


def main():
    args = [a for a in sys.argv[1:]]
    strict = "--strict" in args
    as_json = "--json" in args
    args = [a for a in args if not a.startswith("--")]
    base = args[0].rstrip("/") if args else "http://localhost:8087"

    queries = load_queries("tools/queries.tsv")
    rows = []
    for category, query, hint in queries:
        try:
            body, elapsed = fetch(base, query)
        except Exception as exc:  # noqa: BLE001 - report, do not crash
            print("request failed: %s (%s)" % (query, exc), file=sys.stderr)
            rows.append({"category": category, "query": query, "hint": hint,
                         "results": 0, "sites": 0, "coverage": 0.0,
                         "duplicate": 0, "authority": 0, "latency": 0.0,
                         "error": str(exc)})
            continue

        hosts, titles, _ = parse_page(body)
        sites = {registrable(h) for h in hosts if h}
        tokens = [t for t in re.findall(r"[a-z0-9]+", query.lower())
                  if len(t) >= 3 and t not in STOPWORDS]
        covered = sum(1 for t in titles
                      if any(tok in t.lower() for tok in tokens))
        coverage = (covered / len(titles)) if titles else 0.0
        normalized = [re.sub(r"[^a-z0-9 ]", "", t.lower()) for t in titles]
        duplicates = len(normalized) - len(set(normalized))
        authority = 0
        for host in hosts[:5]:
            low = host.lower()
            if any(low == s.lstrip(".") or low.endswith(s) for s in AUTHORITY_SUFFIXES):
                authority = 1
                break
            if hint and registrable(hint) == registrable(low):
                authority = 1
                break

        rows.append({"category": category, "query": query, "hint": hint,
                     "results": len(titles), "sites": len(sites),
                     "coverage": coverage, "duplicate": duplicates,
                     "authority": authority, "latency": elapsed})

    total = len(rows)
    zeros = sum(1 for r in rows if r["results"] == 0)
    considered = [r for r in rows if r["category"] in AUTHORITY_CATEGORIES
                  and r["results"] > 0]
    authority = (sum(r["authority"] for r in considered) / len(considered)
                 if considered else 0.0)
    dup_rate = (sum(r["duplicate"] for r in rows) /
                max(1, sum(r["results"] for r in rows)))
    coverage = (sum(r["coverage"] for r in rows) / total) if total else 0.0
    latencies = sorted(r["latency"] for r in rows if r["latency"] > 0)
    p50 = statistics.median(latencies) if latencies else 0.0
    p95 = latencies[int(len(latencies) * 0.95) - 1] if latencies else 0.0

    summary = {
        "queries": total,
        "zero_rate": zeros / total if total else 0.0,
        "authority_rate": authority,
        "duplicate_rate": dup_rate,
        "title_coverage": coverage,
        "p50_latency": p50,
        "p95_latency": p95,
        "mean_results": statistics.mean(r["results"] for r in rows) if rows else 0,
        "mean_sites": statistics.mean(r["sites"] for r in rows) if rows else 0,
    }

    if as_json:
        print(json.dumps({"summary": summary, "rows": rows}, indent=2))
    else:
        print("sp-engine eval against %s" % base)
        print("%-12s %-38s %3s %3s %5s %4s %4s %6s" %
              ("category", "query", "n", "sites", "cov", "dup", "auth", "time"))
        for r in rows:
            print("%-12s %-38s %3d %3d %5.2f %4d %4d %5.2fs" %
                  (r["category"], r["query"][:38], r["results"], r["sites"],
                   r["coverage"], r["duplicate"], r["authority"], r["latency"]))
        print()
        print("queries=%d  zero=%.1f%%  authority=%.0f%%  dup=%.2f%%  "
              "coverage=%.0f%%  p50=%.2fs  p95=%.2fs  mean_results=%.1f  "
              "mean_sites=%.1f" %
              (total, summary["zero_rate"] * 100, authority * 100,
               dup_rate * 100, coverage * 100, p50, p95,
               summary["mean_results"], summary["mean_sites"]))

    failed = []
    if summary["zero_rate"] > THRESHOLDS["zero_rate"]:
        failed.append("zero_rate %.1f%% > %.0f%%" %
                      (summary["zero_rate"] * 100, THRESHOLDS["zero_rate"] * 100))
    if authority < THRESHOLDS["authority_rate"]:
        failed.append("authority %.0f%% < %.0f%%" %
                      (authority * 100, THRESHOLDS["authority_rate"] * 100))
    if dup_rate > THRESHOLDS["duplicate_rate"]:
        failed.append("duplicate_rate %.2f%% > %.0f%%" %
                      (dup_rate * 100, THRESHOLDS["duplicate_rate"] * 100))
    if coverage < THRESHOLDS["title_coverage"]:
        failed.append("title_coverage %.0f%% < %.0f%%" %
                      (coverage * 100, THRESHOLDS["title_coverage"] * 100))
    if p95 > THRESHOLDS["p95_latency"]:
        failed.append("p95 %.2fs > %.1fs" % (p95, THRESHOLDS["p95_latency"]))

    if failed:
        print("\nFAIL: " + ", ".join(failed))
        return 1 if strict else 0
    print("\nOK: all thresholds met")
    return 0


if __name__ == "__main__":
    sys.exit(main())
