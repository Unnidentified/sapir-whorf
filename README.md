# sapir-whorf

A metasearch engine written in C. One binary, no JavaScript required, fourteen sources queried in parallel and merged into a single ranked page.

It fetches ordinary result pages over libcurl and parses them with libxml2, so the core results do not depend on any search API. Sixteen themes, fourteen interface languages, and a settings page that never asks you to edit a file by hand.

---

### Table of Contents

- [Quick Start](#quick-start)
- [What you actually get](#what-you-actually-get)
- [Sources](#sources)
- [Cards and answers](#cards-and-answers)
- [Appearance](#appearance)
- [Servers and accounts](#servers-and-accounts)
- [Building from source](#building-from-source)
- [Docker](#docker)
- [Testing](#testing)
- [Under the hood](#under-the-hood)
- [Upstream, licence and the author's request](#upstream-licence-and-the-authors-request)
- [Contributing](#contributing)

---

## Quick Start

Build it and run it:

```bash
make
./bin/omnisearch --config example-config.ini
```

Open **http://127.0.0.1:8087**.

It needs libcurl, libxml2, OpenSSL and [beaker](https://git.bwaaa.space/beaker/). The per-distro install lines are [below](#building-from-source).

To run it as a service instead of in a terminal:

```bash
sudo make install-systemd     # or openrc, runit, s6, dinit; launchd on macOS
```

`example-config.ini` is a working configuration. Copy it to `config.ini` and edit the port, the probe settings or the proxy if you need to.

---

## What you actually get

Every search fans out to each enabled source at once, on its own thread, and the page is assembled from whichever answers arrive before the deadline. A source that is slow does not hold the page: it is cancelled and the page says so, in one line, rather than pretending it was silent.

Results are scored on more than position in the source's list. Domain quality, agreement between sources, title relevance and result freshness all move the ranking, and the same query returns the same order twice.

At `127.0.0.1` it needs no password to save settings: a write must come from this machine, from this origin, with a nonce the page issued. A request from anywhere else needs the admin token from `~/.omnisearch/secrets.ini`, which is written mode `0600` and never displayed on the page.

---

## Sources

Fourteen built in. Each one can be switched off, and its weight can be tuned, from **Settings > Search engines**.

| Source | Answers |
|---|---|
| **DuckDuckGo**, **Bing**, **Yahoo**, **Brave** | general web results |
| **Wikipedia** | the article itself, plus the quick-summary card |
| **OpenAlex**, **Crossref** | academic papers, with citation counts |
| **arXiv** | preprints |
| **Stack Overflow** | programming answers |
| **Hacker News** | community discussion, via Algolia |
| **Reddit** | discussion threads, two ways: the API, or a `site:reddit.com` web lane that needs no account |
| **SearXNG** | a self-hosted instance you point at; the documented low-risk route to Google's index |
| **YaCy** | a peer you run yourself |
| **Google** and **Google Programmable Search Element** | opt-in, and each needs its own key |

Google is deliberately not a default. The two Google rows are switches you turn on yourself, because both involve an API key and a quota that belongs to you.

---

## Cards and answers

A query that is really a question is answered above the results, not instead of them.

| Card | Triggered by |
|---|---|
| **Calculator** | `18% of 240`, `sqrt(2)`, or the keypad |
| **Unit conversion** | `5 km in miles` |
| **Currency** | `40 usd in eur` |
| **Colour codes** | `#c99a75` |
| **Translate** | `hola amigo in english`, two panes with a swap |
| **Dictionary** | `define ephemeral` |
| **Share prices** | `nvda stock` |
| **Places** | `coffee near me` |
| **Reddit** | `r/rust`, or a question phrased like one |

Each card works with JavaScript disabled; the scripts only make them update as you type. Translations and dictionary entries name their provider and say that the text is not checked by this site.

---

## Appearance

Sixteen themes ship: `amber`, `apprentice`, `catppuccin-frappe`, `catppuccin-latte`, `catppuccin-macchiato`, `catppuccin-mocha`, `dark`, `gruvbox-dark`, `iceberg-dark`, `jellybeans`, `light`, `miasma`, `noctis`, `srcery`, `system`, and `custom`.

`custom` is nine colour pickers — background, card, border, primary/secondary/muted/dim text, accent — which apply live and can be returned to any built-in theme at any time.

Interface languages: `en_gb`, `en_us`, `de_de`, `es_es`, `fr_fr`, `it_it`, `nl_nl`, `pt_br`, `ru_ru`, `fi_fi`, `lv_lv`, `vi_vn`, `af_za`, `bn_bd`. Interface scale runs 80–110% and the wordmark has three fonts.

The layout has one definition and two breakpoints. Everything above 1240 px gets the information rail beside the results; below it the rail becomes a card above them rather than dropping underneath the pager.

---

## Servers and accounts

**Settings > Accounts and servers** configures the optional providers. Every value is stored in `~/.omnisearch/secrets.ini` at mode `0600`, never in `config.ini`, and never in the archive `make dist` produces.

**Reddit** is the one that needs three fields and no secret:

1. Register an app at `reddit.com/prefs/apps` as an **installed app**.
2. Paste the **Client ID**, the **redirect URI** and your **User-Agent**.
3. Press **Authorise**, then **Allow**.

Register the redirect URI as `http://<your-host>/settings/reddit-callback`. Reddit then sends the code straight back to this server, which finishes the exchange and reports the account it connected as. A custom scheme such as `myapp://callback` cannot be delivered to a web server, so the card says so rather than letting you press Allow and wonder.

The Reddit **User-Agent must be your own**. A string naming another client is refused at the field, because using someone else's identity is what gets an app banned.

**SearXNG**, **YaCy** and **Google** are covered by `docker-compose.yml` and `searxng/settings.yml`; SearXNG needs `json` in its `search.formats` and its limiter off, both of which that file already sets.

---

## Building from source

Install the libraries for your system:

| System | Command |
|---|---|
| Arch | `pacman -S libxml2 curl` |
| Debian/Ubuntu | `apt install build-essential libssl-dev libxml2-dev libcurl4-openssl-dev` |
| Fedora | `dnf install libxml2-devel libcurl-devel` |
| openSUSE | `zypper install libxml2-devel libcurl-devel` |
| Alpine | `apk add libxml2-dev curl-dev shadow` |
| Void | `xbps-install -S libxml2-devel libcurl-devel` |
| macOS (Homebrew) | `brew install libxml2 curl openssl pkg-config` |
| macOS (MacPorts) | `port install libxml2 curl openssl3 pkgconfig` |

On Alpine, `shadow` is needed for the user creation step during install. On macOS the build uses `pkg-config` to find libxml2, libcurl and OpenSSL.

Then **beaker**:

```bash
git clone https://git.bwaaa.space/beaker
cd beaker
make
sudo make install
```

Finally:

```bash
git clone https://codeberg.org/gabrielf/omnisearch
cd omnisearch
make
sudo make install-systemd
```

Build output is `bin/omnisearch`. `make info` prints what the build detects.

---

## Docker

```bash
docker compose up -d --build
```

The service is published on **`127.0.0.1:8087`** only — the port binding is explicit about the loopback address, so a compose file cannot quietly expose an instance that has no password on its settings page.

`docker compose up searxng` brings up the bundled SearXNG alongside it.

---

## Testing

```bash
make verify
```

Thirty-four check groups: the engine registry and its ordering, the request pipeline, the parser fixtures for every source, ranking determinism, the settings forms, cookie handling, the OAuth callback, the answer cards, the themes and their contrast, and the archive guard.

`make check-themes` alone validates all sixteen themes: token names, no literal colours, one button family, one layout definition, and WCAG contrast for every text/background pair.

There are also `make asan`, `make eval-offline` and `make record-replay` for the deeper passes.

---

## Under the hood

```
src/
  Routes/       handlers: search, images, videos, settings, accounts
  Scraping/     one module per source, plus the engine registry and breaker
  Ranking/      scoring and canonical URLs
  Query/        the query parser, categories, placeholders
  Infobox/      the answer cards
  Providers/    the provider table behind Settings
  Utility/      config, secrets, themes, locale, HTTP
templates/      one file per page and per card
static/         the stylesheet, 16 themes, and the scripts
locales/        14 interface translation files
```

The request path is: parse the query into terms, phrases and a category; pick the engines that serve that category; fan out on threads with a per-source deadline; parse each response into a common result; merge, de-duplicate by canonical URL, score, and paginate.

A source that fails repeatedly is suspended by a breaker rather than retried on every search, and a suspended source is named on the page with when it will be tried again. Nothing is silently dropped.

`docs/SETUP.md` covers the pipeline, the ranking signals, the cache and the breaker in more detail, including the parts that are known to be sharp.

---

## Upstream, licence and the author's request

This is **GPL-2.0** software; the full text is in [LICENSE](LICENSE).

The original project is **omnisearch**, by **frosty**, hosted at [codeberg.org/gabrielf/omnisearch](https://codeberg.org/gabrielf/omnisearch). This repository is a derivative of that work and of the contributions of the people named in its history. Their authorship is preserved in the commit metadata and is not mine to remove.

The upstream author asks the following, and it is reproduced here rather than dropped:

> **Disclaimer**
> I request that none of this code, in part or in full, be hosted on GitHub, SourceForge, or any other proprietary platform. This request is made out of respect for both me, the developer and for you, the user.

That's not a condition of the licence: GPL-2.0 permits redistribution on any platform. It is recorded here because the person who wrote it deserves to be quoted accurately rather than silently overruled. Anyone redistributing this code should read it and make their own decision knowingly.

---

## Contributing

Upstream takes patches by email: generate one with `git format-patch HEAD~1` and send it to [frostinator33@airmail.cc](mailto:frostinator33@airmail.cc), beginning the subject with `[PATCH omnisearch]`. Send revised versions as `[PATCH omnisearch v2]`, `v3`, and so on.
