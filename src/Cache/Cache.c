#include "Cache.h"
#include "Config.h"
#include <dirent.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char cache_dir[BUFFER_SIZE_MEDIUM] = {0};
static int cache_ttl_search_val = DEFAULT_CACHE_TTL_SEARCH;
static int cache_ttl_infobox_val = DEFAULT_CACHE_TTL_INFOBOX;
static int cache_ttl_image_val = DEFAULT_CACHE_TTL_IMAGE;

const char *cache_get_dir(void) {
  return cache_dir;
}

void set_cache_ttl_search(int ttl) { cache_ttl_search_val = ttl; }

void set_cache_ttl_infobox(int ttl) { cache_ttl_infobox_val = ttl; }

void set_cache_ttl_image(int ttl) { cache_ttl_image_val = ttl; }

int get_cache_ttl_search(void) { return cache_ttl_search_val; }

int get_cache_ttl_infobox(void) { return cache_ttl_infobox_val; }

int get_cache_ttl_image(void) { return cache_ttl_image_val; }

static void md5_hash(const char *str, char *output) {
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hash_len;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx)
    return;
  EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
  EVP_DigestUpdate(ctx, str, strlen(str));
  EVP_DigestFinal_ex(ctx, hash, &hash_len);
  EVP_MD_CTX_free(ctx);
  for (unsigned int i = 0; i < hash_len; i++) {
    sprintf(output + (i * 2), "%02x", hash[i]);
  }
}

static time_t get_file_mtime(const char *filepath) {
  struct stat st;
  if (stat(filepath, &st) == 0) {
    return st.st_mtime;
  }
  return 0;
}

int cache_init(const char *dir) {
  if (!dir || strlen(dir) == 0) {
    strncpy(cache_dir, DEFAULT_CACHE_DIR, sizeof(cache_dir) - 1);
    cache_dir[sizeof(cache_dir) - 1] = '\0';
  } else {
    strncpy(cache_dir, dir, sizeof(cache_dir) - 1);
    cache_dir[sizeof(cache_dir) - 1] = '\0';
  }

  struct stat st;
  if (stat(cache_dir, &st) != 0) {
    if (mkdir(cache_dir, 0755) != 0) {
      fprintf(stderr, "[ERROR] Failed to create cache directory: %s\n",
              cache_dir);
      return -1;
    }
  } else if (!S_ISDIR(st.st_mode)) {
    fprintf(stderr, "[ERROR] Cache path exists but is not a directory: %s\n",
            cache_dir);
    return -1;
  }

  for (int i = 0; HEX_CHARS[i]; i++) {
    char subdir_path[BUFFER_SIZE_LARGE];
    snprintf(subdir_path, sizeof(subdir_path), "%s/%c", cache_dir,
             HEX_CHARS[i]);
    if (stat(subdir_path, &st) != 0) {
      mkdir(subdir_path, 0755);
    }
  }

  char cookies_dir[BUFFER_SIZE_LARGE];
  snprintf(cookies_dir, sizeof(cookies_dir), "%s/cookies", cache_dir);
  mkdir(cookies_dir, 0755);

  return 0;
}

void cache_shutdown(void) { cache_dir[0] = '\0'; }

char *cache_compute_key(const char *query, int page, const char *engine_name) {
  char key_buffer[BUFFER_SIZE_LARGE];
  snprintf(key_buffer, sizeof(key_buffer), "%s_%d_%s", query ? query : "", page,
           engine_name ? engine_name : "");

  char *hash = malloc(MD5_HASH_LEN + 1);
  if (!hash) {
    return NULL;
  }
  md5_hash(key_buffer, hash);
  return hash;
}

int cache_get(const char *key, time_t max_age, char **out_data,
              size_t *out_size) {
  if (!key || !out_data || !out_size || cache_dir[0] == '\0') {
    return -1;
  }

  char filepath[BUFFER_SIZE_LARGE];
  snprintf(filepath, sizeof(filepath), "%s/%c/%s.cache", cache_dir, key[0],
           key);

  time_t file_mtime = get_file_mtime(filepath);
  if (file_mtime == 0) {
    return -1;
  }

  time_t now = time(NULL);
  if (max_age > 0 && (now - file_mtime) > max_age) {
    remove(filepath);
    return -1;
  }

  FILE *fp = fopen(filepath, "rb");
  if (!fp) {
    return -1;
  }

  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if (size <= 0) {
    fclose(fp);
    return -1;
  }

  char *data = malloc(size + 1);
  if (!data) {
    fclose(fp);
    return -1;
  }

  size_t read_size = fread(data, 1, size, fp);
  fclose(fp);

  if (read_size != (size_t)size) {
    free(data);
    return -1;
  }

  data[size] = '\0';
  *out_data = data;
  *out_size = size;
  return 0;
}

/* Write through a temp file and rename() into place: a concurrent reader either
   sees the previous body or the new one, never a half-written file (a truncated
   image from the proxy used to be possible here). */
int cache_set(const char *key, const char *data, size_t size) {
  if (!key || !data || size == 0 || cache_dir[0] == '\0') {
    return -1;
  }

  char filepath[BUFFER_SIZE_LARGE];
  snprintf(filepath, sizeof(filepath), "%s/%c/%s.cache", cache_dir, key[0],
           key);

  char tmp_path[BUFFER_SIZE_XLARGE];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", filepath,
           (long)getpid());

  FILE *fp = fopen(tmp_path, "wb");
  if (!fp) {
    return -1;
  }

  size_t written = fwrite(data, 1, size, fp);
  int fl = fflush(fp);
  fclose(fp);

  if (written != size || fl != 0) {
    remove(tmp_path);
    return -1;
  }

  if (rename(tmp_path, filepath) != 0) {
    remove(tmp_path);
    return -1;
  }

  return 0;
}

/* Cache entries are split by class because their lifetimes differ by two orders
   of magnitude. Sweeping them all at the search TTL (1 h) used to delete the
   stale bodies (7 d), infobox answers (24 h) and images (7 d) on every restart,
   which quietly disabled the stale fallback. The class is encoded in the key
   salt by the callers; here we only need per-class TTLs to sweep with. */
typedef struct {
  const char *suffix; /* matched against the key inside the filename */
  time_t ttl;
} CacheClass;

static void sweep_dir(const char *subdir_path, const CacheClass *classes,
                      int nclasses, int fallback_ttl, time_t now) {
  DIR *dir = opendir(subdir_path);
  if (!dir)
    return;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    /* ".cache" is six characters: this was len-7, so the comparison never
       matched and cache_cleanup() had never deleted a single file. */
    size_t len = strlen(entry->d_name);
    if (len <= 6 || strcmp(entry->d_name + len - 6, ".cache") != 0)
      continue;

    char filepath[BUFFER_SIZE_XLARGE];
    snprintf(filepath, sizeof(filepath), "%s/%s", subdir_path, entry->d_name);

    /* stale_* bodies live 7 days; everything else sweeps at its own class TTL */
    time_t ttl = fallback_ttl;
    for (int c = 0; c < nclasses; c++) {
      if (classes[c].suffix && strstr(entry->d_name, classes[c].suffix)) {
        ttl = classes[c].ttl;
        break;
      }
    }

    struct stat st;
    if (stat(filepath, &st) == 0 && st.st_mtime < now - ttl) {
      remove(filepath);
    }
  }
  closedir(dir);
}

void cache_cleanup(time_t max_age) {
  if (cache_dir[0] == '\0')
    return;

  time_t now = time(NULL);
  /* max_age <= 0 used to mean "do nothing"; a zero search TTL (caching off)
     should still sweep the other classes, so treat it as "search class only". */
  time_t search_ttl = max_age > 0 ? max_age : (time_t)get_cache_ttl_search();

  CacheClass classes[] = {
      {"stale_", (time_t)STALE_CACHE_TTL_SECONDS},
      {"info_", (time_t)get_cache_ttl_infobox()},
      {"img_", (time_t)get_cache_ttl_image()},
  };

  for (int d = 0; HEX_CHARS[d]; d++) {
    char subdir_path[BUFFER_SIZE_LARGE];
    snprintf(subdir_path, sizeof(subdir_path), "%s/%c", cache_dir,
             HEX_CHARS[d]);
    sweep_dir(subdir_path, classes, 3, (int)search_ttl, now);
  }
}
