/* L3: one place that answers "what value is in force for this setting, and
 * who set it".
 *
 * Three sources, highest wins: the environment, then secrets.ini (which the
 * page writes), then config.ini (which the owner edits and which travels in
 * the archive). Every reader goes through here, so the page and the code can
 * never disagree about which answer is live.
 */
#ifndef EFFECTIVE_H
#define EFFECTIVE_H

#include <stddef.h>

typedef enum {
  EFFECTIVE_FROM_ENV = 0,
  EFFECTIVE_FROM_FILE,
  EFFECTIVE_FROM_CONFIG,
  EFFECTIVE_FROM_DEFAULT
} EffectiveSource;

/* A server setting the page can edit. The value is copied into out; the return
   value says where it came from. An empty result means "not set anywhere". */
EffectiveSource effective_get(const char *key, char *out, size_t cap);

/* The label the page shows for a source, in words, so "why is this field
   greyed out" has an answer on the page. */
const char *effective_source_label(EffectiveSource src);

/* Copy the effective value of every editable setting into the config object.
   Called once at startup and again after a save, so a saved URL takes effect
   without a restart. Values read per request (the Reddit credentials) are
   looked up through effective_get() and need no folding at all. */
struct Config_;
void apply_effective_settings(void *config);

#endif
