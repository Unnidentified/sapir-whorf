# The opt-in providers switch (J5.5 tier 3)

The review's tier 3 is: torrent indexes and shadow libraries, *"opt-in only, off
by default, owner's explicit call"*, gated behind `[engines] allow_files = on`
with a `legal_note`, and never reached from the All tab.

This is that gate. It is built now, before any provider exists, because the
alternative is the usual one: a switch and a disclaimer bolted on next to a
working download link, which is the worst moment to ask someone to read it.

## What it does today

**Nothing is registered under it.** No file index, no torrent index, no shadow
library. `engine_count_opt_in()` counts engines with `opt_in = 1` in the
registry, and that is 0, so the page says so in as many words:

> No provider of this kind is registered in this build, so the switch changes
> nothing today. It exists so that the choice is recorded before the providers
> are, and so that the disclaimer is not shown for the first time next to a
> working download link.

The switch is real plumbing, not a placeholder: `SearchEngine.opt_in` exists,
`engine_count_opt_in()` reads it, and `opt_in_providers_allowed()` is the single
function a provider must ask before it fetches anything.

## Two keys in series

| Key | Where | Default |
|---|---|---|
| `[engines] allow_files` | server, restart to change | `off` |
| `opt_in` | cookie, one browser | absent |

`opt_in_providers_allowed()` requires both. So:

- a crafted link cannot turn the providers on for a visitor, because the server
  key is checked first and is not in the request;
- a visitor cannot be opted in by the operator, because the cookie is theirs;
- with the server key off, the control is **not rendered at all** — the page
  shows the reason instead, rather than a switch that would silently do nothing.

## What was got wrong first, and fixed

An unchecked checkbox submits nothing. So the first version had a switch that
could be turned on and never off: every request that could have said "off"
simply omitted the field. The control now ships a companion
`opt_in_present=1`, and present-without-the-box means off. `tests/test_opt_in.sh`
case 5 is that bug.

A second one: `global_config` lived in `Main.c`, so any module asking the config
a question dragged `main()` in with it. It lives in `Config.c` now with
`config_allow_files()` on top.

## Tests

`make optin-check` → `tests/test_opt_in.sh`, six cases, all passing:

1. server refuses → no control, and the page says why;
2. `opt_in=1` cookie on a refusing server → still no control (the cookie cannot
   override the server);
3. server permits → control present, **unchecked**, with the "nothing
   registered" note;
4. opting in → `Set-Cookie: opt_in=1`, and the page shows it checked;
5. present-but-unchecked → `Set-Cookie: opt_in=0` (withdrawable);
6. an unrelated save (`theme=amber`) → the opt-in is untouched.

## The disclaimer

Shown on `/settings` whether or not the control is offered, in English and
Spanish, under "Legal notice — read before enabling". It says, in four
paragraphs: that this program hosts and uploads nothing and merely shows what a
third party returned; that much of what those sites index is copyrighted and
downloading, sharing or deliberately linking to it can be unlawful depending on
jurisdiction and use, with no legal advice given; that enabling it means taking
responsibility for your own use and accepting that the operator provides the
feature without warranty and is not liable for third-party results; and that the
choice is a cookie that can be withdrawn, or disabled for everyone by the
operator.

The text lives in `locales/en_gb.ini` and `locales/es_es.ini`
(`opt_in_disclaimer_1..4`), so an operator can reword it in their language
without patching C — and *should* review it, because it is not legal advice for
their jurisdiction.

## Not this item

The per-hit `index_group` fix for the bridge is in `docs/J4-widgets.md`'s
neighbour commit, not here: it belongs to the bridge (J5.6), and it is the
reason `/settings` no longer claims one independent index when a SearXNG
instance answered with three.

## What is deliberately not here

No provider is implemented, and I did not add one to "prove" the switch. The
review's own recommendation is that Anna's Archive and Library Genesis should
not ship in a default build, most of that material is infringing, and the
domains rotate. Adding a scraper for them to demonstrate a toggle would invert
the point of the toggle. When the owner decides which providers they want, each
one is: `opt_in = 1` in the registry, an `opt_in_providers_allowed()` check in
its fetch path, a probe in `docs/probe-log.tsv`, and exclusion from the All tab
fan-out.
