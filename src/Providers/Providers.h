/* K1.3: one table, one row per provider.
 *
 * Before this, /settings could only list engines the *server* had enabled, so a
 * provider that was off by config, opt-in or simply absent from the build was
 * indistinguishable from one the owner had turned off, and the two answer
 * providers that are widgets (translate, stocks) were not listed at all.
 *
 * The table is built once at startup from the registry plus a fixed list of
 * non-engine providers, so a provider added later appears on the page without
 * anyone remembering to add a row. */
#ifndef OMNISEARCH_PROVIDERS_H
#define OMNISEARCH_PROVIDERS_H

typedef enum {
  PG_WEB = 0,
  PG_ACADEMIC,
  PG_ANSWERS,
  PG_VIDEOS,
  PG_BRIDGES,
  PG_OPTIN,
  PG_GROUP_COUNT
} ProviderGroup;

typedef enum {
  PS_ON = 0,      /* on, and reachable */
  PS_OFF,         /* off by this browser */
  PS_UNAVAILABLE  /* the server or the build says no */
} ProviderState;

typedef struct {
  const char *id;      /* engine id, or the provider id for a widget */
  const char *label;   /* what the page calls it */
  const char *host;    /* destination host, NULL when it runs locally */
  ProviderGroup group;
  int opt_in;          /* needs the owner's opt-in */
  int toggleable;      /* 0 for local-only: no network, nothing to switch off */
} ProviderRow;

/* Fills rows (capacity of at least providers_capacity()) and returns the count.
   Ordered by group, so the page reads top to bottom. */
int providers_build(ProviderRow *rows, int capacity);

/* Is this a provider id we know? The disabled set holds engine ids and answer
   provider ids in one list, so a save cannot validate against the registry. */
int provider_id_known(const char *id);

/* Does this provider need the network? Used to decide whether a row gets a
   switch at all: there is nothing to switch off in a local computation. */
int provider_uses_network(const char *id);

/* L1: the row list by index, and the row by id - so a toggle can rebuild the
   whole disabled set without a second list of provider names in the save
   handler. Both return NULL past the end / for an unknown id. */
const char *provider_id_at(int index);
const ProviderRow *provider_by_id(const char *id);

/* K1.4: the widget resolver's gate - "is the provider behind this card
   switched off?". One copy, installed by the search page and by the fragment
   endpoint, because /w/<kind> used to reach the providers without any gate. */
int provider_widget_denied(const char *kind);

/* K5.3: a lane provider has a destination but no host of its own - it is
   reached through another provider's connection (the Reddit lane asks the web
   engines for site:reddit.com). A test that checks "on means a request to its
   host" would be checking the wrong thing, so the distinction is printed. */
int provider_is_lane(const char *id);
int providers_capacity(void);

const char *provider_group_label(ProviderGroup g);

/* The provider id that backs an engine id, or the id itself. Wikipedia is both
   an engine and the infobox panel: one destination, one switch. */
const char *provider_id_for_engine(const char *engine_id);

/* The provider id behind a widget kind ("place" -> "places"), or NULL. */
const char *provider_id_for_widget_kind(const char *kind);

/* K1.4: does this browser's cookie turn the provider off? Engine ids and
   provider ids live in the same list because they name the same destination.
   Reads the request cookie, so it must be called from a request. */
int provider_is_off(const char *id);

/* State for the page: off-by-you wins over server availability, because that is
   the one the owner can change here. */
ProviderState provider_state(const ProviderRow *row);

/* Unavailable because it needs configuration (SearXNG without searxng_url),
   as opposed to switched off by the administrator. The page says which. */
int provider_needs_config(const ProviderRow *row);

/* L4: why a row is unavailable, as a reason a reader can act on. One enum,
 * one sentence per case, and each case names its own fix. "Disabled by the
 * server administrator" was true of nothing the owner could reach: the owner IS
 * the administrator, and there was no statement of what to do. */
typedef enum {
  PR_AVAILABLE = 0,  /* on and reachable */
  PR_NEEDS_FIELD,    /* a card in /settings is missing a value */
  PR_UNREACHABLE,    /* the peer or instance does not answer */
  PR_CLOSED_UPSTREAM,/* the provider stopped accepting new users */
  PR_OFF_IN_CONFIG,  /* engines = ... in config.ini leaves it out */
  PR_NOT_IN_BUILD,   /* this build has no such provider */
  PR_LOCAL           /* runs here, nothing to reach */
} ProviderReason;

/* The reason, and the field that would fix it (for PR_NEEDS_FIELD). */
ProviderReason provider_reason(const ProviderRow *row);
const char *provider_reason_field(const ProviderRow *row);

/* The words next to the state: "On", "Off (by you)", or the reason in a
   sentence with its fix. Never colour alone. */
const char *provider_state_note(const ProviderRow *row);

/* K1.5: one short disclosure per opt-in provider - what leaves the machine,
   where it goes, and what the downside is. The global paragraph is the section
   intro; this is the per-provider sentence. */
const char *provider_opt_in_disclosure(const char *id);

#endif
