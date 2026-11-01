#include "Secrets.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The file this module owns. It is small on purpose: one flat set of keys, no
   nesting, so a reader can audit it in a minute and a hand-edit cannot inject
   a config option that means something else. */
typedef struct {
  SecretKey key;
  const char *name;      /* what the file calls it */
  const char *section;   /* for the initial write, to keep related keys together */
  int credential;        /* never rendered, never logged */
  const char *env;       /* environment override, or NULL */
} SecretSpec;

static const SecretSpec SPECS[SK_COUNT] = {
    {SK_REDDIT_CLIENT_ID, "reddit_client_id", "reddit", 0, "OMNI_REDDIT_CLIENT_ID"},
    {SK_REDDIT_CLIENT_SECRET, "reddit_client_secret", "reddit", 1,
     "OMNI_REDDIT_CLIENT_SECRET"},
    {SK_REDDIT_USERNAME, "reddit_username", "reddit", 0, "OMNI_REDDIT_USERNAME"},
    {SK_REDDIT_DEVICE_ID, "reddit_device_id", "reddit", 1, NULL},
    {SK_REDDIT_REDIRECT_URI, "reddit_redirect_uri", "reddit", 0,
     "OMNI_REDDIT_REDIRECT_URI"},
    {SK_REDDIT_REFRESH_TOKEN, "reddit_refresh_token", "reddit", 1,
     "OMNI_REDDIT_REFRESH_TOKEN"},
    {SK_SEARXNG_URL, "searxng_url", "bridges", 0, "OMNI_SEARXNG_URL"},
    {SK_SEARXNG_ENGINES, "searxng_engines", "bridges", 0, "OMNI_SEARXNG_ENGINES"},
    {SK_YACY_INSTANCE, "yacy_instance", "bridges", 0, "OMNI_YACY_INSTANCE"},
    {SK_GOOGLE_CSE_CX, "google_cse_cx", "bridges", 0, "OMNI_GOOGLE_CSE_CX"},
    {SK_GOOGLE_CSE_KEY, "google_cse_key", "bridges", 1, "OMNI_GOOGLE_CSE_KEY"},
    {SK_ADMIN_TOKEN, "admin_token", "server", 1, "OMNI_ADMIN_TOKEN"},
    {SK_EXTRA_ENGINES, "extra_engines", "engines", 0, "OMNI_EXTRA_ENGINES"},
};

typedef struct {
  char value[1024];
  int present;
} SecretValue;

static SecretValue g_file[SK_COUNT];
static char g_path[1024];
static int g_loaded;
static int g_unsafe;      /* the file exists but its mode is too open */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

const char *secrets_key_name(SecretKey key) {
  if (key < 0 || key >= SK_COUNT)
    return "";
  return SPECS[key].name;
}

SecretKey secrets_key_from_name(const char *name) {
  if (!name)
    return SK_COUNT;
  for (int i = 0; i < SK_COUNT; i++)
    if (strcmp(SPECS[i].name, name) == 0)
      return (SecretKey)i;
  return SK_COUNT;
}

int secrets_is_credential(SecretKey key) {
  if (key < 0 || key >= SK_COUNT)
    return 1;
  return SPECS[key].credential;
}

const char *secrets_path(void) {
  if (g_path[0])
    return g_path;
  const char *override = getenv("OMNI_SECRETS_FILE");
  if (override && override[0]) {
    snprintf(g_path, sizeof(g_path), "%s", override);
    return g_path;
  }
  const char *home = getenv("HOME");
  if (!home || !home[0])
    home = "/tmp";
  snprintf(g_path, sizeof(g_path), "%s/.omnisearch/secrets.ini", home);
  return g_path;
}

/* the directory, created 0700, with its mode checked as well as its creation */
static int ensure_dir(const char *file_path) {
  char dir[1024];
  snprintf(dir, sizeof(dir), "%s", file_path);
  char *slash = strrchr(dir, '/');
  if (!slash)
    return 0;
  *slash = '\0';
  struct stat st;
  if (stat(dir, &st) != 0) {
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
      fprintf(stderr, "[WARN] secrets: cannot create %s: %s\n", dir,
              strerror(errno));
      return -1;
    }
  } else if ((st.st_mode & 077) != 0) {
    /* the directory holds a credential: close it rather than inherit an
       open one from the umask or a careless copy */
    chmod(dir, 0700);
  }
  return 0;
}

static void parse_file(void) {
  for (int i = 0; i < SK_COUNT; i++) {
    g_file[i].value[0] = '\0';
    g_file[i].present = 0;
  }
  FILE *fh = fopen(secrets_path(), "r");
  if (!fh)
    return;
  char line[1200];
  while (fgets(line, sizeof(line), fh)) {
    char *hash = strchr(line, '#');
    if (hash)
      *hash = '\0';
    char *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *key = line;
    while (*key == ' ' || *key == '\t' || *key == '[')
      key++;
    char *key_end = key + strlen(key);
    while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t' ||
                             key_end[-1] == ']'))
      *--key_end = '\0';
    char *value = eq + 1;
    while (*value == ' ' || *value == '\t')
      value++;
    size_t vlen = strlen(value);
    while (vlen > 0 && (value[vlen - 1] == '\n' || value[vlen - 1] == '\r' ||
                        value[vlen - 1] == ' ' || value[vlen - 1] == '\t'))
      value[--vlen] = '\0';
    if (vlen >= 2 && (value[0] == '"' || value[0] == '\'') &&
        value[vlen - 1] == value[0]) {
      value[vlen - 1] = '\0';
      value++;
    }
    SecretKey sk = secrets_key_from_name(key);
    if (sk == SK_COUNT)
      continue; /* unknown keys are ignored, so a hand-edit cannot inject one */
    snprintf(g_file[sk].value, sizeof(g_file[sk].value), "%s", value);
    g_file[sk].present = value[0] ? 1 : 0;
  }
  fclose(fh);
}

int secrets_load(void) {
  pthread_mutex_lock(&g_lock);
  g_loaded = 1;
  g_unsafe = 0;
  const char *path = secrets_path();
  ensure_dir(path);
  struct stat st;
  if (stat(path, &st) == 0) {
    if ((st.st_mode & 077) != 0) {
      g_unsafe = 1;
      fprintf(stderr,
              "[SECURITY] secrets: %s is mode %04o; it holds credentials and "
              "must be 0600. Refusing to read it. Run: chmod 600 %s\n",
              path, (unsigned)(st.st_mode & 07777), path);
      pthread_mutex_unlock(&g_lock);
      return -1;
    }
    if (st.st_uid != geteuid()) {
      fprintf(stderr,
              "[SECURITY] secrets: %s is owned by uid %d, not by this process "
              "(%d). Refusing to read it.\n",
              path, (int)st.st_uid, (int)geteuid());
      g_unsafe = 1;
      pthread_mutex_unlock(&g_lock);
      return -1;
    }
  }
  parse_file();
  pthread_mutex_unlock(&g_lock);
  return 0;
}

void secrets_reload_after_save(void) {
  pthread_mutex_lock(&g_lock);
  parse_file();
  pthread_mutex_unlock(&g_lock);
}

SecretSource secrets_get(SecretKey key, const char **value) {
  if (value)
    *value = "";
  if (key < 0 || key >= SK_COUNT)
    return SECRET_SOURCE_ABSENT;
  if (!g_loaded)
    secrets_load();
  /* environment first: it is the deployment's own answer and the page must not
     pretend it can override it */
  const char *env_name = SPECS[key].env;
  if (env_name) {
    const char *from_env = getenv(env_name);
    if (from_env && from_env[0]) {
      if (value)
        *value = from_env;
      return SECRET_SOURCE_ENV;
    }
  }
  pthread_mutex_lock(&g_lock);
  int present = g_file[key].present;
  char copy[1024];
  snprintf(copy, sizeof(copy), "%s", g_file[key].value);
  pthread_mutex_unlock(&g_lock);
  if (present && copy[0]) {
    if (value) {
      /* a per-key static so the caller can hold the pointer without copying;
         the file is read once, and a save reloads it */
      static __thread char thread_copy[1024];
      snprintf(thread_copy, sizeof(thread_copy), "%s", copy);
      *value = thread_copy;
    }
    return SECRET_SOURCE_FILE;
  }
  return SECRET_SOURCE_ABSENT;
}

/* write the whole file from the in-memory map, 0600, atomically */
static int write_file(void) {
  const char *path = secrets_path();
  char temp[1200];
  snprintf(temp, sizeof(temp), "%s.tmp", path);

  int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    fprintf(stderr, "[WARN] secrets: cannot write %s: %s\n", temp,
            strerror(errno));
    return -1;
  }
  /* O_CREAT honours the umask, so the mode is set explicitly */
  fchmod(fd, 0600);
  FILE *fh = fdopen(fd, "w");
  if (!fh) {
    close(fd);
    return -1;
  }
  fprintf(fh, "# omnisearch server-side settings\n");
  fprintf(fh, "# Written by /settings. Mode 0600. Never commit this file,\n");
  fprintf(fh, "# never include it in an archive, never paste it anywhere.\n");
  const char *section = "";
  for (int i = 0; i < SK_COUNT; i++) {
    if (!g_file[i].present || !g_file[i].value[0])
      continue;
    if (strcmp(section, SPECS[i].section) != 0) {
      section = SPECS[i].section;
      fprintf(fh, "\n[%s]\n", section);
    }
    fprintf(fh, "%s = %s\n", SPECS[i].name, g_file[i].value);
  }
  if (fflush(fh) != 0 || fsync(fileno(fh)) != 0) {
    fclose(fh);
    unlink(temp);
    return -1;
  }
  fclose(fh);
  if (rename(temp, path) != 0) {
    fprintf(stderr, "[WARN] secrets: cannot replace %s: %s\n", path,
            strerror(errno));
    unlink(temp);
    return -1;
  }
  chmod(path, 0600);
  return 0;
}

int secrets_set(SecretKey key, const char *value) {
  if (key < 0 || key >= SK_COUNT)
    return -1;
  if (!g_loaded)
    secrets_load();
  pthread_mutex_lock(&g_lock);
  if (value && value[0])
    snprintf(g_file[key].value, sizeof(g_file[key].value), "%s", value);
  else
    g_file[key].value[0] = '\0';
  g_file[key].present = (value && value[0]) ? 1 : 0;
  int rc = write_file();
  pthread_mutex_unlock(&g_lock);
  return rc;
}

void secrets_mask(SecretKey key, char *out, size_t cap) {
  if (!out || cap == 0)
    return;
  out[0] = '\0';
  const char *value = "";
  SecretSource src = secrets_get(key, &value);
  if (src == SECRET_SOURCE_ABSENT || !value[0])
    return;
  size_t len = strlen(value);
  if (src == SECRET_SOURCE_ENV) {
    snprintf(out, cap, "%s", "(set by the environment)");
    return;
  }
  if (len <= 4)
    snprintf(out, cap, "%s", "****");
  else
    snprintf(out, cap, "%s%s", "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2",
             value + len - 4);
}

/* 32 random bytes as hex. Reads /dev/urandom; a failure is fatal to the caller
   rather than silently weaker. */
static int random_hex(char *out, size_t hex_len) {
  unsigned char raw[64];
  size_t bytes = hex_len / 2;
  if (bytes > sizeof(raw))
    bytes = sizeof(raw);
  FILE *fh = fopen("/dev/urandom", "rb");
  if (!fh)
    return -1;
  size_t got = fread(raw, 1, bytes, fh);
  fclose(fh);
  if (got != bytes)
    return -1;
  static const char *HEX = "0123456789abcdef";
  for (size_t i = 0; i < bytes; i++) {
    out[i * 2] = HEX[raw[i] >> 4];
    out[i * 2 + 1] = HEX[raw[i] & 15];
  }
  out[bytes * 2] = '\0';
  return 0;
}

const char *secrets_admin_token(void) {
  const char *value = "";
  SecretSource src = secrets_get(SK_ADMIN_TOKEN, &value);
  if (src != SECRET_SOURCE_ABSENT && value[0])
    return value;
  /* first start: generate, store, and say it once. The page needs it for
     writes, so it must exist before the first page is served. */
  char token[80];
  if (random_hex(token, 64) != 0)
    return "";
  secrets_set(SK_ADMIN_TOKEN, token);
  fprintf(stderr,
          "\n"
          "[SECURITY] admin token created. Writes to /settings need it; it is\n"
          "stored in %s (mode 0600) and shown here only once:\n"
          "\n    %s\n"
          "\n"
          "Read it again from that file if you lose it. Do not paste it into\n"
          "chat, an issue, or a screenshot.\n\n",
          secrets_path(), token);
  const char *stored = "";
  return secrets_get(SK_ADMIN_TOKEN, &stored) == SECRET_SOURCE_ABSENT ? ""
                                                                     : stored;
}

int secrets_admin_token_matches(const char *candidate) {
  const char *token = secrets_admin_token();
  if (!token || !token[0] || !candidate || !candidate[0])
    return 0;
  /* constant time: compare the full length whatever the first difference is */
  size_t a = strlen(token), b = strlen(candidate);
  unsigned char diff = (unsigned char)(a == b ? 0 : 1);
  size_t n = a < b ? a : b;
  for (size_t i = 0; i < n; i++)
    diff |= (unsigned char)(token[i] ^ candidate[i]);
  return diff == 0;
}
