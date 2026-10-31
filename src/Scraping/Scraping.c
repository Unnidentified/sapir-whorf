#include "Scraping.h"
#include <pthread.h>
#include "BlockDetection.h"
#include "Replay.h"

__thread int batch_transport_outage_hint = 0;
#include "../Ranking/Ranker.h"

/* set by the results handler when the request asked to wait for all sources */
__thread int request_is_patient = 0;
#include "EngineHealth.h"
#include "../Cache/Cache.h"
#include "../Proxy/Proxy.h"
#include "../Utility/XmlHelper.h"
#include "Config.h"
#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void cookie_jar_path(const char *engine_id, char *out, size_t size);

/* C14: one jar file per engine was read and rewritten by every concurrent
   request (curl rewrites it at handle cleanup), and a failure path remove()d it
   while other requests were mid-flight. Serialise jar use per engine so a
   cookie read cannot see a half-written file and a removal cannot race a load.
   Only Yahoo really needs a jar; the others are stateless by design. */
#define MAX_JAR_LOCKS 16
static pthread_mutex_t jar_locks[MAX_JAR_LOCKS];
static int jar_locks_ready = 0;

static pthread_mutex_t *jar_lock_for(const char *engine_id) {
  static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&init_mutex);
  if (!jar_locks_ready) {
    for (int i = 0; i < MAX_JAR_LOCKS; i++)
      pthread_mutex_init(&jar_locks[i], NULL);
    jar_locks_ready = 1;
  }
  pthread_mutex_unlock(&init_mutex);

  unsigned int h = 0;
  for (const char *p = engine_id ? engine_id : ""; *p; p++)
    h = h * 31u + (unsigned char)*p;
  return &jar_locks[h % MAX_JAR_LOCKS];
}

static int response_contains(const char *response, const char *needle) {
  return response && needle && strstr(response, needle) != NULL;
}

/* anti-bot interstitials served instead of a results page */
/* Dispatch on the registry id, not the display name: the name is prose and
   changed once already ("DuckDuckGo Lite" -> "DuckDuckGo"), which silently
   dropped this whole branch. */
static int response_looks_like_results_page(const ScrapeJob *job,
                                            const char *response) {
  if (!job || !job->engine || !response)
    return 0;

  const char *id = job->engine->id;

  if (strcmp(id, "ddg") == 0 || strcmp(id, "ddg-lite") == 0) {
    return response_contains(response, "result__a") ||
           response_contains(response, "result-link") ||
           response_contains(response, "result__snippet") ||
           response_contains(response, "result-snippet");
  }

  if (strcmp(id, "bing") == 0) {
    return response_contains(response, "b_algo") ||
           response_contains(response, "b_results");
  }

  if (strcmp(id, "brave") == 0) {
    return response_contains(response, "search-snippet-title") ||
           response_contains(response, "data-type=\"web\"");
  }

  if (strcmp(id, "yahoo") == 0) {
    return response_contains(response, "algo-sr") ||
           response_contains(response, "compTitle") ||
           response_contains(response, "compText");
  }

  if (strncmp(id, "yacy", 4) == 0) {
    return response_contains(response, "<item") ||
           response_contains(response, "<rss");
  }

  return 0;
}

/* An engine that answers 200 and says "no matches" has worked correctly and
   found nothing. Treating that as "we could not parse the results" is a false
   alarm that hides the real answer from /info and from the operator. */
static int response_declares_no_results(const ScrapeJob *job,
                                        const char *response) {
  if (!job || !job->engine || !response)
    return 0;
  const char *id = job->engine->id;
  if (strcmp(id, "bing") == 0)
    return response_contains(response, "b_no") ||
           response_contains(response, "There are no results");
  if (strcmp(id, "google") == 0 || strcmp(id, "google_cse") == 0)
    return response_contains(response, "did not match any documents");
  if (strcmp(id, "ddg") == 0 || strcmp(id, "ddg-lite") == 0)
    return response_contains(response, "No results found") ||
           response_contains(response, "No results.");
  return 0;
}

static size_t capture_header(char *buffer, size_t size, size_t nitems,
                            void *userdata) {
  ScrapeJob *job = userdata;
  size_t length = size * nitems;
  if (job && length > 12 &&
      strncasecmp(buffer, "retry-after:", 12) == 0) {
    job->retry_after = (int)strtol(buffer + 12, NULL, 10);
  }
  return length;
}

static void maybe_dump_response(const ScrapeJob *job, const char *response) {
  const char *want = getenv("OMNI_DUMP");
  if (!want || !job || !job->engine || !response)
    return;
  if (strcmp(want, job->engine->id) != 0)
    return;
  char path[256];
  snprintf(path, sizeof path, "/tmp/dump-%s.html", job->engine->id);
  FILE *fp = fopen(path, "w");
  if (!fp)
    return;
  fwrite(response, 1, strlen(response), fp);
  fclose(fp);
}

static void classify_job_response(ScrapeJob *job, const char *response,
                                  size_t response_size) {
  job->results_count = 0;
  maybe_dump_response(job, response);

  if (!response || response_size == 0) {
    job->status = SCRAPE_STATUS_FETCH_ERROR;
    return;
  }

  if (job->engine->body_parser) {
    job->results_count = job->engine->body_parser(
        response, response_size, job->lang, job->out_results, job->max_results);
    if (job->results_count > 0) {
      job->status = SCRAPE_STATUS_OK;
      return;
    }
    if (job->http_status >= 400) {
      job->status = SCRAPE_STATUS_FETCH_ERROR;
      return;
    }
    if (response_is_blocked_for_engine(response, job->results_count)) {
      job->status = SCRAPE_STATUS_BLOCKED;
      return;
    }
    job->status = SCRAPE_STATUS_EMPTY;
    return;
  }

  xmlDocPtr doc;
  if (job->engine->is_xml) {
    doc = xmlReadMemory(response, response_size, NULL, NULL,
                        XML_PARSE_RECOVER | XML_PARSE_NOERROR |
                            XML_PARSE_NOWARNING);
  } else {
    doc = htmlReadMemory(response, response_size, NULL, NULL,
                         HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
                             HTML_PARSE_NOWARNING);
  }

  if (!doc) {
    job->status = SCRAPE_STATUS_FETCH_ERROR;
    return;
  }

  job->results_count = job->engine->parser(job->engine->name, doc,
                                           job->out_results, job->max_results);
  xmlFreeDoc(doc);

  if (job->results_count > 0) {
    job->status = SCRAPE_STATUS_OK;
    return;
  }

  if (job->http_status >= 400) {
    job->status = SCRAPE_STATUS_FETCH_ERROR;
    return;
  }

  /* Only now, with zero results in hand, is "blocked" a useful judgement: a
     page that parsed results can never be a challenge page, whatever words its
     snippets contain. */
  if (response_is_blocked_for_engine(response, job->results_count)) {
    job->status = SCRAPE_STATUS_BLOCKED;
    return;
  }

  if (response_declares_no_results(job, response)) {
    job->status = SCRAPE_STATUS_EMPTY;
    return;
  }

  if (response_looks_like_results_page(job, response)) {
    job->status = SCRAPE_STATUS_PARSE_MISMATCH;
    return;
  }

  job->status = SCRAPE_STATUS_EMPTY;
}

static void job_cache_salt(const ScrapeJob *job, char *out, size_t size) {
  snprintf(out, size, "%s|%s", job->engine ? job->engine->name : "?",
           job->lang[0] ? job->lang : "en");
}

int check_cache_for_job(ScrapeJob *job) {
  if (get_cache_ttl_search() <= 0)
    return 0;

  char salt[128];
  job_cache_salt(job, salt, sizeof(salt));
  char *key = cache_compute_key(job->query, job->page, salt);
  if (!key)
    return 0;

  char *cached_data = NULL;
  size_t cached_size = 0;

  if (cache_get(key, (time_t)get_cache_ttl_search(), &cached_data,
                &cached_size) == 0 &&
      cached_data && cached_size > 0) {
    classify_job_response(job, cached_data, cached_size);

    if (job->status == SCRAPE_STATUS_BLOCKED) {
      free(cached_data);
      free(key);
      return 0;
    }

    free(cached_data);
    free(key);

    if (job->results_count == 0)
      return 0;

    return 1;
  }

  free(key);
  return 0;
}

#define STALE_CACHE_TTL (7 * 86400)

static void stale_cache_key(const ScrapeJob *job, char *out, size_t size) {
  char salt[160];
  /* the "stale_" tag is load-bearing: cache_cleanup sweeps this class at the
     7-day TTL while everything else sweeps at the search TTL */
  snprintf(salt, sizeof(salt), "stale_%s|%s",
           job->engine ? job->engine->name : "?",
           job->lang[0] ? job->lang : "en");
  char *key = cache_compute_key(job->query, job->page, salt);
  if (key) {
    snprintf(out, size, "%s", key);
    free(key);
  } else {
    out[0] = '\0';
  }
}

/* a failed fetch falls back to the last body that produced results */
static int load_stale_results(ScrapeJob *job) {
  char key[BUFFER_SIZE_LARGE];
  stale_cache_key(job, key, sizeof(key));
  if (!key[0])
    return 0;

  char *cached = NULL;
  size_t cached_size = 0;
  if (cache_get(key, STALE_CACHE_TTL, &cached, &cached_size) != 0 ||
      !cached || cached_size == 0) {
    free(cached);
    return 0;
  }

  classify_job_response(job, cached, cached_size);
  free(cached);

  if (job->results_count > 0) {
    job->stale = 1;
    job->status = SCRAPE_STATUS_OK;
    return 1;
  }
  return 0;
}

void parse_and_cache_response(ScrapeJob *job) {
  if (job->response.size == 0) {
    job->results_count = 0;
    job->status = SCRAPE_STATUS_FETCH_ERROR;
    return;
  }

  classify_job_response(job, job->response.memory, job->response.size);

  if (job->status == SCRAPE_STATUS_OK || job->status == SCRAPE_STATUS_EMPTY) {
    char salt[128];
    job_cache_salt(job, salt, sizeof(salt));
    char *key = cache_compute_key(job->query, job->page, salt);
    if (key && get_cache_ttl_search() > 0)
      cache_set(key, job->response.memory, job->response.size);
    free(key);
  }

  if (job->status == SCRAPE_STATUS_OK) {
    char stale_key[BUFFER_SIZE_LARGE];
    stale_cache_key(job, stale_key, sizeof(stale_key));
    if (stale_key[0])
      cache_set(stale_key, job->response.memory, job->response.size);
  }
}

static void cleanup_job_results(ScrapeJob *job) {
  if (!job || !job->out_results)
    return;

  xml_result_free(*job->out_results, job->results_count);
  *job->out_results = NULL;
  job->results_count = 0;
}

static void cleanup_job_handle(ScrapeJob *job, CURL *handle) {
  struct curl_slist *headers = NULL;
  if (handle)
    curl_easy_getinfo(handle, CURLINFO_PRIVATE, &headers);
  if (headers)
    curl_slist_free_all(headers);

  free(job->response.memory);
  job->response.memory = NULL;
  job->response.size = 0;
  job->response.capacity = 0;
}

void process_response(ScrapeJob *job, CURL *handle, CURLMsg *msg) {
  struct timespec finished;
  clock_gettime(CLOCK_MONOTONIC, &finished);
  job->elapsed_ms = (double)(finished.tv_sec - job->started.tv_sec) * 1000.0 +
                    (double)(finished.tv_nsec - job->started.tv_nsec) / 1e6;

  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &job->http_status);

  if (msg->data.result == CURLE_OK)
    parse_and_cache_response(job);
  else {
    job->results_count = 0;
    job->status = SCRAPE_STATUS_FETCH_ERROR;
    /* classify: a transport failure is our network, not the engine's fault */
    switch (msg->data.result) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_PARTIAL_FILE:
      job->failure_kind = FETCH_FAIL_TRANSPORT;
      break;
    default:
      job->failure_kind = FETCH_FAIL_REFUSED;
      break;
    }
    if (getenv("OMNI_PERF") && job->engine)
      fprintf(stderr, "[FETCHERR] %s: %s (http=%ld)\n", job->engine->id,
              curl_easy_strerror(msg->data.result), job->http_status);
  }

  /* Health must judge the live fetch, never the substitution below it: a stale
     body standing in for a blocked engine used to report success, so the
     breaker could never open for any engine that had ever worked. */
  int live_status = (int)job->status;
  int live_count = job->results_count;

  if (job->results_count == 0 &&
      job->status != SCRAPE_STATUS_EMPTY) {
    load_stale_results(job);
  }

  if (getenv("OMNI_PERF") && job->engine)
    fprintf(stderr, "[ENGINE] %s %.0fms status=%d results=%d http=%ld%s\n",
            job->engine->id, job->elapsed_ms, live_status, job->results_count,
            job->http_status, job->stale ? " stale=1" : "");

  if (replay_enabled_record() && job->out_results && job->out_results[0])
    replay_record_job(job, job->out_results[0], job->results_count);

  if (job->engine && job->status == SCRAPE_STATUS_SKIPPED) {
    /* G2: we never asked. Charging the engine for our own pacing would pause
       it for the one reason that is entirely our idea. */
  } else if (job->engine) {
    /* C8: one transport failure among many successes is the engine; many at
       once is our network. The batch-level flag is set by the caller before
       the responses are drained, so this is a cheap local decision. */
    extern __thread int batch_transport_outage_hint;
    int outage = (job->failure_kind == FETCH_FAIL_TRANSPORT) &&
                 batch_transport_outage_hint;
    if (outage)
      engine_health_set_batch_outage(1);
    engine_health_record(job->engine->id, live_status, job->http_status,
                         job->elapsed_ms, job->retry_after);
    if (outage)
      engine_health_set_batch_outage(0);
  }

  /* A challenge or region cookie poisons every later request: drop the jar on
     a block, and also when a page that should have results came back empty.
     Bing silently serves its "no results" shell to a poisoned session. */
  int zero_web_results =
      job->engine && (job->engine->verticals & V_WEB) &&
      live_count == 0 && job->http_status == 200;
  if (job->engine &&
      (job->status == SCRAPE_STATUS_BLOCKED || job->http_status == 429 ||
       job->http_status == 202 || job->http_status >= 500 ||
       zero_web_results)) {
    char jar_path[BUFFER_SIZE_LARGE];
    cookie_jar_path(job->engine->id, jar_path, sizeof(jar_path));
    if (jar_path[0]) {
      pthread_mutex_lock(jar_lock_for(job->engine->id));
      remove(jar_path);
      pthread_mutex_unlock(jar_lock_for(job->engine->id));
    }
  }

  cleanup_job_handle(job, handle);
}

/* These engines serve a worse page to a session cookie than to a clean client:
   Bing in particular answers a repeat visit with its empty "no results" shell.
   Yahoo is the opposite: it 307s to a bot-check gif that sets YBV, and only a
   cookie jar lets that round trip finish, so it keeps its jar. */
static int engine_wants_no_cookies(const char *engine_id) {
  return engine_id &&
         (!strcmp(engine_id, "bing") || !strcmp(engine_id, "ddg") ||
          !strcmp(engine_id, "mojeek"));
}

static void cookie_jar_path(const char *engine_id, char *out, size_t size) {
  out[0] = '\0';
  const char *dir = cache_get_dir();
  if (!dir || !dir[0] || !engine_id || !engine_id[0])
    return;
  if (engine_wants_no_cookies(engine_id))
    return;
  snprintf(out, size, "%s/cookies/%s.txt", dir, engine_id);
}

static void append_locale_params(const SearchEngine *engine, const ScrapeJob *job,
                                 char *url, size_t size) {
  if (!engine || !url || !job->lang[0])
    return;

  char region_lower[8] = "";
  for (size_t i = 0; job->region[i] && i + 1 < sizeof(region_lower); i++)
    region_lower[i] = (char)((job->region[i] >= 'A' && job->region[i] <= 'Z')
                                 ? job->region[i] + 32
                                 : job->region[i]);

  size_t used = strlen(url);
  if (strcmp(engine->id, "ddg") == 0 && region_lower[0])
    snprintf(url + used, size - used, "&kl=%s-%s", region_lower, job->lang);
  else if (strcmp(engine->id, "bing") == 0) {
    /* Bing answers every locale parameter (setlang, cc, mkt) with its empty
       shell, so the browser's Accept-Language header is the only safe signal. */
  }
  else if (strcmp(engine->id, "brave") == 0)
    snprintf(url + used, size - used, "&country=%s&search_lang=%s",
             job->region[0] ? job->region : "US", job->lang);
}

int setup_job(ScrapeJob *job, CURLM *multi_handle) {
  if (job->handle) {
    cleanup_job_handle(job, job->handle);
    curl_easy_cleanup(job->handle);
    job->handle = NULL;
  } else if (job->response.memory) {
    free(job->response.memory);
    job->response.memory = NULL;
    job->response.size = 0;
    job->response.capacity = 0;
  }

  cleanup_job_results(job);
  job->http_status = 0;
  job->retry_after = 0;
  job->status = SCRAPE_STATUS_PENDING;
  job->stale = 0;
  job->elapsed_ms = 0;
  clock_gettime(CLOCK_MONOTONIC, &job->started);

  if (check_cache_for_job(job))
    return 0;

  /* G2: pacing happens before we spend the request. A cache hit above does not
     cost a token, because it never leaves the machine. */
  if (job->engine && !engine_health_take_token(job->engine->id)) {
    job->status = SCRAPE_STATUS_SKIPPED;
    return 0;
  }

  cleanup_job_results(job);

  char *encoded_query = curl_easy_escape(NULL, job->query, 0);
  if (!encoded_query) {
    job->status = SCRAPE_STATUS_FETCH_ERROR;
    return -1;
  }

  char *read = encoded_query;
  char *write = encoded_query;
  while (*read) {
    if (read[0] == '%' && read[1] == '2' && read[2] == '0') {
      *write++ = '+';
      read += 3;
    } else {
      *write++ = *read++;
    }
  }
  *write = '\0';

  char *full_url = job->engine->build_url
                       ? job->engine->build_url(encoded_query, job->page,
                                                job->lang)
                       : build_search_url(job->engine->base_url,
                                          job->engine->page_param,
                                          job->engine->page_multiplier,
                                          job->engine->page_base,
                                          encoded_query, job->page);
  free(encoded_query);

  if (full_url)
    append_locale_params(job->engine, job, full_url, BUFFER_SIZE_LARGE);

  /* Bing serves an empty shell for `first=1`, which is the same as page one */
  if (full_url && job->engine && strcmp(job->engine->id, "bing") == 0 &&
      job->page <= 1) {
    char *first_param = strstr(full_url, "&first=1");
    if (first_param)
      memmove(first_param, first_param + strlen("&first=1"),
              strlen(first_param + strlen("&first=1")) + 1);
  }

  if (!full_url) {
    job->status = SCRAPE_STATUS_FETCH_ERROR;
    return -1;
  }

  if (getenv("OMNI_PERF"))
  {
    /* API keys travel in the query string (Google CSE's key=, for one), so the
       URL log is redacted before it is written. */
    char safe[BUFFER_SIZE_XLARGE];
    snprintf(safe, sizeof(safe), "%s", full_url);
    const char *secrets[] = {"key=", "apikey=", "api_key=", "token=",
                             "access_token=", "secret="};
    for (size_t k = 0; k < sizeof(secrets) / sizeof(secrets[0]); k++) {
      char *hit = strcasestr(safe, secrets[k]);
      if (!hit)
        continue;
      char *val = hit + strlen(secrets[k]);
      char *end = strpbrk(val, "& \"");
      if (end)
        snprintf(val, strlen(val) + 1, "REDACTED%s", end);
      else
        snprintf(val, strlen(val) + 1, "REDACTED");
    }
    fprintf(stderr, "[URL] %s %s\n", job->engine->id, safe);
  }

  job->handle = curl_easy_init();
  if (!job->handle) {
    free(full_url);
    job->status = SCRAPE_STATUS_FETCH_ERROR;
    return -1;
  }

  job->response.memory = (char *)malloc(INITIAL_BUFFER_SIZE);
  job->response.size = 0;
  job->response.capacity = INITIAL_BUFFER_SIZE;
  if (!job->response.memory) {
    curl_easy_cleanup(job->handle);
    job->handle = NULL;
    job->response.capacity = 0;
    free(full_url);
    job->status = SCRAPE_STATUS_FETCH_ERROR;
    return -1;
  }

  struct curl_slist *headers =
      build_request_headers(job->engine->host_header, job->engine->referer,
                            job->accept_language,
                            job->engine->browser_headers);

  configure_curl_handle(job->handle, full_url, &job->response, headers,
                        job->engine ? job->engine->timeout_secs : 0,
                        job->engine ? job->engine->id : NULL,
                        job->engine ? job->engine->user_agent : NULL,
                        job->engine ? job->engine->browser_headers : 0);

  /* per-engine cookie jar: some engines only answer after a challenge cookie */
  {
    char jar_path[BUFFER_SIZE_LARGE];
    cookie_jar_path(job->engine ? job->engine->id : NULL, jar_path,
                    sizeof(jar_path));
    if (jar_path[0]) {
      curl_easy_setopt(job->handle, CURLOPT_COOKIEFILE, jar_path);
      curl_easy_setopt(job->handle, CURLOPT_COOKIEJAR, jar_path);
      /* seed locale/shape cookies on the very first request; the jar keeps
         them from then on */
      if (job->engine->init_cookies && job->engine->init_cookies[0])
        curl_easy_setopt(job->handle, CURLOPT_COOKIE,
                         job->engine->init_cookies);
    }
  }
  curl_easy_setopt(job->handle, CURLOPT_PRIVATE, headers);
  curl_easy_setopt(job->handle, CURLOPT_HEADERFUNCTION, capture_header);
  curl_easy_setopt(job->handle, CURLOPT_HEADERDATA, (void *)job);

  free(full_url);
  CURLMcode add_result = curl_multi_add_handle(multi_handle, job->handle);
  if (add_result != CURLM_OK) {
    cleanup_job_handle(job, job->handle);
    curl_easy_cleanup(job->handle);
    job->handle = NULL;
    job->status = SCRAPE_STATUS_FETCH_ERROR;
    return -1;
  }
  return 0;
}

static void cleanup_unfinished_jobs(CURLM *multi_handle, ScrapeJob *jobs,
                                    int num_jobs) {
  for (int i = 0; i < num_jobs; i++) {
    if (!jobs[i].handle)
      continue;

    curl_multi_remove_handle(multi_handle, jobs[i].handle);
    cleanup_job_handle(&jobs[i], jobs[i].handle);
    curl_easy_cleanup(jobs[i].handle);
    jobs[i].handle = NULL;

    if (jobs[i].status == SCRAPE_STATUS_PENDING) {
      jobs[i].status = SCRAPE_STATUS_CANCELLED;
      if (jobs[i].engine)
        engine_health_record_cancelled(jobs[i].engine->id);
    }
  }
}

/* Process whatever finished since the last call. Returns the number of jobs
   that completed in this pass and adds their results to *results_out. */
static int drain_responses(CURLM *multi_handle, ScrapeJob *jobs, int num_jobs,
                           int *results_out) {
  CURLMsg *msg;
  int msgs_left;
  int completed = 0;

  while ((msg = curl_multi_info_read(multi_handle, &msgs_left))) {
    if (msg->msg != CURLMSG_DONE)
      continue;

    CURL *handle = msg->easy_handle;

    for (int i = 0; i < num_jobs; i++) {
      if (jobs[i].handle && jobs[i].handle == handle) {
        process_response(&jobs[i], handle, msg);
        curl_multi_remove_handle(multi_handle, handle);
        curl_easy_cleanup(handle);
        jobs[i].handle = NULL;
        if (results_out)
          *results_out += jobs[i].results_count;
        /* only an engine that actually produced results is a second opinion:
           a fast failure must not trigger the early exit */
        if (jobs[i].results_count > 0)
          completed++;
        break;
      }
    }
  }

  return completed;
}

int handle_responses(CURLM *multi_handle, ScrapeJob *jobs, int num_jobs) {
  return drain_responses(multi_handle, jobs, num_jobs, NULL);
}

int should_retry(ScrapeJob *jobs, int num_jobs) {
  if (proxy_count <= 0)
    return 0;

  for (int i = 0; i < num_jobs; i++) {
    if (jobs[i].status == SCRAPE_STATUS_FETCH_ERROR ||
        jobs[i].status == SCRAPE_STATUS_BLOCKED)
      return 1;
  }
  return 0;
}

/* every engine came back with nothing — almost always a transient block */
static int all_jobs_empty(ScrapeJob *jobs, int num_jobs) {
  if (num_jobs <= 0)
    return 0;

  for (int i = 0; i < num_jobs; i++) {
    if (jobs[i].results_count > 0)
      return 0;
  }
  return 1;
}

/* Projected yield: the number of results the page can actually render, which
   is NOT the raw hit count. The old early exit counted raw hits, so two engines
   with four results each ended the batch at 1.2s even when dedupe and the
   relevance floor would leave three. This counts what survives instead. */
static int projected_yield(ScrapeJob *jobs, int num_jobs, int per_domain_cap) {
  char domains[128][128];
  int domain_counts[128];
  int ndomains = 0;
  int yield_total = 0;

  for (int i = 0; i < num_jobs; i++) {
    if (!jobs[i].out_results || !jobs[i].out_results[0])
      continue;
    for (int r = 0; r < jobs[i].results_count; r++) {
      const char *url = jobs[i].out_results[0][r].url;
      if (!url)
        continue;
      char dom[128] = {0};
      rank_domain_of(url, dom, sizeof(dom));
      if (!dom[0])
        continue;

      int slot = -1;
      for (int d = 0; d < ndomains; d++) {
        if (strcmp(domains[d], dom) == 0) {
          slot = d;
          break;
        }
      }
      if (slot < 0) {
        if (ndomains >= 128)
          continue;
        slot = ndomains++;
        snprintf(domains[slot], sizeof(domains[slot]), "%s", dom);
        domain_counts[slot] = 0;
      }
      if (domain_counts[slot] < per_domain_cap) {
        domain_counts[slot]++;
        yield_total++;
      }
    }
  }
  return yield_total;
}

int scrape_engines_parallel(ScrapeJob *jobs, int num_jobs) {
  /* measurement hook: OMNI_LEGACY restores the pre-optimisation batch policy */
  int legacy = getenv("OMNI_LEGACY") != NULL;
  /* hard cap: never let one slow engine set the page latency.
     soft cap: when the page would still be thin, allow a little longer. */
  long batch_cap_ms = legacy ? 7000 : 2000;
  long soft_cap_ms = legacy ? 7000 : 3000;
  /* &patient=1 (or OMNI_PATIENT=1) waits for every engine up to the hard cap */
  int patient = getenv("OMNI_PATIENT") != NULL;

  if (!legacy && !patient) {
    /* &patient=1 is set by the results handler for the current request */
    if (request_is_patient)
      patient = 1;
  }
  int retries = 0;
  int empty_retry_done = 0;

retry:;
  struct timespec batch_start;
  clock_gettime(CLOCK_MONOTONIC, &batch_start);

  CURLM *multi_handle = curl_multi_init();
  if (!multi_handle)
    return -1;

  /* Offline replay: if a recording is active, jobs answered from it never open
     a socket. This is how the ranking benchmark runs with zero network. */
  if (replay_enabled_read()) {
    int replayed = 0;
    for (int i = 0; i < num_jobs; i++) {
      if (replay_fill_job(&jobs[i]))
        replayed++;
    }
    if (replayed == num_jobs) {
      curl_multi_cleanup(multi_handle);
      return replayed;
    }
  }

  for (int i = 0; i < num_jobs; i++) {
    setup_job(&jobs[i], multi_handle);
  }

  /* No artificial delay here. It slept 100-200ms before the first
     curl_multi_perform, including when every job had been served from cache -
     a quarter of the page latency, and jitter that did nothing useful because
     every engine started at once anyway. */

  /* results already served from cache count towards the early exit */
  int total_so_far = 0;
  int engines_so_far = 0;
  for (int i = 0; i < num_jobs; i++) {
    if (!jobs[i].handle && jobs[i].results_count > 0) {
      total_so_far += jobs[i].results_count;
      engines_so_far++;
    }
  }

  int still_running = 0;
  CURLMcode mc = curl_multi_perform(multi_handle, &still_running);

  while (mc == CURLM_OK && still_running) {
    int numfds = 0;
    mc = curl_multi_wait(multi_handle, NULL, 0, 60, &numfds);
    if (mc != CURLM_OK)
      break;
    mc = curl_multi_perform(multi_handle, &still_running);

    engines_so_far += drain_responses(multi_handle, jobs, num_jobs,
                                      &total_so_far);

    /* C8: if at least two engines have now failed and every failure so far is
       transport-level, the common cause is this host's network, so later
       failures in this batch must not be charged to the engines' breakers. */
    {
      int failed = 0, transport = 0, done = 0;
      for (int i = 0; i < num_jobs; i++) {
        if (jobs[i].status == SCRAPE_STATUS_FETCH_ERROR) {
          failed++;
          if (jobs[i].failure_kind == FETCH_FAIL_TRANSPORT)
            transport++;
        }
        if (jobs[i].status != SCRAPE_STATUS_PENDING)
          done++;
      }
      batch_transport_outage_hint = (failed >= 2 && failed == transport);
    }

    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    long elapsed_ms =
        (long)((now_ts.tv_sec - batch_start.tv_sec) * 1000 +
               (now_ts.tv_nsec - batch_start.tv_nsec) / 1000000);
    /* Exit on projected yield, not on raw hits (F1): a page that will render
       three results is not a page that can be left alone. The batch stops
       early only when enough results would survive dedupe and the domain cap
       to fill a page; otherwise the remaining engines are allowed to answer
       up to a softer cap. A "trusted" engine is one whose SERP we rank highly
       (Yahoo, Brave); Bing and DuckDuckGo answer fast but sometimes with
       relaxed junk, so their answer alone must not end the batch. */
    double best_weight = 0.0;
    for (int i = 0; i < num_jobs; i++)
      if (jobs[i].results_count > 0 && jobs[i].engine &&
          jobs[i].engine->weight > best_weight)
        best_weight = jobs[i].engine->weight;
    int trusted = best_weight >= 1.1;

    int projected = projected_yield(jobs, num_jobs, 3);
    int full_page = 10;

    /* C12: the old "weight >= 1.1 means trusted" test counted Wikipedia,
       StackExchange and OpenAlex as anchors, so one fast API could end a
       general query before any web engine answered. A web result is required
       for a general query unless the user picked a single engine. */
    int have_web_result = 0;
    for (int i = 0; i < num_jobs; i++) {
      if (jobs[i].results_count > 0 && jobs[i].engine &&
          (jobs[i].engine->verticals & V_WEB))
        have_web_result = 1;
    }
    /* a single-engine request (engine= or a one-engine allowlist) is the
       user's explicit choice, so it does not need a web result */
    int single_engine_request = (num_jobs <= 1);
    int needs_web = !single_engine_request;

    /* Independent sources, not engine count: Bing and Yahoo share an index, so
       their agreement must not satisfy the "two engines answered" test (F2). */
    const char *answered_groups[16] = {0};
    int group_count = 0;
    for (int i = 0; i < num_jobs; i++) {
      if (jobs[i].results_count <= 0 || !jobs[i].engine)
        continue;
      const char *g = jobs[i].engine->index_group
                          ? jobs[i].engine->index_group
                          : jobs[i].engine->id;
      int seen = 0;
      for (int k = 0; k < group_count; k++)
        if (strcmp(answered_groups[k], g) == 0) {
          seen = 1;
          break;
        }
      if (!seen && group_count < 16)
        answered_groups[group_count++] = g;
    }

    int web_ok = have_web_result || !needs_web;

    if (!legacy && num_jobs > 1 && web_ok && trusted && group_count >= 2 &&
        projected >= full_page && elapsed_ms >= 500)
      break;
    if (!legacy && num_jobs > 1 && web_ok && trusted && engines_so_far >= 1 &&
        projected >= 6 && elapsed_ms >= 550)
      break;
    if (!legacy && num_jobs > 1 && web_ok && group_count >= 2 &&
        projected >= full_page && elapsed_ms >= 1200)
      break;

    /* Not enough to fill a page: keep waiting, but not forever. This is the
       soft cap that used to be a hard 2s cut, which is what left pages thin. */
    int thin = projected < full_page;
    long cap = (thin || patient) ? soft_cap_ms : batch_cap_ms;
    if (elapsed_ms >= cap)
      break;
  }

  handle_responses(multi_handle, jobs, num_jobs);
  cleanup_unfinished_jobs(multi_handle, jobs, num_jobs);
  curl_multi_cleanup(multi_handle);

  if (retries < max_proxy_retries && should_retry(jobs, num_jobs)) {
    retries++;
    goto retry;
  }

  /* one second chance for a single-engine request; a full batch retry only
     doubles the page latency and hits the same block */
  if (!empty_retry_done && num_jobs == 1 && all_jobs_empty(jobs, num_jobs)) {
    empty_retry_done = 1;
    goto retry;
  }

  return 0;
}

int scrape_engine(const SearchEngine *engine, const char *query,
                  SearchResult **out_results, int max_results) {
  ScrapeJob job = {.engine = engine,
                   .query = (char *)query,
                   .out_results = out_results,
                   .max_results = max_results,
                   .results_count = 0,
                   .page = 1,
                   .http_status = 0,
                   .status = SCRAPE_STATUS_PENDING};

  scrape_engines_parallel(&job, 1);
  return job.results_count;
}
