/* J4.0: /w/<kind> - the card on its own.

   The card already renders inside /search, and with JavaScript off that is all
   it needs to be. This endpoint exists for two reasons the review asked for: a
   widget is a thing with a URL rather than an accident of one page's context,
   and the enhancement in static/widgets.js can refresh one card without
   re-rendering the results it sits on.

   Every provider a widget can reach is a constant in C (see PLACES_ENDPOINT,
   STOCK_ENDPOINT, TRANSLATE_ENDPOINT). The request cannot name a host, a URL or
   a template: `kind` selects one of four functions and `q` is only ever passed
   as data to them. That is the SSRF guard, and it is structural rather than a
   filter. The inbound limiter applies exactly as it does to /search. */

#include "../Infobox/AnswerBox.h"
#include "../Limiter/RateLimit.h"
#include "../Utility/Utility.h"
#include "Config.h"

/* declared locally, the way Settings.c does: Config.c owns the definition and
   the header does not export it */
extern Config global_config;
#include <stdio.h>
#include <string.h>

static int widget_fragment(const char *kind, UrlParams *params) {
  if (!kind || !params)
    return 0;

  /* the limiter, same budget as the search path: a card fetch is a page fetch
     as far as this server is concerned */
  RateLimitConfig limiter = {
      .max_requests = global_config.rate_limit_search_requests,
      .interval_seconds = global_config.rate_limit_search_interval,
  };
  RateLimitResult limit = rate_limit_check("widgets", &limiter);
  if (limit.limited) {
    send_status("429 Too Many Requests");
    send_response("<p>Too many requests</p>");
    return 0;
  }

  const char *query = "";
  const char *key = "";
  for (int i = 0; i < params->count; i++) {
    if (strcmp(params->params[i].key, "q") == 0)
      query = params->params[i].value;
    else if (strcmp(params->params[i].key, "key") == 0)
      key = params->params[i].value;
  }

  /* get_locale() returns a heap string that may be NULL, and the fallback is
     the configured default - which is what makes {{l("...")}} render a string
     instead of its key on a request from a fresh client. It is copied rather
     than kept: the allocated one is freed immediately, and a pointer into it
     used after that was a use-after-free that rendered template keys. */
  char locale_id[32] = "";
  char *cookie_locale = get_locale(NULL);
  if (cookie_locale && cookie_locale[0])
    snprintf(locale_id, sizeof(locale_id), "%s", cookie_locale);
  else
    snprintf(locale_id, sizeof(locale_id), "%s", global_config.default_locale);
  free(cookie_locale);

  char lang[8] = "en";
  if (locale_id[0])
    snprintf(lang, sizeof(lang), "%.2s", locale_id);

  /* the keypad sends one character per request; that is the whole no-JS
     protocol and the fragment has to honour it exactly as the page does */
  char work[320];
  snprintf(work, sizeof(work), "%s", query);
  if (key[0] && strcmp(kind, "calculator") == 0) {
    size_t used = strlen(work);
    if (used + strlen(key) + 1 < sizeof(work))
      snprintf(work + used, sizeof(work) - used, "%s", key);
  }

  WidgetData widget;
  memset(&widget, 0, sizeof(widget));
  int resolved = answer_box_widget_forced(kind, work, lang, &widget);

  if (!resolved) {
    send_status("204 No Content");
    send_response("");
    return 0;
  }

  char *html = NULL;
  switch (widget.kind[0] ? 1 : 0) {
  case 1: {
    char template[64];
    snprintf(template, sizeof(template), "widget_%s.html", widget.kind);
    TemplateContext ctx = new_context();
    /* a fragment is a page as far as the template is concerned: without this
       {{l("...")}} renders the key instead of the string */
    if (locale_id[0])
      beaker_set_locale(&ctx, locale_id);
    context_set(&ctx, "widget_kind", widget.kind);
    context_set(&ctx, "widget_result",
                widget.ok && widget.result[0] ? widget.result : widget.note);
    context_set(&ctx, "widget_expression", widget.expression);
    context_set(&ctx, "widget_note", widget.note);
    context_set(&ctx, "widget_href", widget.href);
    context_set(&ctx, "widget_ok", widget.ok ? "1" : "");
    /* the keypad appends to this, and the form carries it back */
    /* the card must show what will be submitted next, which is the expression
       including the key that was just pressed */
    context_set(&ctx, "query", widget.expression);
    context_set(&ctx, "query_encoded", widget.expression);
    html = render_template(template, &ctx);
    free_context(&ctx);
    break;
  }
  default:
    break;
  }

  if (!html) {
    send_status("204 No Content");
    send_response("");
    return 0;
  }
  send_response(html);
  free(html);
  return 0;
}

static int widget_calculator_handler(UrlParams *params) {
  return widget_fragment("calculator", params);
}
static int widget_translate_handler(UrlParams *params) {
  return widget_fragment("translate", params);
}
static int widget_place_handler(UrlParams *params) {
  return widget_fragment("place", params);
}
static int widget_stock_handler(UrlParams *params) {
  return widget_fragment("stock", params);
}

void register_widget_routes(void) {
  set_handler("/w/calculator", widget_calculator_handler);
  set_handler("/w/translate", widget_translate_handler);
  set_handler("/w/place", widget_place_handler);
  set_handler("/w/stock", widget_stock_handler);
}
