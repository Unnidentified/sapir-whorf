/* A1: registry lint. Fails the build (make check) when an engine entry drifts:
   the Brave entry sat at weight 1.0 with supports_site=0 for several revisions
   and nothing noticed. */
#include "../src/Scraping/Scraping.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>

extern SearchEngine *ENGINE_REGISTRY;
extern int ENGINE_COUNT;

static int failures = 0;

static void fail(const char *id, const char *why) {
  printf("  FAIL %-14s %s\n", id ? id : "(null)", why);
  failures++;
}

int main(void) {
  printf("registry lint: %d engines\n", ENGINE_COUNT);

  for (int i = 0; i < ENGINE_COUNT; i++) {
    SearchEngine *e = &ENGINE_REGISTRY[i];

    if (!e->id || !e->id[0]) {
      fail("(unnamed)", "empty id");
      continue;
    }
    if (!e->name || !e->name[0])
      fail(e->id, "empty name");

    /* unique ids */
    for (int j = 0; j < i; j++) {
      if (ENGINE_REGISTRY[j].id && strcmp(ENGINE_REGISTRY[j].id, e->id) == 0)
        fail(e->id, "duplicate id");
    }

    /* must be able to build a request somehow */
    if (!e->base_url && !e->build_url && !e->body_parser)
      fail(e->id, "no base_url, build_url or body_parser");

    if (e->verticals == 0)
      fail(e->id, "no verticals");

    if (e->weight <= 0.0 || e->weight > 3.0)
      fail(e->id, "weight out of range");

    if (e->timeout_secs < 0 || e->timeout_secs > 30)
      fail(e->id, "timeout out of range");

    if (!e->parser && !e->body_parser)
      fail(e->id, "no parser");

    /* web engines must declare whether they honour site: - this is what the
       site: routing depends on */
    if ((e->verticals & 1u) && e->supports_site != 0 && e->supports_site != 1)
      fail(e->id, "supports_site must be explicitly 0 or 1");

    /* every engine must declare which index answers it: consensus and the
       early exit both count groups, so a missing group silently becomes its
       own opinion (F2) */
    if (!e->index_group || !e->index_group[0])
      fail(e->id, "no index_group");

    /* a "trusted" web engine for the batch deadline is weight >= 1.1; make the
       intent explicit so a silent weight edit cannot change batch behaviour */
    if ((e->verticals & 1u) && e->weight >= 1.1 && !e->supports_site)
      fail(e->id, "trusted-weight web engine without site: support");

    printf("  ok   %-14s w=%.2f vert=0x%02x%s\n", e->id, e->weight,
           e->verticals, e->enabled ? "" : " (disabled)");
  }

  /* every theme must define every palette slot (kept from the shell script) */
  printf("%s\n", failures == 0 ? "registry lint: PASS" : "registry lint: FAIL");
  return failures == 0 ? 0 : 1;
}
