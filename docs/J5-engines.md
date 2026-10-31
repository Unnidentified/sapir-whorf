# J5.1 — what the framework has to change, measured

The review's plan is right (build the framework once so an engine is ~10 lines
of data, then add engines in tiers). This records the exact cost of the first
step, so the next round starts from numbers instead of a guess, plus one probed
engine ready to be its reference implementation.

## The blocker: `ENGINE_COUNT` is compile-time and load-bearing

```
$ grep -rn "ENGINE_REGISTRY\|ENGINE_COUNT" src/ | wc -l
114
```

Three of those are not loops — they are **stack arrays sized by the count**:

| site | declaration |
|---|---|
| `Routes/Search.c` | `ScrapeJob jobs[ENGINE_COUNT]`, `SearchResult *all_results[ENGINE_COUNT]` |
| `Routes/SettingsSave.c` | `char selected_ids[ENGINE_COUNT][32]` |
| `Routes/Search.c` | `const SearchEngine *eligible[ENGINE_COUNT]`, `const char *groups[ENGINE_COUNT]` |

`ScrapeJob` is ~2 KB, so `jobs[]` alone is ~26 KB today and would be ~300 KB at
150 engines — on a thread whose stack the review already flags as small on
macOS. So the migration is not "make the array dynamic": it is

1. move the heap-sized-by-**selected** arrays off the stack (6–8 per request,
   not one per registered engine),
2. replace `ENGINE_COUNT` in loops with a runtime `engine_count`, keeping a
   compile-time `MAX_ENGINES` only as the allocation cap,
3. move the `1u << i` masks from engine indices to job indices (they are
   already job indices in the fusion loop, but `eligible[]` uses engine
   indices),
4. then the registry can be a heap pointer and `static/engines.d/*.ini` can
   append to it.

Steps 1–3 touch every one of those 114 sites. That is a round of its own, and
doing it in the middle of a round that is also adding engines is how a working
tree ends up half-migrated.

## The one thing that makes "~10 lines of data" work: identity

`BodyParserFn` and `UrlBuilderFn` take no engine identity:

```c
typedef int (*BodyParserFn)(const char *body, size_t len, const char *lang,
                            SearchResult **out, int max_results);
```

A generic JSON parser needs to know **which** definition to apply (result path,
field paths, URL prefix). So a def-driven engine needs either one trampoline
per def (which defeats the purpose) or the definition carried to the call. The
cheapest correct shape is a `const void *def` field on `SearchEngine` plus a
thread-local "current engine" set by the scraper before it calls the parser —
the codebase already uses `__thread` for the calculator's working buffer, and
the scrape path is one thread per job.

## The reference engine, probed

Open Library is Tier 1 in the review's list, keyless, and documented. One paced
request, recorded in `docs/probe-log.tsv`:

```
GET https://openlibrary.org/search.json?q=linux+kernel&limit=5
    &fields=key,title,author_name,first_publish_year,cover_i,subject
→ 200, 1809 bytes, 0.76 s, numFound=89
```

Response shape (fixture: `tests/fixtures/openlibrary/search.json`):

```json
{"numFound": 89, "docs": [
  {"key": "/works/OL5739563W", "title": "Linux Kernel Development",
   "author_name": ["Robert Love"], "first_publish_year": 2004,
   "cover_i": 410921, "subject": ["Linux", "Operating systems"]}
]}
```

Every field the row template needs is there: title, url (`key` + a prefix),
author, date (`first_publish_year`), thumbnail (`cover_i` → a covers URL),
kind (`book`). This is a good first subject for the generic parser because it
needs the three path forms a definition must support — a plain string, an array
element (`author_name[0]`), and a number used inside a template
(`cover_i` → `https://covers.openlibrary.org/b/id/{value}-M.jpg`).

## What this round did *not* do, and will not pretend to

No loader, no generic parser, no new registry entry. The probe and the fixture
are the evidence the next round needs; the 114-site migration above is the work
it has to budget for.

Two smaller things are ready and land with this note: `SearchResult` now carries
`thumbnail/date/author/source/kind` and the row template renders them (J5.2,
`docs/J4-widgets.md` neighbour commit), which is the output side of rich
results; and the SearXNG bridge already fills those fields from a real
instance.


## What shipped from this plan: definitions as data

`static/engines.d/*.json` is loaded at startup by
`engines_load_definitions()` (`src/Scraping/EngineDefs.c`), before
`apply_engines_config`, so a definition can be switched off by id in `[engines]`
like any compiled-in engine. `[engines] definitions_dir` says where to look;
empty means `static/engines.d`, and a missing directory is the normal case.

One shared parser runs four XPaths per definition, so the C side is written
once. Every file is validated and a bad one is logged and skipped, never fatal:

| refusal | why it is refused |
|---|---|
| `search_url` without `%s` | would fetch the same page for every query |
| no `xpath_result` or `xpath_link` | could only ever return nothing |
| duplicate `id` | `[engines]` selection would be ambiguous |
| duplicate `name` | the shared parser looks engines up by display name, so the second would read the first one's selectors |
| malformed JSON | |

Relative hrefs are resolved against `base_url` and selector text is
whitespace-collapsed, because both of those are the norm on real search pages
rather than the exception - Open Library links `/works/OL5739563W` and renders
its author span as `"\n          \nby Daniel P. Bovet\n\n        "`.

`openlibrary` is the worked example:

```
loaded 1 engine definition
url fits: https://openlibrary.org/search?q=linux+kernel
parsed 5
  [0] url=https://openlibrary.org/works/OL5739563W/Linux_Kernel_Development?...
      title=Linux Kernel Development
      snippet=by Robert Love
```

and it appears in a live search with 10 results parsed from a 117 KB page.

**What this does not do:** a JSON API still needs a body parser in C. That half
of J5.1 is the 114-call-site migration described above and is still open.
