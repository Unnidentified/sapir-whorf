#ifndef CONFIG_H
#define CONFIG_H

/* Bind to loopback by default. The one scarce resource this app spends is the
   host's egress reputation: one noisy LAN client gets every engine blocked and
   then breaker-banned for hours. LAN exposure is now an explicit choice. */
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 5000
#define DEFAULT_CACHE_DIR "/tmp/omnisearch_cache"
#define DEFAULT_CACHE_TTL_SEARCH 3600
#define DEFAULT_CACHE_TTL_INFOBOX 86400
#define DEFAULT_CACHE_TTL_IMAGE 604800
#define DEFAULT_MAX_PROXY_RETRIES 3
/* A modest inbound limit by default. One runaway client (a reload loop, a
   scraper on the LAN) can otherwise get every upstream engine to block this
   host, and the breakers then persist that for hours. */
#define DEFAULT_RATE_LIMIT_SEARCH_REQUESTS 120
#define DEFAULT_RATE_LIMIT_SEARCH_INTERVAL 60
#define DEFAULT_RATE_LIMIT_IMAGES_REQUESTS 60
#define DEFAULT_RATE_LIMIT_IMAGES_INTERVAL 60

#define DEFAULT_BREAKER_THRESHOLD 5
#define DEFAULT_ANSWERS_MODE "local"
/* J3c: results belong under the answer, as they do on Google. `never` is the
   previous behaviour (a confident local answer skipped the fan-out), `auto`
   skips only for a very confident answer and a query with no words in it. */
#define DEFAULT_ANSWERS_FANOUT "always"
#define DEFAULT_PACE_PER_MINUTE 40
#define DEFAULT_PACE_BURST 6
#define DEFAULT_BREAKER_COOLDOWN 900
#define DEFAULT_YACY_INSTANCE "http://127.0.0.1:8090"

#define BUFFER_SIZE_SMALL 256
#define BUFFER_SIZE_MEDIUM 512
#define BUFFER_SIZE_LARGE 1024
#define BUFFER_SIZE_XLARGE 2048

#define INITIAL_BUFFER_SIZE 16384

#define WIKI_SUMMARY_MAX_CHARS 300

#define MD5_HASH_LEN 32
#define HEX_CHARS "0123456789abcdef"

#define INFOBOX_FIELD_COUNT 5
#define MAX_RESULTS_PER_ENGINE 10

#define CURL_TIMEOUT_SECS 15L
/* J3b: how long the page waits for one answer panel before rendering
   without it. The panel is a bonus; the results are the page. */
#define INFOBOX_BUDGET_MS 700L
#define CURL_DNS_TIMEOUT_SECS 300L

#define BING_IMAGE_URL "https://www.bing.com/images/search"
#define IMAGE_RESULTS_PER_PAGE 32
#define IMAGE_RESULT_FIELDS 4

typedef struct {
  char host[256];
  int port;
  char domain[256];
  char default_locale[32];
  char proxy[256];
  char proxy_list_file[256];
  int max_proxy_retries;
  int randomize_username;
  int randomize_password;
  char cache_dir[512];
  int cache_ttl_search;
  int cache_ttl_infobox;
  int cache_ttl_image;
  char engines[512];
  char yacy_instance[512];
  char google_cse_key[256];
  char google_cse_cx[128];
  /* G3: the element endpoint needs a token the widget JS mints at runtime.
     Supplying one is an explicit opt-in, and the engine stays off without it. */
  char google_cse_tok[512];
  char searxng_url[256];
  /* J5.6: the owner's list is 20+ names; 128 bytes truncated it
     silently. */
  char searxng_engines[1024];
  char trusted_proxies[512];
  /* "all"    - every infobox handler, including Wikipedia, dictionary and
                currency lookups (these leave the machine)
     "local"  - calculator, units and colour only; nothing leaves the box
     "off"    - no infobox at all */
  char infobox_mode[16];
  /* H4: all | local | off - what the answer box is allowed to do */
  char answers_mode[16];
  char answers_fanout[16];
  int rate_limit_search_requests;
  int rate_limit_search_interval;
  int rate_limit_images_requests;
  int rate_limit_images_interval;
  int breaker_threshold;
  /* G2: per-engine outbound pacing - the opposite of a cooldown: it stops the
     burst that earns the challenge in the first place. */
  int pace_per_minute;
  int pace_burst;
  int breaker_cooldown;
} Config;

void config_set_defaults(Config *config);
int load_config(const char *filename, Config *config);

#endif
