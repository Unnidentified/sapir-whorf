# Engine definitions

Each `.json` file here becomes an engine at startup, so adding one is a file and
a restart rather than a parser and a compile. Loaded by
`engines_load_definitions()` in `src/Scraping/EngineDefs.c`; every file is
validated and a bad one is logged and skipped, never fatal.

A definition supplies its URL template and four XPaths; one shared parser runs
them, so a definition that cannot name a result and a link is rejected at load
time rather than returning nothing in production.

| key | required | meaning |
|---|---|---|
| `id` | yes | unique id, used by `[engines]` to enable/disable |
| `name` | yes | unique display name; the shared parser looks the engine up by it |
| `search_url` | yes | must contain one `%s`, the encoded query |
| `xpath_result` | yes | selects each result block |
| `xpath_link` | yes | within a block; an element with `href`, or `@href` |
| `xpath_title` | no | within a block |
| `xpath_snippet` | no | within a block |
| `base_url`, `index_group`, `weight`, `pace_per_minute` | no | |
| `verticals` | no | `web` (default), `news`, `media`, `code`, `academic`, `entity` |
| `opt_in` | no | tier 3; still gated by `[engines] allow_files` and the visitor's switch |

An engine defined this way is a **HTML** engine: it fetches a page and runs
XPaths. For a JSON API, the body parser still has to exist in C - that is the
remaining half of the review's J5.1 and it is described in
`docs/J5-engines.md`.
