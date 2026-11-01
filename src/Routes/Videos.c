/* K4: /videos - one grid of video cards.
 *
 * Deliberately no player embed: the card links out to the platform's watch
 * page. An <iframe> would make the reader's browser contact the platform, and
 * the whole design keeps third-party contact on the server, behind the
 * provider switches.
 *
 * Page 1 only for now; the pager arrives with the second provider. */
#include "Videos.h"
#include "../Providers/Providers.h"
#include "../Scraping/VideoScraping.h"
#include "ImageProxy.h"
#include "../Utility/Utility.h"
#include "Config.h"
#include <beaker.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 3725 -> "1:02:05", 95 -> "1:35". A duration the provider omitted stays empty
   rather than showing 0:00, which reads as a real length. */
static void format_duration(int seconds, char *out, size_t cap) {
  if (seconds <= 0) {
    out[0] = '\0';
    return;
  }
  int h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
  if (h > 0)
    snprintf(out, cap, "%d:%02d:%02d", h, m, s);
  else
    snprintf(out, cap, "%d:%02d", m, s);
}

int videos_handler(UrlParams *params) {
  TemplateContext ctx = new_context();
  char *raw_query = "";

  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "q") == 0)
        raw_query = params->params[i].value;
    }
  }

  context_set(&ctx, "query", raw_query);
  context_set(&ctx, "chrome_action", "/videos");
  context_set(&ctx, "chrome_active", "videos");
  {
    char enc[BUFFER_SIZE_MEDIUM];
    url_encode(raw_query ? raw_query : "", enc, sizeof(enc));
    context_set(&ctx, "query_encoded", enc);
  }
  context_set(&ctx, "chrome_engine", "");
  context_set(&ctx, "chrome_all_href", "/");

  char *theme = get_theme("");
  context_set(&ctx, "theme", theme);
  context_set(&ctx, "title_font", get_title_font());
  context_set(&ctx, "page_scale_css", get_page_scale_css());
  CustomTheme custom_theme;
  get_custom_theme(&custom_theme);
  context_set_custom_palette(&ctx, &custom_theme);
  free(theme);

  char *locale = get_locale(NULL);
  beaker_set_locale(&ctx, locale);

  VideoResult *results = NULL;
  int count = 0;
  int status = scrape_videos(raw_query, &results, &count);

  StringMatrix videos;
  string_matrix_init(&videos);
  for (int i = 0; i < count; i++) {
    /* the thumbnail goes through the image proxy: the browser must never talk
       to the platform, and the proxy keeps the reader's address private */
    char *thumb = image_proxy_sign_url(results[i].thumb_url);
    char duration[16];
    format_duration(results[i].duration_s, duration, sizeof(duration));
    const char *values[VIDEO_RESULT_FIELDS] = {
        thumb ? thumb : "", results[i].title, results[i].watch_url, duration,
        results[i].channel, results[i].platform};
    string_matrix_append(&videos, values, VIDEO_RESULT_FIELDS);
    free(thumb);
  }

  context_set_array_of_arrays(&ctx, "videos", videos.rows, videos.count,
                              videos.count > 0 ? videos.field_counts : NULL);
  context_set(&ctx, "videos_state",
              status != 0 ? "no_providers" : (videos.count > 0 ? "ok" : "empty"));

  char *html = render_template("videos.html", &ctx);
  if (html) {
    send_response(html);
    free(html);
  } else {
    send_status("500 Internal Server Error");
    send_response("<h1>Template rendering failed</h1>");
  }

  string_matrix_free(&videos);
  free_video_results(results, count);
  free(locale);
  free_context(&ctx);
  return 0;
}
