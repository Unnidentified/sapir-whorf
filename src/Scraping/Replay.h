#ifndef REPLAY_H
#define REPLAY_H

#include "Scraping.h"

/* Offline replay, for the ranking benchmark (A3).
 *
 * OMNI_REPLAY_RECORD=<file>  append one JSON line per engine per query with
 *                            the parsed results, so a run can be re-ranked
 *                            later without touching the network.
 * OMNI_REPLAY_READ=<file>    serve results from such a file instead of fetching
 *                            anything at all.
 *
 * Both are env-only and off by default: a normal install never touches this.
 */

int replay_enabled_read(void);
int replay_enabled_record(void);

/* Returns 1 and fills the job when the read file had an entry for this
 * query+engine; 0 means "no entry, fetch it normally". */
int replay_fill_job(ScrapeJob *job);

/* Append the parsed results of a finished job. No query text beyond the query
 * itself is stored; this file is a local benchmark artefact. */
void replay_record_job(const ScrapeJob *job, const SearchResult *results,
                       int count);

#endif
