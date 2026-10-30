# Parser fixtures

Captured engine responses used to test the parsers without touching the network.

Layout: `tests/fixtures/<engine>/<case>.html` (or `.json`, `.xml` for APIs).
Cases worth keeping per engine:

- `good`      — a real results page
- `empty`     — a genuine "no results" page
- `challenge` — a captcha / bot-check page (the blocked detector must fire)

Capture with `OMNI_DUMP=<engine>` while running a search:

    OMNI_DUMP=bing ./bin/omnisearch
    curl "http://localhost:8087/search?q=linux+kernel&engine=bing"
    cp /tmp/dump-bing.html tests/fixtures/bing/good.html

Fixtures are intentionally few and hand-picked: each one pins a layout the
parser depends on, so a silent upstream redesign fails `make fixtures` instead
of reaching a user.
