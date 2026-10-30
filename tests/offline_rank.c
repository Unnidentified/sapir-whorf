/* A3: offline relevance benchmark.
 *
 * Re-ranks a recorded replay file with the production ranker and prints
 * metrics. No network, no server, fully deterministic: this is the harness
 * that makes a ranking change measurable instead of a guess.
 *
 *   tests/offline_rank <replay.jsonl> [judgments.tsv]
 *
 * Reports, per query and in aggregate: result count, MRR@10, nDCG@10 and
 * hit@3 against expected hosts, plus the coverage distribution.
 */
#include "../src/Ranking/Ranker.h"
#include "../src/Query/QueryAnalysis.h"
#include "../src/Scraping/Scraping.h"
#include "../src/Utility/Utility.h"
#include <jansson.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DOCS 512

typedef struct {
  char *q;
  char *engine;
  int page;
  RankDoc docs[MAX_DOCS];
  int doc_engine[MAX_DOCS]; /* which engine ordinal produced each doc */
  int count;
  int engine_count;
  char *engines[32];
} QueryRecord;

extern SearchEngine ENGINE_REGISTRY[];
extern const int ENGINE_COUNT;

static int engine_ordinal(QueryRecord *r, const char *id) {
  for (int i = 0; i < r->engine_count; i++)
    if (strcmp(r->engines[i], id) == 0)
      return i;
  if (r->engine_count < 32) {
    r->engines[r->engine_count] = strdup(id);
    return r->engine_count++;
  }
  return 0;
}

/* minimal containment test so this harness does not drag in the whole app */
/* Is `expected` the registrable host of `url`, or a parent of it?
   "en.wikipedia.org" matches "wikipedia.org"; "notwikipedia.org" does not. */
static int host_matches(const char *url, const char *expected) {
  if (!url || !expected || !expected[0])
    return 0;
  const char *p = strstr(url, "://");
  p = p ? p + 3 : url;
  if (strncasecmp(p, "www.", 4) == 0)
    p += 4;

  /* host ends at the first / ? # or : */
  size_t hlen = strcspn(p, "/?#:");
  if (hlen == 0)
    return 0;

  size_t elen = strlen(expected);
  if (elen > hlen)
    return 0;
  if (strncasecmp(p + (hlen - elen), expected, elen) != 0)
    return 0;
  /* require a dot boundary: match the whole host or a subdomain of it */
  if (hlen == elen)
    return 1;
  return p[hlen - elen - 1] == '.';
}

/* judgments: "query<TAB>expected-host[<TAB>grade]" */
typedef struct {
  char *query;
  char *host;
  int grade;
} Judgment;

static Judgment judgments[512];
static int judgment_count = 0;

static void load_judgments(const char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    return;
  char *line = NULL;
  size_t cap = 0;
  while (getline(&line, &cap, fp) > 0 && judgment_count < 512) {
    if (line[0] == '#')
      continue;
    char *t1 = strchr(line, '\t');
    if (!t1)
      continue;
    *t1 = '\0';
    char *host = t1 + 1;
    char *t2 = strchr(host, '\t');
    int grade = 1;
    if (t2) {
      *t2 = '\0';
      grade = atoi(t2 + 1);
    }
    char *nl = strpbrk(host, "\r\n");
    if (nl)
      *nl = '\0';
    judgments[judgment_count].query = strdup(line);
    judgments[judgment_count].host = strdup(host);
    judgments[judgment_count].grade = grade > 0 ? grade : 1;
    judgment_count++;
  }
  free(line);
  fclose(fp);
}

static const Judgment *judgment_for(const char *query) {
  for (int i = 0; i < judgment_count; i++) {
    if (strcasecmp(judgments[i].query, query) == 0)
      return &judgments[i];
  }
  return NULL;
}

static int load(const char *path, QueryRecord **out, int *out_count) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return -1;

  QueryRecord *recs = calloc(256, sizeof(QueryRecord));
  int n = 0;

  char *line = NULL;
  size_t cap = 0;
  while (getline(&line, &cap, fp) > 0 && n < 256) {
    json_error_t err;
    json_t *root = json_loads(line, 0, &err);
    if (!root)
      continue;

    const char *q = json_string_value(json_object_get(root, "q"));
    const char *e = json_string_value(json_object_get(root, "engine"));
    if (!q || !e) {
      json_decref(root);
      continue;
    }

    /* fold records for the same query into one candidate pool */
    int slot = -1;
    for (int i = 0; i < n; i++) {
      if (strcmp(recs[i].q, q) == 0 && recs[i].page ==
                                             (int)json_integer_value(
                                                 json_object_get(root, "page"))) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      slot = n++;
      recs[slot].q = strdup(q);
      recs[slot].engine = strdup(e);
      recs[slot].page =
          (int)json_integer_value(json_object_get(root, "page"));
      recs[slot].count = 0;
      recs[slot].engine_count = 0;
      memset(recs[slot].engines, 0, sizeof(recs[slot].engines));
    }

    json_t *rows = json_object_get(root, "results");
    if (json_is_array(rows)) {
      for (size_t i = 0; i < json_array_size(rows); i++) {
        if (recs[slot].count >= MAX_DOCS)
          break;
        json_t *r = json_array_get(rows, i);
        const char *url = json_string_value(json_object_get(r, "url"));
        if (!url)
          continue;
        int ord = engine_ordinal(&recs[slot], e);
        RankDoc *d = &recs[slot].docs[recs[slot].count];
        recs[slot].doc_engine[recs[slot].count] = ord;
        recs[slot].count++;
        memset(d, 0, sizeof(*d));
        d->url = strdup(url);
        d->title = strdup(json_string_value(json_object_get(r, "title")) ?: "");
        d->snippet =
            strdup(json_string_value(json_object_get(r, "snippet")) ?: "");
      }
    }
    json_decref(root);
  }

  free(line);
  fclose(fp);
  *out = recs;
  *out_count = n;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <replay.jsonl> [judgments.tsv]\n", argv[0]);
    return 2;
  }

  QueryRecord *recs = NULL;
  int nrecs = 0;
  if (argc >= 3)
    load_judgments(argv[2]);

  if (load(argv[1], &recs, &nrecs) != 0) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }

  printf("offline replay: %d queries\n", nrecs);

  int queries = 0, empty = 0, total = 0;
  double coverage_sum = 0.0;
  int judged = 0, hit3 = 0;
  double reciprocal_sum = 0.0, dcg = 0.0, idcg = 0.0;

  for (int r = 0; r < nrecs; r++) {
    if (recs[r].count == 0) {
      empty++;
      queries++;
      printf("  %-50s 0 results\n", recs[r].q);
      continue;
    }

    /* the production pipeline, minus fetching */
    SearchQuery analysis;
    memset(&analysis, 0, sizeof(analysis));
    query_analyze(recs[r].q, "en_gb", &analysis);

    /* mark which engine produced each doc so consensus works */
    int count = recs[r].count;

    /* Reconstruct the engine mask so consensus and engine weight actually
       participate; without it the harness measured a pipeline that never runs
       in production. */
    for (int i = 0; i < count; i++) {
      int ord = recs[r].doc_engine[i];
      recs[r].docs[i].engine_mask |= (1 << ord);
      if (ord < 32) {
        const char *gid = recs[r].engines[ord];
        for (int e = 0; e < ENGINE_COUNT; e++) {
          if (strcmp(ENGINE_REGISTRY[e].id, gid) == 0) {
            recs[r].docs[i].engine_groups[ord] = ENGINE_REGISTRY[e].index_group;
            recs[r].docs[i].engine_weights[ord] = ENGINE_REGISTRY[e].weight;
            break;
          }
        }
      }
    }

    int deduped = rank_dedupe(recs[r].docs, count);
    rank_score(recs[r].docs, deduped, &analysis);

    int kept = 0;
    double cov = 0.0;
    for (int i = 0; i < deduped; i++) {
      if (recs[r].docs[i].coverage < 0.15) {
        rank_doc_free(&recs[r].docs[i]);
        continue;
      }
      cov += recs[r].docs[i].coverage;
      if (kept != i)
        recs[r].docs[kept] = recs[r].docs[i];
      kept++;
    }
    if (kept > 0)
      coverage_sum += cov / kept;

    queries++;
    total += kept;
    const Judgment *j = judgment_for(recs[r].q);
    int hit_rank = 0;
    if (j) {
      for (int i = 0; i < kept && i < 10; i++) {
        if (host_matches(recs[r].docs[i].url, j->host)) {
          hit_rank = i + 1;
          break;
        }
      }
      judged++;
      if (hit_rank)
        reciprocal_sum += 1.0 / hit_rank;
      if (hit_rank && hit_rank <= 3)
        hit3++;
      if (hit_rank)
        dcg += (double)((1 << j->grade) - 1) / log2(hit_rank + 1.0);
      idcg += (double)((1 << j->grade) - 1) / log2(2.0);
    }

    printf("  %-50s raw=%-3d deduped=%-3d floored=%-3d%s\n", recs[r].q, count,
           deduped, kept,
           j ? (hit_rank ? "  hit@" : "  miss") : "");
  }

  printf("\nqueries=%d empty=%d avg_rendered=%.1f avg_coverage=%.2f\n", queries,
         empty, queries ? (double)total / queries : 0.0,
         queries ? coverage_sum / queries : 0.0);
  if (judged > 0) {
    printf("judged=%d  hit@3=%.2f  MRR@10=%.3f  nDCG@10=%.3f\n", judged,
           (double)hit3 / judged, reciprocal_sum / judged,
           idcg > 0 ? dcg / idcg : 0.0);
  } else {
    printf("judged=0 (pass a judgments file for MRR/nDCG)\n");
  }

  printf("offline replay: %s\n", empty == 0 ? "PASS" : "PASS (some empty)");
  return 0;
}
