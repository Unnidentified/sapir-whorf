#include "Images.h"
#include "../Cache/Cache.h"
#include "../Limiter/RateLimit.h"
#include "../Scraping/ImageScraping.h"
#include "../Utility/Unescape.h"
#include "../Utility/Utility.h"
#include "Config.h"
#include <beaker.h>

static char *build_images_request_cache_key(const char *query, int page,
                                            const char *client_key) {
  char scope_key[BUFFER_SIZE_MEDIUM];
  snprintf(scope_key, sizeof(scope_key), "images_request:%s",
           client_key ? client_key : "unknown");
  return cache_compute_key(query, page, scope_key);
}

static char *build_images_href(const char *query, int page) {
  const char *safe_query = query ? query : "";
  size_t needed = strlen("/images?q=") + strlen(safe_query) + 1;
  if (page > 1)
    needed += strlen("&p=") + 16;

  char *href = (char *)malloc(needed);
  if (!href)
    return NULL;

  snprintf(href, needed, "/images?q=%s", safe_query);
  if (page > 1) {
    char page_buf[16];
    snprintf(page_buf, sizeof(page_buf), "%d", page);
    strcat(href, "&p=");
    strcat(href, page_buf);
  }
  return href;
}

static char *images_href_builder(int page, void *data) {
  return build_images_href((const char *)data, page);
}

int images_handler(UrlParams *params) {
  extern Config global_config;
  TemplateContext ctx = new_context();
  char *raw_query = "";
  int page = 1;

  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "q") == 0) {
        raw_query = params->params[i].value;
      } else if (strcmp(params->params[i].key, "p") == 0) {
        int parsed = atoi(params->params[i].value);
        if (parsed > 1)
          page = parsed;
      }
    }
  }

  context_set(&ctx, "query", raw_query);

  char *theme = get_theme("");
  context_set(&ctx, "theme", theme);
  free(theme);

  char *locale = get_locale(NULL);
  beaker_set_locale(&ctx, locale);

  const char *rate_limit_msg = beaker_get_locale_value(locale, "rate_limit");
  if (!rate_limit_msg) rate_limit_msg = "Slow down! Too many image searches from you!";
  const char *error_images_msg = beaker_get_locale_value(locale, "error_images");
  if (!error_images_msg) error_images_msg = "Error fetching images";

  char ***pager_matrix = NULL;
  int *pager_inner_counts = NULL;
  int pager_count = build_pagination(page, images_href_builder,
                                     (void *)raw_query, &pager_matrix,
                                     &pager_inner_counts);
  if (pager_count > 0) {
    context_set_array_of_arrays(&ctx, "pagination_links", pager_matrix,
                                pager_count, pager_inner_counts);
  }

  char *display_query = url_decode_query(raw_query);
  context_set(&ctx, "query", display_query);

  if (!raw_query || strlen(raw_query) == 0) {
    send_redirect("/");
    if (display_query)
      free(display_query);
    free_context(&ctx);
    return -1;
  }

  char client_key[BUFFER_SIZE_SMALL];
  rate_limit_get_client_key(client_key, sizeof(client_key));

  char *request_cache_key =
      build_images_request_cache_key(raw_query, page, client_key);
  int request_is_cached = 0;

  if (request_cache_key && get_cache_ttl_image() > 0) {
    char *cached_marker = NULL;
    size_t cached_marker_size = 0;

    if (cache_get(request_cache_key, (time_t)get_cache_ttl_image(),
                  &cached_marker, &cached_marker_size) == 0) {
      request_is_cached = 1;
    }

    free(cached_marker);
  }

  if (!request_is_cached) {
    RateLimitConfig rate_limit_config = {
        .max_requests = global_config.rate_limit_images_requests,
        .interval_seconds = global_config.rate_limit_images_interval,
    };
    RateLimitResult rate_limit_result =
        rate_limit_check("images", &rate_limit_config);
    if (rate_limit_result.limited) {
      char response[256];
      snprintf(response, sizeof(response), "<h1>%s</h1>", rate_limit_msg);
      send_response(response);
      free(request_cache_key);
      free(display_query);
      free_context(&ctx);
      return -1;
    }

    if (request_cache_key && get_cache_ttl_image() > 0) {
      cache_set(request_cache_key, "1", 1);
    }
  }

  ImageResult *results = NULL;
  int result_count = 0;

  if (scrape_images(raw_query, page, &results, &result_count) != 0 ||
      !results) {
    char error_html[128];
    snprintf(error_html, sizeof(error_html), "<h1>%s</h1>", error_images_msg);
    send_response(error_html);
    free(request_cache_key);
    free(display_query);
    free_context(&ctx);
    return -1;
  }

  char ***image_matrix = malloc(sizeof(char **) * result_count);
  int *inner_counts = malloc(sizeof(int) * result_count);

  if (!image_matrix || !inner_counts) {
    if (image_matrix)
      free(image_matrix);
    if (inner_counts)
      free(inner_counts);
    free_image_results(results, result_count);
    free(request_cache_key);
    free(display_query);
    free_context(&ctx);
    return -1;
  }

  for (int i = 0; i < result_count; i++) {
    image_matrix[i] = malloc(sizeof(char *) * IMAGE_RESULT_FIELDS);
    image_matrix[i][0] = strdup(results[i].thumbnail_url);
    image_matrix[i][1] = strdup(results[i].title);
    image_matrix[i][2] = strdup(results[i].page_url);
    image_matrix[i][3] = strdup(results[i].full_url);
    inner_counts[i] = IMAGE_RESULT_FIELDS;
  }

  context_set_array_of_arrays(&ctx, "images", image_matrix, result_count,
                              inner_counts);

  char *rendered = render_template("images.html", &ctx);
  if (rendered) {
    send_response(rendered);
    free(rendered);
  } else {
    send_response("<h1>Error rendering image results</h1>");
  }

  for (int i = 0; i < result_count; i++) {
    for (int j = 0; j < IMAGE_RESULT_FIELDS; j++)
      free(image_matrix[i][j]);
    free(image_matrix[i]);
  }
  free(image_matrix);
  free(inner_counts);

  if (pager_count > 0) {
    for (int i = 0; i < pager_count; i++) {
      for (int j = 0; j < LINK_FIELD_COUNT; j++)
        free(pager_matrix[i][j]);
      free(pager_matrix[i]);
    }
    free(pager_matrix);
    free(pager_inner_counts);
  }

  free_image_results(results, result_count);
  free(request_cache_key);
  free(display_query);
  free_context(&ctx);

  return 0;
}
