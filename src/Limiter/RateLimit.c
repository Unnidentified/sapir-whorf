#include "RateLimit.h"
#include <arpa/inet.h>
#include <beaker.h>
#include <ctype.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct RateLimitEntry {
  char client_key[64];
  char scope[32];
  time_t window_start;
  time_t last_seen;
  int count;
  struct RateLimitEntry *next;
} RateLimitEntry;

extern __thread int current_client_socket;
extern __thread char current_request_buffer[];

static pthread_mutex_t rate_limit_mutex = PTHREAD_MUTEX_INITIALIZER;
static RateLimitEntry *rate_limit_entries = NULL;

static int is_blank_char(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *str_case_str(const char *haystack, const char *needle) {
  size_t nlen = strlen(needle);
  for (; *haystack; haystack++) {
    if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
      size_t i;
      for (i = 1; i < nlen; i++) {
        if (tolower((unsigned char)haystack[i]) !=
            tolower((unsigned char)needle[i]))
          break;
      }
      if (i == nlen)
        return haystack;
    }
  }
  return NULL;
}

static void trim_copy(char *dest, size_t dest_size, const char *src,
                      size_t src_len) {
  while (src_len > 0 && is_blank_char(*src)) {
    src++;
    src_len--;
  }

  while (src_len > 0 && is_blank_char(src[src_len - 1])) {
    src_len--;
  }

  if (dest_size == 0)
    return;

  if (src_len >= dest_size)
    src_len = dest_size - 1;

  memcpy(dest, src, src_len);
  dest[src_len] = '\0';
}

static void get_client_key(char *client_key, size_t client_key_size) {
  const char *header = str_case_str(current_request_buffer, "x-forwarded-for:");
  if (!header)
    return;

  header += strlen("X-Forwarded-For:");
  const char *line_end = strpbrk(header, "\r\n");
  size_t line_len = line_end ? (size_t)(line_end - header) : strlen(header);
  const char *comma = memchr(header, ',', line_len);
  size_t value_len = comma ? (size_t)(comma - header) : line_len;

  trim_copy(client_key, client_key_size, header, value_len);
}

static void get_client_key_from_socket(char *client_key,
                                       size_t client_key_size) {
  struct sockaddr_storage addr;
  socklen_t addr_len = sizeof(addr);

  if (getpeername(current_client_socket, (struct sockaddr *)&addr, &addr_len) !=
      0) {
    return;
  }

  if (addr.ss_family == AF_INET) {
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)&addr;
    inet_ntop(AF_INET, &ipv4->sin_addr, client_key, client_key_size);
  } else if (addr.ss_family == AF_INET6) {
    struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&addr;
    inet_ntop(AF_INET6, &ipv6->sin6_addr, client_key, client_key_size);
  } else if (addr.ss_family == AF_UNIX) {
    snprintf(client_key, client_key_size, "unix:%d", current_client_socket);
  }
}

void rate_limit_get_client_key(char *client_key, size_t client_key_size) {
  if (!client_key || client_key_size == 0)
    return;

  client_key[0] = '\0';
  get_client_key(client_key, client_key_size);

  if (client_key[0] == '\0') {
    get_client_key_from_socket(client_key, client_key_size);
  }

  if (client_key[0] == '\0') {
    snprintf(client_key, client_key_size, "nun");
  }
}

static void prune_stale_entries(time_t now) {
  RateLimitEntry **cursor = &rate_limit_entries;

  while (*cursor) {
    RateLimitEntry *entry = *cursor;
    if (now - entry->last_seen > 9999) {
      *cursor = entry->next;
      free(entry);
      continue;
    }
    cursor = &entry->next;
  }
}

static RateLimitEntry *find_entry(const char *client_key, const char *scope) {
  for (RateLimitEntry *entry = rate_limit_entries; entry; entry = entry->next) {
    if (strcmp(entry->client_key, client_key) == 0 &&
        strcmp(entry->scope, scope) == 0) {
      return entry;
    }
  }
  return NULL;
}

static RateLimitEntry *create_entry(const char *client_key, const char *scope,
                                    time_t now) {
  RateLimitEntry *entry = (RateLimitEntry *)calloc(1, sizeof(RateLimitEntry));
  if (!entry)
    return NULL;

  snprintf(entry->client_key, sizeof(entry->client_key), "%s", client_key);
  snprintf(entry->scope, sizeof(entry->scope), "%s", scope);
  entry->window_start = now;
  entry->last_seen = now;
  entry->next = rate_limit_entries;
  rate_limit_entries = entry;
  return entry;
}

RateLimitResult rate_limit_check(const char *scope,
                                 const RateLimitConfig *config) {
  RateLimitResult result = {.limited = 0, .retry_after_seconds = 0};

  if (!scope || !config || config->max_requests <= 0 ||
      config->interval_seconds <= 0) {
    return result;
  }

  char client_key[64];
  time_t now = time(NULL);

  rate_limit_get_client_key(client_key, sizeof(client_key));

  pthread_mutex_lock(&rate_limit_mutex);

  prune_stale_entries(now);

  RateLimitEntry *entry = find_entry(client_key, scope);
  if (!entry) {
    entry = create_entry(client_key, scope, now);
    if (!entry) {
      pthread_mutex_unlock(&rate_limit_mutex);
      return result;
    }
  }

  entry->last_seen = now;

  if (now - entry->window_start >= config->interval_seconds) {
    entry->window_start = now;
    entry->count = 0;
  }

  if (entry->count >= config->max_requests) {
    result.limited = 1;
    result.retry_after_seconds =
        config->interval_seconds - (int)(now - entry->window_start);
    if (result.retry_after_seconds < 1) {
      result.retry_after_seconds = 1;
    }
    pthread_mutex_unlock(&rate_limit_mutex);
    return result;
  }

  entry->count++;
  pthread_mutex_unlock(&rate_limit_mutex);
  return result;
}
