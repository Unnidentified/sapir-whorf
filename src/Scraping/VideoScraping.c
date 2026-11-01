#include "VideoScraping.h"
#include "../Providers/Providers.h"
#include "../Utility/Display.h"
#include "../Utility/HttpClient.h"
#include "../Utility/Utility.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_USER_AGENT "omnisearch/1.0 (+https://codeberg.org/gabrielf/omnisearch)"
#define VIDEO_LIMIT 24

static void copy_field(char *dst, size_t cap, const char *src) {
  if (!dst || cap == 0)
    return;
  if (!src)
    src = "";
  snprintf(dst, cap, "%s", src);
}

/* Dailymotion's public search API: no key, and the fields we ask for are the
   ones the card draws. Measured 2026-11-01 (docs/probe-log.tsv). */
static int fetch_dailymotion(const char *query, VideoResult *out, int room) {
  char encoded[1024];
  url_encode(query ? query : "", encoded, sizeof(encoded));
  char url[2048];
  snprintf(url, sizeof(url),
           "https://api.dailymotion.com/videos?search=%s&limit=%d&fields="
           "title,url,thumbnail_480_url,duration,created_time,"
           "owner.screenname",
           encoded, VIDEO_LIMIT);

  HttpResponse response = http_get(url, VIDEO_USER_AGENT);
  if (!response.memory || response.size == 0) {
    free(response.memory);
    return 0;
  }

  json_error_t error;
  json_t *root = json_loads(response.memory, 0, &error);
  free(response.memory);
  if (!root)
    return 0;

  json_t *list = json_object_get(root, "list");
  int added = 0;
  if (json_is_array(list)) {
    size_t index;
    json_t *item;
    json_array_foreach(list, index, item) {
      if (added >= room)
        break;
      VideoResult *v = &out[added];
      memset(v, 0, sizeof(*v));
      copy_field(v->title, sizeof(v->title),
                 json_string_value(json_object_get(item, "title")));
      copy_field(v->watch_url, sizeof(v->watch_url),
                 json_string_value(json_object_get(item, "url")));
      copy_field(v->thumb_url, sizeof(v->thumb_url),
                 json_string_value(json_object_get(item, "thumbnail_480_url")));
      copy_field(v->channel, sizeof(v->channel),
                 json_string_value(json_object_get(item, "owner.screenname")));
      copy_field(v->platform, sizeof(v->platform), "Dailymotion");
      json_t *duration = json_object_get(item, "duration");
      if (json_is_integer(duration))
        v->duration_s = (int)json_integer_value(duration);
      /* a card with no title or no watch page is not a result */
      if (!v->title[0] || !v->watch_url[0])
        continue;
      added++;
    }
  }
  json_decref(root);
  return added;
}

int scrape_videos(const char *query, VideoResult **out, int *count) {
  *out = NULL;
  *count = 0;
  if (!query || !query[0])
    return 1;

  int asked = 0;
  VideoResult *results = calloc(VIDEO_LIMIT, sizeof(VideoResult));
  if (!results)
    return 1;

  if (!provider_is_off("dailymotion")) {
    asked = 1;
    fetch_dailymotion(query, results, VIDEO_LIMIT);
  }

  if (!asked) {
    free(results);
    return 1;
  }
  *out = results;
  /* count the filled prefix: the fetch returns how many it added, so scan */
  int n = 0;
  while (n < VIDEO_LIMIT && results[n].title[0])
    n++;
  *count = n;
  return 0;
}

void free_video_results(VideoResult *results, int count) {
  (void)count;
  free(results);
}
