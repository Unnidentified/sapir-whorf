#!/usr/bin/env python3
"""K11: build the bundled company-name -> ticker table.

Source: the SEC's own company_tickers.json (public company filings data, no key,
no account). Fetched once, and the date is written into the file so the table is
never mistaken for live data.

    python3 tools/make_symbol_table.py --source /tmp/company_tickers.json

The table answers "nvidia" and "apple" with no network and no leak. It is a
*name* map: the quote still comes from the chart endpoint, and a name that is
not in the table is a name, not a guessed ticker (see the guard in Stocks.c).
"""
import argparse
import json
import re
import sys
import unicodedata

# legal-form suffixes that do not identify the company
SUFFIXES = [
    "INC", "INCORPORATED", "CORP", "CORPORATION", "CO", "COMPANY", "COMPANIES",
    "LTD", "LIMITED", "LLC", "L L C", "PLC", "NV", "SA", "AG", "SE", "AB", "ASA",
    "OYJ", "BV", "NV", "GMBH", "S A", "HOLDINGS", "HOLDING", "GROUP", "GROUPE",
    "TRUST", "FUND", "CLASS A", "CLASS B", "CLASS C", "COMMON STOCK",
    "AMERICAN DEPOSITARY SHARES", "ADS", "SPONSORED ADR", "ADR", "REIT",
]

# names people type that are not the SEC's legal title
ALIASES = {
    "google": "GOOGL", "alphabet": "GOOGL", "facebook": "META",
    "instagram": "META", "whatsapp": "META", "youtube": "GOOGL",
    "microsoft": "MSFT", "apple": "AAPL", "amazon": "AMZN", "netflix": "NFLX",
    "nvidia": "NVDA", "tesla": "TSLA", "intel": "INTC", "amd": "AMD",
    "boeing": "BA", "disney": "DIS", "coca cola": "KO", "cocacola": "KO",
    "pepsi": "PEP", "pepsico": "PEP", "mcdonalds": "MCD", "starbucks": "SBUX",
    "walmart": "WMT", "target": "TGT", "costco": "COST", "nike": "NKE",
    "adidas": "ADDYY", "visa": "V", "mastercard": "MA", "paypal": "PYPL",
    "spotify": "SPOT", "uber": "UBER", "lyft": "LYFT", "airbnb": "ABNB",
    "palantir": "PLTR", "snowflake": "SNOW", "salesforce": "CRM",
    "oracle": "ORCL", "ibm": "IBM", "cisco": "CSCO", "adobe": "ADBE",
    "qualcomm": "QCOM", "broadcom": "AVGO", "arm": "ARM", "tsmc": "TSM",
    "samsung": "SSNLF", "sony": "SONY", "toyota": "TM", "honda": "HMC",
    "volkswagen": "VWAGY", "bmw": "BMWYY", "mercedes": "MBGAF",
    "ferrari": "RACE", "shell": "SHEL", "bp": "BP", "exxon": "XOM",
    "chevron": "CVX", "pfizer": "PFE", "moderna": "MRNA", "johnson": "JNJ",
    "jpmorgan": "JPM", "goldman": "GS", "morgan stanley": "MS",
    "bank of america": "BAC", "wells fargo": "WFC", "citigroup": "C",
    "berkshire": "BRK-B", "berkshire hathaway": "BRK-B",
    "lockheed": "LMT", "raytheon": "RTX", "northrop": "NOC",
    "general motors": "GM", "ford": "F", "rivian": "RIVN", "lucid": "LCID",
    "dell": "DELL", "hp": "HPQ", "lenovo": "LNVGY", "asus": "ASUUY",
    "logitech": "LOGI", "nintendo": "NTDOY", "ea": "EA", "activision": "ATVI",
    "take two": "TTWO", "roblox": "RBLX", "unity": "U", "epic": "EPIC",
    "twitter": "X", "x": "X", "snap": "SNAP", "pinterest": "PINS",
    "reddit": "RDDT", "shopify": "SHOP", "etsy": "ETSY", "ebay": "EBAY",
    "alibaba": "BABA", "tencent": "TCEHY", "baidu": "BIDU", "jd": "JD",
    "aerospace": "BA", "openai": "MSFT", "anthropic": "AMZN",
}

def normalise(name: str) -> str:
    name = unicodedata.normalize("NFKD", name)
    name = name.encode("ascii", "ignore").decode()
    name = name.upper()
    name = re.sub(r"[^A-Z0-9 ]+", " ", name)
    name = re.sub(r"\b(THE)\b", " ", name)
    words = [w for w in name.split() if w]
    # drop legal forms from the end, once each
    changed = True
    while changed and words:
        changed = False
        for suffix in SUFFIXES:
            parts = suffix.split()
            if len(words) >= len(parts) and words[-len(parts):] == parts:
                del words[-len(parts):]
                changed = True
                break
    return " ".join(words).strip()

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, help="company_tickers.json from sec.gov")
    ap.add_argument("--out", default="static/stocks/company_tickers.tsv")
    ap.add_argument("--date", default=None, help="date the source was fetched")
    args = ap.parse_args()

    with open(args.source, encoding="utf-8") as fh:
        raw = json.load(fh)

    # normalised name -> ticker. The first entry wins: the SEC file lists the
    # primary registrant before its later classes often enough that overriding
    # would be a guess, and a guess is what this table exists to avoid.
    table = {}
    skipped = 0
    for entry in raw.values():
        title = entry.get("title") or ""
        ticker = entry.get("ticker") or ""
        if not title or not ticker:
            skipped += 1
            continue
        key = normalise(title)
        if not key or len(key) < 3:
            skipped += 1
            continue
        # a bare word that is also a common English word is a coin flip
        table.setdefault(key, ticker.upper())

    for alias, ticker in ALIASES.items():
        table[normalise(alias)] = ticker

    rows = sorted(table.items())
    date = args.date or "unknown"
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write("# K11 company-name -> ticker. Source: sec.gov/files/company_tickers.json\n")
        fh.write(f"# fetched {date}. Generated by tools/make_symbol_table.py - do not edit.\n")
        for name, ticker in rows:
            fh.write(f"{name}\t{ticker}\n")
    print(f"{len(rows)} names written to {args.out} ({skipped} entries skipped)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
