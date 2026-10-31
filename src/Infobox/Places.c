/* J4c: a place card.

   Probed before building (docs/probe-log.tsv): Nominatim answers "Madrid" in
   240 ms with lat/lon and a display name, and its usage policy is explicit -
   1 request per second, an identifying User-Agent, and no bulk geocoding. The
   card is therefore a single lookup for a query that asked for a place, never
   a loop, and the widget refuses to be the thing that makes a search loop.

   It is a place card, not a map: rendering tiles would make the reader's
   browser contact a tile server on our behalf, which is the third-party call
   this project avoids. The coordinates and a link are the honest version. */

#include "AnswerBox.h"
#include "../Utility/HttpClient.h"
#include "../Utility/Utility.h"
#include <jansson.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define PLACES_BUDGET_MS 900
#define PLACES_ENDPOINT                                                   \
  "https://nominatim.openstreetmap.org/search?q=%.400s&format=json&limit=1"

/* The shapes a person types. A bare "madrid" is a web query, so the trigger is
   a phrase that only makes sense as a place lookup. */
static const char *PLACE_PREFIXES[] = {
    "where is ", "where's ", "where are ", "map of ", "show me a map of ",
    "coordinates of ", "coords of ", "location of ", "donde esta ",
    "donde está ", "dónde está ", "dónde esta ", "mapa de ",
    "coordenadas de ", "ubicacion de ", "ubicación de ",
};

int places_parse(const char *query, char *place, size_t place_size) {
  if (!query || !place)
    return 0;
  place[0] = '\0';
  char copy[512];
  snprintf(copy, sizeof(copy), "%s", query);
  char *start = copy;
  while (*start == ' ')
    start++;
  size_t len = strlen(start);
  while (len > 0 && start[len - 1] == ' ')
    start[--len] = '\0';
  for (size_t i = 0; i < sizeof(PLACE_PREFIXES) / sizeof(PLACE_PREFIXES[0]); i++) {
    size_t plen = strlen(PLACE_PREFIXES[i]);
    if (strncasecmp(start, PLACE_PREFIXES[i], plen) != 0)
      continue;
    char *rest = start + plen;
    while (*rest == ' ')
      rest++;
    if (!rest[0])
      return 0;
    snprintf(place, place_size, "%s", rest);
    return 1;
  }
  return 0;
}

int places_widget(const char *place, WidgetData *out) {
  if (!out || !place || !place[0])
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->kind, sizeof(out->kind), "place");
  snprintf(out->title, sizeof(out->title), "Place");
  snprintf(out->expression, sizeof(out->expression), "%s", place);

  char encoded[1024];
  url_encode(place, encoded, sizeof(encoded));
  char url[1200];
  snprintf(url, sizeof(url), PLACES_ENDPOINT, encoded);

  HttpResponse resp = http_get_budget(
      url, "omnisearch/1.0 (place lookup; +https://codeberg.org/gabrielf/omnisearch)",
      PLACES_BUDGET_MS);
  if (!resp.memory || resp.size == 0) {
    http_response_free(&resp);
    snprintf(out->note, sizeof(out->note), "Provider did not answer");
    return 1;
  }
  json_error_t error;
  json_t *root = json_loadb(resp.memory, resp.size, 0, &error);
  http_response_free(&resp);
  if (!root) {
    snprintf(out->note, sizeof(out->note), "Provider sent unreadable JSON");
    return 1;
  }
  if (!json_is_array(root) || json_array_size(root) == 0) {
    snprintf(out->note, sizeof(out->note), "Not found");
    json_decref(root);
    return 1;
  }
  json_t *first = json_array_get(root, 0);
  const char *display = json_string_value(json_object_get(first, "display_name"));
  const char *lat = json_string_value(json_object_get(first, "lat"));
  const char *lon = json_string_value(json_object_get(first, "lon"));
  if (!display || !lat || !lon) {
    snprintf(out->note, sizeof(out->note), "Provider sent no coordinates");
    json_decref(root);
    return 1;
  }
  snprintf(out->result, sizeof(out->result), "%.200s", display);
  out->ok = 1;
  snprintf(out->note, sizeof(out->note), "%.12s, %.12s · OpenStreetMap", lat,
           lon);
  /* the link is built here, with the coordinates, so the template renders a
     URL instead of assembling one and the reader never has to trust a template
     to encode anything */
  snprintf(out->href, sizeof(out->href),
           "https://www.openstreetmap.org/?mlat=%s&mlon=%s#map=12/%s/%s", lat,
           lon, lat, lon);
  json_decref(root);
  return 1;
}
