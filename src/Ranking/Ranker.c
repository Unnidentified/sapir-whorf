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
#define W_REDDIT 0.22
#define W_COVERAGE 0.65

#define BM25_K1 1.2
#define BM25_B 0.75
#define MAX_TOKENS 64

static double freshness_score(const RankDoc *doc);

/* ---------- registrable domain ---------- */

/* C11: the suffix list is data, not code. A 14-entry hand list knew co.uk but
   not org.uk, gov.uk, com.pl, and nothing about hosted platforms, so every
   *.github.io site counted as one domain and one thread could fill a page. */
#define MAX_SUFFIXES 256
static char suffixes[MAX_SUFFIXES][64];
static int suffix_count = 0;
static pthread_once_t suffix_once = PTHREAD_ONCE_INIT;

static void load_suffixes(void) {
  FILE *fp = fopen("static/public_suffixes.txt", "r");
  if (!fp)
    return;
  char line[128];
  while (fgets(line, sizeof(line), fp) && suffix_count < MAX_SUFFIXES) {
    char *nl = strpbrk(line, "\r\n");
    if (nl)
      *nl = '\0';
    if (line[0] == '#' || line[0] == '\0')
      continue;
    snprintf(suffixes[suffix_count], sizeof(suffixes[0]), "%s", line);
    suffix_count++;
  }
  fclose(fp);
}

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

  char *first_dot = strchr(out, '.');
  if (!first_dot)
    return;

  /* If the host ends with a known multi-label suffix, the registrable domain
     is that suffix plus exactly one more label: bbc.co.uk, not co.uk;
     alice.github.io, not github.io. The old code computed `prev` from the
     suffix start and then required prev > out, which silently did nothing for
     a two-label host - the very case every `.co.uk` site hits. */
  pthread_once(&suffix_once, load_suffixes);
  for (int i = 0; i < suffix_count; i++) {
    size_t slen = strlen(suffixes[i]);
    size_t olen = strlen(out);
    if (olen <= slen + 1)
      continue;
    if (out[olen - slen - 1] != '.' )
      continue;
    if (strcmp(out + olen - slen, suffixes[i]) != 0)
      continue;

    /* walk back over exactly one more label */
    size_t cut = olen - slen - 1; /* index of the dot before the suffix */
    while (cut > 0 && out[cut - 1] != '.')
      cut--;
    if (cut > 0 && cut < olen)
      memmove(out, out + cut, strlen(out + cut) + 1);
    return;
  }

  /* no special suffix: the registrable domain is the last two labels */
  {
    const char *last_two = NULL;
    char *scan = strchr(out, '.');
    while ((scan = strchr(scan, '.')) != NULL) {
      scan++;
      if (strchr(scan, '.'))
        last_two = scan;
    }
    if (last_two)
      memmove(out, last_two, strlen(last_two) + 1);
  }
}

static int url_has_tracking_param(const char *url) {
  /* "utm_" missed bare "utm=1"; match prefixes both with and without the
     separator's underscore-or-equals spelling */
  static const char *TRACKERS[] = {"utm_", "utm=", "gclid=",  "fbclid=",
                                   "ref_src=", "ref=", "mc_cid=", "mc_eid=",
                                   "igshid=", "spm=", "si=", "_ga="};
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
  /* reddit serves one thread from many hosts (F3); collapse them here */
  if (strncasecmp(p, "old.reddit.com", 14) == 0)
    p += 4;
  else if (strncasecmp(p, "np.reddit.com", 13) == 0)
    p += 3;
  else if (strncasecmp(p, "new.reddit.com", 14) == 0)
    p += 4;
  else if (strncasecmp(p, "i.reddit.com", 12) == 0)
    p += 2;
  else if (strncasecmp(p, "m.", 2) == 0 || strncasecmp(p, "amp.", 4) == 0)
    p += (p[0] == 'm' ? 2 : 4);

  /* Lowercase the HOST only. Paths are case-sensitive: youtu.be/<id>,
     Wikipedia titles after the first character and paste ids all collide when
     the path is folded, and dedupe then silently drops one of them. */
  char buffer[1024];
  size_t o = 0;
  const char *query_start = NULL;
  int in_host = 1;
  for (; *p && o + 1 < sizeof(buffer); p++) {
    if (*p == '#')
      break;
    if (*p == '?') {
      query_start = p + 1;
      break;
    }
    if (*p == '/')
      in_host = 0;
    buffer[o++] = in_host ? (char)tolower((unsigned char)*p) : *p;
  }
  buffer[o] = '\0';

  /* /r/<sub>/comments/<id>/<slug>/<comment-id> -> /r/<sub>/comments/<id>
     (F3: the same thread otherwise appears several times, once per comment
     permalink or slug spelling) */
  if (strncmp(buffer, "reddit.com/r/", 13) == 0) {
    const char *comments = strstr(buffer, "/comments/");
    if (comments) {
      const char *id = comments + strlen("/comments/");
      const char *id_end = strchr(id, '/');
      if (id_end)
        buffer[(size_t)(id_end - buffer)] = '\0';
    }
  }

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
    /* the query runs to the fragment, if any: "#frag" was being folded into
       the parameter list and survived as part of the key */
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", query_start);
    char *frag = strchr(copy, '#');
    if (frag)
      *frag = '\0';
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

/* Reddit threads are often the best human answer, but only when the thread is
   actually about the query. Require the query terms to appear in the title or
   the thread URL. */
static double reddit_relevance(const RankDoc *doc, const SearchQuery *query) {
  if (!doc->domain[0] || strcmp(doc->domain, "reddit.com") != 0)
    return 0.0;
  if (query->term_count <= 0)
    return 0.0;

  int hits = 0;
  for (int i = 0; i < query->term_count; i++) {
    if (contains_ci(doc->title, query->terms[i]) ||
        contains_ci(doc->url, query->terms[i]))
      hits++;
  }

  double coverage = (double)hits / (double)query->term_count;
  if (query->term_count == 1)
    return coverage >= 1.0 ? 1.0 : 0.0;
  if (query->term_count == 2)
    return coverage >= 0.5 ? 1.0 : 0.0;
  return coverage >= 0.66 ? 1.0 : 0.0;
}

/* Bounded edit distance: "hipity" and "hippity" are the same word typed
   twice, and the coverage floor must not throw that result away. */
static int edit_distance_at_most(const char *a, const char *b, int max) {
  size_t la = strlen(a), lb = strlen(b);
  if (la > 31 || lb > 31)
    return 0;
  if ((int)(la > lb ? la - lb : lb - la) > max)
    return 0;

  int prev[32], cur[32];
  for (size_t j = 0; j <= lb; j++)
    prev[j] = (int)j;

  for (size_t i = 1; i <= la; i++) {
    cur[0] = (int)i;
    int row_min = cur[0];
    for (size_t j = 1; j <= lb; j++) {
      int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      int del = prev[j] + 1;
      int ins = cur[j - 1] + 1;
      int sub = prev[j - 1] + cost;
      int best = del < ins ? del : ins;
      if (sub < best)
        best = sub;
      cur[j] = best;
      if (best < row_min)
        row_min = best;
    }
    if (row_min > max)
      return 0;
    for (size_t j = 0; j <= lb; j++)
      prev[j] = cur[j];
  }
  return prev[lb] <= max;
}

/* does this snippet end in an ellipsis, i.e. was it cut by the engine? */
static int snippet_ellipsis(const char *snippet) {
  if (!snippet)
    return 0;
  size_t n = strlen(snippet);
  while (n > 0 && (snippet[n - 1] == ' ' || snippet[n - 1] == '\n' ||
                   snippet[n - 1] == '\r' || snippet[n - 1] == '\t'))
    n--;
  if (n == 0)
    return 0;
  if (n >= 3 && strncmp(snippet + n - 3, "...", 3) == 0)
    return 1;
  /* UTF-8 ellipsis E2 80 A6 */
  if (n >= 3 && (unsigned char)snippet[n - 3] == 0xE2 &&
      (unsigned char)snippet[n - 2] == 0x80 &&
      (unsigned char)snippet[n - 1] == 0xA6)
    return 1;
  return 0;
}

static int coverage_token_matches(const char *token, const char *term) {
  if (strcmp(token, term) == 0)
    return 1;
  if (strlen(term) < 5)
    return 0;
  if (strstr(token, term))
    return 1;
  return edit_distance_at_most(token, term, 1);
}

static int coverage_has_term(char tokens[MAX_TOKENS][32], int n, const char *term) {
  for (int i = 0; i < n; i++)
    if (coverage_token_matches(tokens[i], term))
      return 1;
  return 0;
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

  /* Document frequencies over the candidate set.
     C13: df used to be computed over title+snippet while tf and coverage also
     looked at URL tokens, so a term that appeared only in a URL had df == 0
     and was silently dropped from coverage. All three now use the same
     fields. `in_title_or_url` records whether any candidate carries the term
     in a non-truncated field, which is what licenses the snippet half-credit
     below. */
  int df[8] = {0};
  int in_title_or_url[8] = {0};
  for (int t = 0; t < query->term_count; t++) {
    for (int i = 0; i < count; i++) {
      char tokens[32][32];
      char text[1024];
      snprintf(text, sizeof(text), "%s %s %s",
               docs[i].title ? docs[i].title : "",
               docs[i].snippet ? docs[i].snippet : "",
               docs[i].url ? docs[i].url : "");
      int n = tokenize_into(text, tokens, 32);
      if (count_token(tokens, n, query->terms[t]) > 0)
        df[t]++;

      char tu[32][32];
      char tu_text[768];
      snprintf(tu_text, sizeof(tu_text), "%s %s",
               docs[i].title ? docs[i].title : "",
               docs[i].url ? docs[i].url : "");
      int tun = tokenize_into(tu_text, tu, 32);
      if (count_token(tu, tun, query->terms[t]) > 0)
        in_title_or_url[t] = 1;
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

    /* Consensus counts independent INDEXES, not engines (F2): Yahoo's web
       results are largely Bing's and DuckDuckGo's partly so, so Bing+Yahoo
       agreeing is one opinion with one failure mode, not two. Counting them
       separately inflated consensus exactly when the shared index was wrong. */
    const char *groups[8] = {0};
    int group_count = 0;
    for (int bit = 0; bit < 32; bit++) {
      if (!(doc->engine_mask & (1u << bit)))
        continue;
      if (!doc->engine_groups[bit])
        continue;
      int seen = 0;
      for (int g = 0; g < group_count; g++) {
        if (strcmp(groups[g], doc->engine_groups[bit]) == 0) {
          seen = 1;
          break;
        }
      }
      if (!seen && group_count < 8)
        groups[group_count++] = doc->engine_groups[bit];
    }
    double consensus = (group_count - 1) / 3.0;
    if (consensus < 0)
      consensus = 0;
    if (consensus > 1)
      consensus = 1;

    double authority = rank_authority(doc->domain);
    double spam = spam_penalty(doc, query);

    /* IDF-weighted coverage: a page that only matches the common word of the
       query ("best hotels" for "best mechanical keyboard") must not win on
       engine consensus or domain authority alone. */
    /* A snippet the engine truncated tells us nothing about the words that
       were cut off. Bing truncates 406 of 472 snippets to ~126 chars while
       Yahoo averages 212, so charging a term as missing penalised the page for
       the engine's formatting, not its relevance. Terms absent from a
       truncated snippet are treated as unknown and dropped from the
       denominator, exactly like terms no candidate contains. */
    int snippet_truncated = snippet_ellipsis(doc->snippet);

    double idf_sum = 0.0;
    double idf_hit = 0.0;
    for (int t = 0; t < query->term_count; t++) {
      /* opinion fillers are not topical: they must not dilute coverage */
      if (query_is_opinion_modifier(query->terms[t]))
        continue;
      /* a term no candidate contains cannot be matched by anything: counting
         it as missing would let one typo blank an otherwise good page */
      if (df[t] == 0)
        continue;
      int hit = coverage_has_term(title_tokens, title_n, query->terms[t]) ||
                coverage_has_term(snippet_tokens, snippet_n, query->terms[t]) ||
                coverage_has_term(url_tokens, url_n, query->terms[t]);
      double idf = log(1.0 + ((double)count - df[t] + 0.5) / (df[t] + 0.5));
      if (!hit && snippet_truncated) {
        /* Unknown, not absent: the engine cut the text, so half credit. Fully
           dropping the term pushed every truncated result to coverage 1.0 and
           destroyed the signal (measured: hit@3 0.41 -> 0.29).

           The credit is capped, though: if the term appears in NO candidate's
           title or URL, the snippet says nothing and the query is simply
           unrelated ("zzqqxx vbbnmm"). Without this cap every nonsense query
           scored ~0.5 and rendered junk (measured: 13 results for garbage). */
        if (in_title_or_url[t]) {
          idf_sum += idf;
          idf_hit += idf * 0.5;
        } else {
          continue;
        }
        continue;
      }
      idf_sum += idf;
      if (hit)
        idf_hit += idf;
    }
    /* nothing matched at all: every term was unknown to the result set */
    double coverage = idf_sum > 0.0 ? idf_hit / idf_sum : 0.0;

    doc->coverage = coverage;
    doc->score = W_RRF * (doc->rrf / max_rrf) + W_BM25 * bm25 +
                 W_AUTHORITY * authority + W_PHRASE * phrase +
                 W_CONSENSUS * consensus + W_URL_QUALITY * url_quality(doc) +
                 W_FRESHNESS * freshness_score(doc) - W_SPAM * spam +
                 W_REDDIT * reddit_relevance(doc, query) -
                 W_COVERAGE * (1.0 - coverage);
  }
}

/* C13: freshness was the constant 0.5 for every document while SETUP described
   "date fragments in snippet". Engines do put a date at the start of the
   snippet ("2 days ago - ...", "Jan 4, 2025 - ..."), so read it. Anything we
   cannot date scores neutral. */
static double freshness_score(const RankDoc *doc) {
  if (!doc || !doc->snippet)
    return 0.5;

  const char *s = doc->snippet;
  time_t now = time(NULL);
  struct tm tm;
  double result = 0.5;

  /* relative forms */
  if (strstr(s, "hour ago") || strstr(s, "hours ago"))
    return 0.9;
  if (strstr(s, "minute ago") || strstr(s, "minutes ago"))
    return 0.95;
  if (strstr(s, "day ago") || strstr(s, "days ago"))
    return 0.85;
  if (strstr(s, "week ago") || strstr(s, "weeks ago"))
    return 0.7;
  if (strstr(s, "month ago") || strstr(s, "months ago"))
    return 0.5;
  if (strstr(s, "year ago") || strstr(s, "years ago"))
    return 0.3;

  /* absolute dates: only within the first ~24 characters, where engines put
     them; matching mid-snippet picks up incidental numbers */
  char head[32];
  snprintf(head, sizeof(head), "%s", s);
  memset(&tm, 0, sizeof(tm));
  const char *formats[] = {"%b %d, %Y", "%d %b %Y", "%Y-%m-%d", "%b %d %Y"};
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
    if (strptime(head, formats[i], &tm) != NULL) {
      tm.tm_isdst = -1;
      time_t then = mktime(&tm);
      if (then > 0 && then <= now) {
        double years = (double)(now - then) / (365.25 * 86400.0);
        if (years < 0.25)
          result = 0.9;
        else if (years < 1.0)
          result = 0.7;
        else if (years < 3.0)
          result = 0.45;
        else
          result = 0.25;
      }
      break;
    }
  }
  return result;
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
      /* C13: title similarity alone merged DIFFERENT pages across sites -
         "How to install X on Ubuntu 22.04" from two vendors is not a
         duplicate. Require the snippets to agree as well, and only then
         across domains; the same-domain case is already covered by the
         canonical URL test above. */
      if (strcmp(docs[i].domain, docs[j].domain) != 0 &&
          title_jaccard(docs[i].title, docs[j].title) &&
          title_jaccard(docs[i].snippet, docs[j].snippet)) {
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
