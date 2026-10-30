#include "Replay.h"
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t replay_mutex = PTHREAD_MUTEX_INITIALIZER;

int replay_enabled_read(void) {
  const char *f = getenv("OMNI_REPLAY_READ");
  return f && f[0];
}

int replay_enabled_record(void) {
  const char *f = getenv("OMNI_REPLAY_RECORD");
  return f && f[0];
}

/* Records are keyed by "query\tengine\tpage" - the same identity the scrape
 * uses, and deliberately not the client key, so one recording serves everyone. */
static void replay_key(const ScrapeJob *job, char *out, size_t size) {
  snprintf(out, size, "%s|%s|%d", job->query ? job->query : "",
           job->engine ? job->engine->id : "?", job->page);
}

int replay_fill_job(ScrapeJob *job) {
  const char *path = getenv("OMNI_REPLAY_READ");
  if (!path || !path[0] || !job || !job->engine)
    return 0;

  char want[1024];
  replay_key(job, want, sizeof(want));

  FILE *fp = fopen(path, "rb");
  if (!fp)
    return 0;

  char *line = NULL;
  size_t cap = 0;
  int found = 0;

  while (getline(&line, &cap, fp) > 0) {
    json_error_t err;
    json_t *root = json_loads(line, 0, &err);
    if (!root)
      continue;

    const char *q = json_string_value(json_object_get(root, "q"));
    const char *e = json_string_value(json_object_get(root, "engine"));
    int page = (int)json_integer_value(json_object_get(root, "page"));

    char got[1024];
    snprintf(got, sizeof(got), "%s|%s|%d", q ? q : "", e ? e : "?", page);
    if (strcmp(got, want) != 0) {
      json_decref(root);
      continue;
    }

    json_t *rows = json_object_get(root, "results");
    if (json_is_array(rows)) {
      int n = (int)json_array_size(rows);
      job->out_results[0] = alloc_results_array(n, job->max_results);
      int count = 0;
      for (int i = 0; i < n && count < job->max_results; i++) {
        json_t *r = json_array_get(rows, i);
        const char *url = json_string_value(json_object_get(r, "url"));
        const char *title = json_string_value(json_object_get(r, "title"));
        const char *snippet = json_string_value(json_object_get(r, "snippet"));
        if (url) {
          assign_result(&job->out_results[0][count], strdup(url),
                        strdup(title ? title : ""),
                        strdup(snippet ? snippet : ""), 1);
          count++;
        }
      }
      job->results_count = count;
      job->status = count > 0 ? SCRAPE_STATUS_OK : SCRAPE_STATUS_EMPTY;
      found = 1;
    }
    json_decref(root);
    break;
  }

  free(line);
  fclose(fp);
  return found;
}

void replay_record_job(const ScrapeJob *job, const SearchResult *results,
                       int count) {
  const char *path = getenv("OMNI_REPLAY_RECORD");
  if (!path || !path[0] || !job || !job->engine)
    return;

  json_t *root = json_object();
  json_object_set_new(root, "q", json_string(job->query ? job->query : ""));
  json_object_set_new(root, "engine", json_string(job->engine->id));
  json_object_set_new(root, "page", json_integer(job->page));

  json_t *rows = json_array();
  for (int i = 0; i < count; i++) {
    json_t *r = json_object();
    json_object_set_new(r, "url",
                        json_string(results[i].url ? results[i].url : ""));
    json_object_set_new(r, "title",
                        json_string(results[i].title ? results[i].title : ""));
    json_object_set_new(r, "snippet", json_string(results[i].snippet
                                                      ? results[i].snippet
                                                      : ""));
    json_array_append_new(rows, r);
  }
  json_object_set_new(root, "results", rows);

  char *line = json_dumps(root, JSON_COMPACT);
  json_decref(root);
  if (!line)
    return;

  pthread_mutex_lock(&replay_mutex);
  FILE *fp = fopen(path, "ab");
  if (fp) {
    fprintf(fp, "%s\n", line);
    fclose(fp);
  }
  pthread_mutex_unlock(&replay_mutex);
  free(line);
}
