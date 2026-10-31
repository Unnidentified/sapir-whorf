# J4b — translation

Built after a probe, as the review asks, and the probe decided the design.

## The probe

`docs/probe-log.tsv`:

```
2026-10-31T17:27:17Z  mymemory  hello world  en|es  200  1166
{"translatedText":"Hola Mundo","match":1}  omnisearch/1.0  plain  J4b translate probe
```

706 ms, no key, no account. The second probe was the negative one, because an
error that looks like a translation is the failure mode that matters here:

```
GET .../get?q=hello&langpair=en|xx
{"responseData":{"translatedText":"'XX' IS AN INVALID TARGET LANGUAGE ..."},
 "responseStatus":403,"quotaFinished":null}
```

The error text arrives **inside `translatedText`**. So the status is checked
before the text, and a card that only read the text would have shown
`'XX' IS AN INVALID TARGET LANGUAGE` as if it were a translation.

## What it is

A widget kind like any other (J4.0): `translate_widget()` fills `WidgetData`,
`templates/widget_translate.html` renders it. The provider is a **fixed
endpoint** — never a host taken from the query — because the widget runs on the
request path and a user-supplied URL there is an SSRF with a friendly name. The
budget is 900 ms; past it the card says "Provider did not answer" instead of
rendering an empty box.

Detection requires the verb (`translate`, `traduce`, `traducir`). `<x> in
spanish` is a web query far more often than a translation request, and guessing
would answer the wrong one — it stays a search.

## States, all with a test

| request | card |
|---|---|
| `translate hello world to spanish` | `Hola Mundo`, `english → spanish · MyMemory` |
| `traduce good morning al frances` | translates; the marker is `al` |
| `translate hello world to zz` | no card — `zz` is not a language, so it stays a search |
| `translate hello to english` | no card for an English reader: it would be a no-op |
| provider down or slow | card with "Provider did not answer", `ok=0` |
| provider says 403 | card with "No translation for <language>" |

`tests/test_j4_translate.c` covers the parser (16 cases, including `translate to
be or not to be to spanish`, where the last marker wins and only a known
language ends the sentence) and `make translate-check` runs it.

## Not done

- no detection of the **source** language: `from` is `en` unless the caller says
  otherwise, so translating Spanish into German is not wired to the UI yet.
- no language selector on the card; the form carries text and target only
  through the `q` string.
- results are not cached, so a repeat query asks the provider again. MyMemory's
  free tier is per-IP and generous, but this is the obvious next fix.
