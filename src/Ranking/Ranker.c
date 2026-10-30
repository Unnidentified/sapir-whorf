#include "Ranker.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants (see the implementation plan) ---------- */

#define W_RRF 0.50
#define W_BM25 0.18
#define W_AUTHORITY 0.12
#define W_PHRASE 0.06
#define W_CONSENSUS 0.05
#define W_URL_QUALITY 0.04
#define W_FRESHNESS 0.03
#define W_SPAM 0.15

#define BM25_K1 1.2
#define BM25_B 0.75
#define MAX_TOKENS 64

/* ---------- registrable domain ---------- */

static const char *TWO_LEVEL_SUFFIXES[] = {
    "co.uk", "ac.uk", "com.au", "co.jp", "com.br", "co.in", "com.mx",
    "co.nz", "co.za",  "com.tr", "com.cn", "com.tw", "co.kr", "com.sg"};

void rank_domain_of(const char *url, char *out, size_t size) {
  out[0] = '\0';
  if (!url || !size)
    return;

  const char *start = strstr(url, "://");
  start = start ? start + 3 : url;
  if (strncasecmp(start, "www.", 4) == 0)
    start += 4;

  size_t len = 0;
  while (start[len] && start[len] != '/' && start[len] != '?' &&
         start[len] != ':' && start[len] != '#' && len + 1 < size) {
    out[len] = (char)tolower((unsigned char)start[len]);
    len++;
  }
  out[len] = '\0';

  /* strip common mobile / amp prefixes */
  const char *prefixes[] = {"m.", "amp.", "mobile."};
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    size_t plen = strlen(prefixes[i]);
    if (strncmp(out, prefixes[i], plen) == 0 && strlen(out) > plen + 3)
      memmove(out, out + plen, strlen(out + plen) + 1);
  }

  /* find the label boundary, then the public suffix boundary */
  char *first_dot = strchr(out, '.');
  if (!first_dot)
    return;

  const char *last_two = NULL;
  char *scan = first_dot;
  while ((scan = strchr(scan, '.')) != NULL) {
    scan++;
    if (strchr(scan, '.'))
      last_two = scan;
  }
  if (last_two) {
    memmove(out, last_two, strlen(last_two) + 1);
  }

  for (size_t i = 0; i < sizeof(TWO_LEVEL_SUFFIXES) / sizeof(TWO_LEVEL_SUFFIXES[0]);
       i++) {
    size_t slen = strlen(TWO_LEVEL_SUFFIXES[i]);
    size_t olen = strlen(out);
    if (olen > slen && out[olen - slen - 1] == '.' &&
        strcmp(out + olen - slen, TWO_LEVEL_SUFFIXES[i]) == 0) {
      /* keep one extra label for e.g. example.co.uk */
      char *prev = out + olen - slen - 1;
      while (prev > out && prev[-1] != '.')
        prev--;
      if (prev > out)
        memmove(out, prev, strlen(prev) + 1);
      break;
    }
  }
}

static int url_has_tracking_param(const char *url) {
  static const char *TRACKERS[] = {"utm_", "gclid=", "fbclid=", "ref_src=",
                                   "mc_cid=", "igshid=", "spm="};
  for (size_t i = 0; i < sizeof(TRACKERS) / sizeof(TRACKERS[0]); i++) {
    if (strstr(url, TRACKERS[i]))
      return 1;
  }
  return 0;
}

void rank_canonical_url(const char *url, char *out, size_t size) {
  out[0] = '\0';
  if (!url || !size)
    return;

  const char *p = url;
  int https = 0;
  if (strncasecmp(p, "https://", 8) == 0) {
    https = 1;
    p += 8;
  } else if (strncasecmp(p, "http://", 7) == 0) {
    p += 7;
  }
  if (strncasecmp(p, "www.", 4) == 0)
    p += 4;
  if (strncasecmp(p, "m.", 2) == 0 || strncasecmp(p, "amp.", 4) == 0)
    p += (p[0] == 'm' ? 2 : 4);

  char buffer[1024];
  size_t o = 0;
  const char *query_start = NULL;
  for (; *p && o + 1 < sizeof(buffer); p++) {
    if (*p == '#')
      break;
    if (*p == '?') {
      query_start = p + 1;
      break;
    }
    buffer[o++] = (char)tolower((unsigned char)*p);
  }
  buffer[o] = '\0';

  /* drop /index.html and trailing slashes */
  size_t len = strlen(buffer);
  const char *INDEX = "/index.html";
  size_t index_len = strlen(INDEX);
  if (len > index_len &&
      strcmp(buffer + len - index_len, INDEX) == 0) {
    buffer[len - index_len] = '\0';
    len -= index_len;
  }
  while (len > 0 && buffer[len - 1] == '/')
    buffer[--len] = '\0';

  /* keep non-tracking query parameters, sorted, so order cannot fake a dupe */
  char params[512] = "";
  if (query_start) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", query_start);
    char *saveptr = NULL;
    char *token = strtok_r(copy, "&", &saveptr);
    char *parts[16];
    int n = 0;
    while (token && n < 16) {
      size_t tl = strlen(token);
      while (tl > 0 && (token[tl - 1] == ' ' || token[tl - 1] == '\t'))
        token[--tl] = '\0';
      if (tl > 0 && !url_has_tracking_param(token))
        parts[n++] = token;
      token = strtok_r(NULL, "&", &saveptr);
    }
    for (int i = 1; i < n; i++) {
      for (int j = i; j > 0 && strcmp(parts[j - 1], parts[j]) > 0; j--) {
        char *tmp = parts[j - 1];
        parts[j - 1] = parts[j];
        parts[j] = tmp;
      }
    }
    size_t used = 0;
    for (int i = 0; i < n && used + 2 < sizeof(params); i++) {
      used += (size_t)snprintf(params + used, sizeof(params) - used, "%s%s",
                               i ? "&" : "?", parts[i]);
    }
  }

  (void)https;
  snprintf(out, size, "%s%s", buffer, params);
}

/* ---------- authority + spam tables ---------- */

typedef struct {
  char suffix[128];
  double tier;
} AuthorityRule;

static AuthorityRule authority_rules[512];
static int authority_count = 0;
static char spam_hosts[256][128];
static int spam_count = 0;
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;
static char configured_authority_path[512] = "static/authority.tsv";
static char configured_spam_path[512] = "static/spam_hosts.txt";

static void load_tables(void) {
  FILE *file = fopen(configured_authority_path, "r");
  if (file) {
    char line[256];
    while (fgets(line, sizeof(line), file) && authority_count < 512) {
      if (line[0] == '#' || line[0] == '\n')
        continue;
      double tier = 0;
      char suffix[128] = "";
      if (sscanf(line, "%lf\t%127s", &tier, suffix) == 2) {
        if (suffix[0] == '.')
          memmove(suffix, suffix + 1, strlen(suffix));
        snprintf(authority_rules[authority_count].suffix,
                 sizeof(authority_rules[authority_count].suffix), "%s",
                 suffix);
        authority_rules[authority_count].tier = tier;
        authority_count++;
      }
    }
    fclose(file);
  }

  file = fopen(configured_spam_path, "r");
  if (file) {
    char line[128];
    while (fgets(line, sizeof(line), file) && spam_count < 256) {
      if (line[0] == '#' || line[0] == '\n')
        continue;
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
      if (len == 0)
        continue;
      snprintf(spam_hosts[spam_count++], 128, "%s", line);
    }
    fclose(file);
  }
}

void rank_init(const char *authority_path, const char *spam_path) {
  if (authority_path && authority_path[0])
    snprintf(configured_authority_path, sizeof(configured_authority_path), "%s",
             authority_path);
  if (spam_path && spam_path[0])
    snprintf(configured_spam_path, sizeof(configured_spam_path), "%s",
             spam_path);
  pthread_once(&tables_once, load_tables);
}

static double host_matches(const char *domain, const char *suffix) {
  size_t dlen = strlen(domain);
  size_t slen = strlen(suffix);
  if (dlen == slen)
    return strcmp(domain, suffix) == 0;
  if (dlen > slen && domain[dlen - slen - 1] == '.' &&
      strcmp(domain + dlen - slen, suffix) == 0)
    return 1;
  return 0;
}

double rank_authority(const char *domain) {
  pthread_once(&tables_once, load_tables);
  double best = 0.5;
  int found = 0;
  for (int i = 0; i < authority_count; i++) {
    if (host_matches(domain, authority_rules[i].suffix)) {
      if (!found || authority_rules[i].tier > best)
        best = authority_rules[i].tier;
      found = 1;
    }
  }
  return best;
}

static int is_spam_host(const char *domain) {
  pthread_once(&tables_once, load_tables);
  for (int i = 0; i < spam_count; i++) {
    if (host_matches(domain, spam_hosts[i]))
      return 1;
  }
  return 0;
}

/* ---------- tokens ---------- */

static int tokenize_into(const char *text, char tokens[][32], int max) {
  int count = 0;
  int len = 0;
  char buf[32];
  for (const char *p = text ? text : "";; p++) {
    char c = *p;
    int word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    if (word) {
      if (len < 31)
        buf[len++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    } else {
      if (len >= 2 && count < max) {
        buf[len] = '\0';
        snprintf(tokens[count++], 32, "%s", buf);
      }
      len = 0;
      if (c == '\0')
        break;
    }
  }
  return count;
}

static int count_token(const char tokens[][32], int count, const char *needle) {
  int hits = 0;
  for (int i = 0; i < count; i++)
    if (strcmp(tokens[i], needle) == 0)
      hits++;
  return hits;
}

static int contains_ci(const char *haystack, const char *needle) {
  if (!haystack || !needle || !*needle)
    return 0;
  size_t n = strlen(needle);
  for (const char *p = haystack; *p; p++) {
    size_t i = 0;
    while (i < n && p[i]) {
      char a = p[i];
      char b = needle[i];
      if (a >= 'A' && a <= 'Z')
        a += 32;
      if (b >= 'A' && b <= 'Z')
        b += 32;
      if (a != b)
        break;
      i++;
    }
    if (i == n)
      return 1;
  }
  return 0;
}

/* ---------- scoring ---------- */

static double spam_penalty(const RankDoc *doc, const SearchQuery *query) {
  double penalty = 0.0;

  if (is_spam_host(doc->domain))
    penalty += 0.5;

  /* keyword stuffed slug: many query words inside a long path */
  const char *path = strchr(doc->url, '/');
  if (path) {
    int stuffed = 0;
    for (int i = 0; i < query->term_count; i++)
      if (contains_ci(path, query->terms[i]))
        stuffed++;
    if (stuffed >= 4 && strlen(path) >= 40)
      penalty += 0.25;
  }

  /* the title is basically the query again on an unknown host */
  if (query->term_count >= 2 && rank_authority(doc->domain) < 0.5) {
    int hits = 0;
    for (int i = 0; i < query->term_count; i++)
      if (contains_ci(doc->title, query->terms[i]))
        hits++;
    if (hits == query->term_count && strlen(doc->title) < 60)
      penalty += 0.25;
  }

  if (doc->snippet && strlen(doc->snippet) < 50)
    penalty += 0.1;

  if (penalty > 0.75)
    penalty = 0.75;
  return penalty;
}

static double url_quality(const RankDoc *doc) {
  double score = 0.0;
  if (strncasecmp(doc->url, "https://", 8) == 0)
    score += 0.4;
  const char *path = strstr(doc->url, "://");
  path = path ? strchr(path + 3, '/') : NULL;
  int depth = 0;
  for (const char *p = path; p && *p; p++)
    if (*p == '/')
      depth++;
  if (depth <= 3)
    score += 0.3;
  if (!url_has_tracking_param(doc->url))
    score += 0.3;
  return score;
}

void rank_score(RankDoc *docs, int count, const SearchQuery *query) {
  if (!docs || count <= 0)
    return;

  double max_rrf = 0.0;
  for (int i = 0; i < count; i++)
    if (docs[i].rrf > max_rrf)
      max_rrf = docs[i].rrf;
  if (max_rrf <= 0)
    max_rrf = 1.0;

  /* document frequencies over the candidate set */
  int df[8] = {0};
  for (int t = 0; t < query->term_count; t++) {
    for (int i = 0; i < count; i++) {
      char tokens[32][32];
      char text[1024];
      snprintf(text, sizeof(text), "%s %s", docs[i].title ? docs[i].title : "",
               docs[i].snippet ? docs[i].snippet : "");
      int n = tokenize_into(text, tokens, 32);
      if (count_token(tokens, n, query->terms[t]) > 0)
        df[t]++;
    }
  }

  double avgdl = 1.0;
  for (int i = 0; i < count; i++) {
    char tokens[MAX_TOKENS][32];
    char text[1024];
    snprintf(text, sizeof(text), "%s %s", docs[i].title ? docs[i].title : "",
             docs[i].snippet ? docs[i].snippet : "");
    avgdl += tokenize_into(text, tokens, MAX_TOKENS);
  }
  avgdl /= count;

  for (int i = 0; i < count; i++) {
    RankDoc *doc = &docs[i];
    char title_tokens[MAX_TOKENS][32];
    char snippet_tokens[MAX_TOKENS][32];
    char url_tokens[MAX_TOKENS][32];
    int title_n = tokenize_into(doc->title, title_tokens, MAX_TOKENS);
    int snippet_n = tokenize_into(doc->snippet, snippet_tokens, MAX_TOKENS);
    int url_n = tokenize_into(doc->url, url_tokens, MAX_TOKENS);
    int dl = title_n + snippet_n;
    if (dl < 1)
      dl = 1;

    double bm25 = 0.0;
    for (int t = 0; t < query->term_count; t++) {
      double tf = 3.0 * count_token(title_tokens, title_n, query->terms[t]) +
                  1.0 * count_token(snippet_tokens, snippet_n, query->terms[t]) +
                  0.5 * count_token(url_tokens, url_n, query->terms[t]);
      if (tf <= 0)
        continue;
      double idf = log(1.0 + ((double)count - df[t] + 0.5) / (df[t] + 0.5));
      double norm = tf * (BM25_K1 + 1.0) /
                    (tf + BM25_K1 * (1.0 - BM25_B + BM25_B * (double)dl / avgdl));
      bm25 += idf * norm;
    }
    bm25 = bm25 / (bm25 + 4.0);

    double phrase = 0.0;
    for (int p = 0; p < query->phrase_count; p++) {
      if (contains_ci(doc->title, query->phrases[p]))
        phrase = 1.0;
      else if (contains_ci(doc->snippet, query->phrases[p]) && phrase < 0.5)
        phrase = 0.5;
    }

    int engines = 0;
    for (int bit = 0; bit < 32; bit++)
      if (doc->engine_mask & (1 << bit))
        engines++;
    double consensus = (engines - 1) / 3.0;
    if (consensus < 0)
      consensus = 0;
    if (consensus > 1)
      consensus = 1;

    double authority = rank_authority(doc->domain);
    double spam = spam_penalty(doc, query);

    doc->score = W_RRF * (doc->rrf / max_rrf) + W_BM25 * bm25 +
                 W_AUTHORITY * authority + W_PHRASE * phrase +
                 W_CONSENSUS * consensus + W_URL_QUALITY * url_quality(doc) +
                 W_FRESHNESS * 0.5 - W_SPAM * spam;
  }
}

/* ---------- near duplicate titles ---------- */

static int title_jaccard(const char *a, const char *b) {
  char ta[32][32], tb[32][32];
  int na = tokenize_into(a, ta, 32);
  int nb = tokenize_into(b, tb, 32);
  if (na < 4 || nb < 4)
    return 0;
  int common = 0;
  for (int i = 0; i < na; i++)
    for (int j = 0; j < nb; j++)
      if (strcmp(ta[i], tb[j]) == 0) {
        common++;
        break;
      }
  int union_size = na + nb - common;
  if (union_size <= 0)
    return 0;
  return (common * 100 / union_size) >= 80;
}

int rank_dedupe(RankDoc *docs, int count) {
  if (!docs || count <= 1)
    return count;

  int kept = 0;
  for (int i = 0; i < count; i++) {
    int duplicate_of = -1;
    for (int j = 0; j < kept; j++) {
      if (docs[i].url && docs[j].url && strcmp(docs[i].url, docs[j].url) == 0) {
        duplicate_of = j;
        break;
      }
      if (title_jaccard(docs[i].title, docs[j].title) &&
          strcmp(docs[i].domain, docs[j].domain) != 0) {
        duplicate_of = j;
        break;
      }
    }

    if (duplicate_of < 0) {
      if (kept != i)
        docs[kept] = docs[i];
      kept++;
      continue;
    }

    /* keep the more authoritative copy, merge the engine votes */
    RankDoc *winner = &docs[duplicate_of];
    winner->engine_mask |= docs[i].engine_mask;
    if (rank_authority(docs[i].domain) > rank_authority(winner->domain)) {
      char *tmp_url = winner->url, *tmp_title = winner->title;
      char *tmp_snippet = winner->snippet, *tmp_key = winner->key;
      char tmp_domain[128];
      snprintf(tmp_domain, sizeof(tmp_domain), "%s", winner->domain);
      int tmp_mask = winner->engine_mask;

      winner->url = docs[i].url;
      winner->title = docs[i].title;
      winner->snippet = docs[i].snippet;
      winner->key = docs[i].key;
      snprintf(winner->domain, sizeof(winner->domain), "%s", docs[i].domain);
      winner->engine_mask = tmp_mask;

      docs[i].url = tmp_url;
      docs[i].title = tmp_title;
      docs[i].snippet = tmp_snippet;
      docs[i].key = tmp_key;
      snprintf(docs[i].domain, sizeof(docs[i].domain), "%s", tmp_domain);
    }
    rank_doc_free(&docs[i]);
  }
  return kept;
}

void rank_doc_free(RankDoc *doc) {
  if (!doc)
    return;
  free(doc->url);
  free(doc->title);
  free(doc->snippet);
  free(doc->key);
  doc->url = NULL;
  doc->title = NULL;
  doc->snippet = NULL;
  doc->key = NULL;
}
