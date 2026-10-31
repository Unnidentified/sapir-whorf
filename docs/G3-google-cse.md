# G3 — Google, after the JSON API closed

## What actually happened to the API

The Programmable Search JSON API (`www.googleapis.com/customsearch/v1`) still
answers for existing keys, but Google stopped issuing new ones in 2026-01 and
the service ends 2027-01-01. Any setup instructions that call it "free, 100
queries a day" are describing a product nobody starting today can buy. That
line is gone from `config.ini`.

## What the Element endpoint does, measured

The review asked for a clean-room `google_cse_element` that uses the widget's
own endpoint. I probed it on 2026-10-31; the results are the reason this engine
ships disabled and why it insists on a token.

| Request | Result |
|---|---|
| `GET cse.google.com/cse?cx=…&q=…` | 200, **1063 bytes**, a shell that loads `/cse.js` and says "enable JavaScript". No results in the HTML. |
| `GET cse.google.com/cse.js?cx=…` | 404 for the cx I could construct. |
| `GET cse.google.com/cse/element/v1?…&cse_tok=` (empty) | **403**, Google's "Sorry…" bot page. |
| `GET programmablesearchengine.google.com/iframe/search?…` | 404. |

So: the results are not in the HTML, and the JSON endpoint refuses any request
without a valid `cse_tok`. That token is minted by Google's obfuscated widget
JavaScript at runtime — it is not in the shell page, and it is not in `cse.js`
until a real `cx` resolves. Minting it server-side would mean reimplementing an
anti-abuse token generator, which is both fragile and exactly the kind of thing
that breaks silently.

## What was built

`google_cse_element`:

- clean-room — written from the endpoint's observed request/response shape,
  nothing copied from SearXNG (which is AGPL) or anywhere else;
- needs `google_cse_cx` **and** `google_cse_tok`. The token is an operator
  paste, taken from a browser's devtools once; it is not something this code
  can invent;
- **disabled by default**, and cannot be enabled without both values;
- parsing is covered by `tests/test_g3_cse_element.c`, including the 403 HTML
  body returning zero rather than a crash or a bogus result.

The parser reads the `cb({...})`-wrapped JSON, takes `titleNoFormatting` /
`unescapedUrl` / `content`, falls back to `title` / `url` /
`contentNoFormatting`, unescapes entities, and drops entries with no link.

## What is not verified, and why

**This has not been run against a live Google element, because no valid `cx`
exists in this environment** — a `cx` comes from a Google account signup, and
the ones I constructed return 404 from `cse.js` and 403 from the element
endpoint. The parser is tested against the endpoint's documented shape, not a
captured live response. That distinction matters and is stated rather than
papered over.

If you have a `cx`:

```
[engines]
google_cse_cx  = <your cx>
google_cse_tok = <paste the cse_tok from a devtools request>
```

and the engine turns on with a log line confirming it. Without the token it
stays off and says so once at startup, instead of spending a request slot per
query to receive a 403.

## The route that does work

`searxng_url` remains the supported way to reach Google's index: a self-hosted
SearXNG relays it server-side, its maintainers track markup changes, and
OmniSearch inherits that for free. That is what `config.ini` recommends.
