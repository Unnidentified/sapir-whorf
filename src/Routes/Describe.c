#include "Describe.h"
#include "../Infobox/Descriptions.h"
#include "../Providers/Providers.h"
#include <stdlib.h>
#include <string.h>

int describe_handler(UrlParams *params) {
  const char *encoded = "";
  const char *signature = "";
  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "u") == 0)
        encoded = params->params[i].value;
      else if (strcmp(params->params[i].key, "s") == 0)
        signature = params->params[i].value;
    }
  }
  /* the provider switch turns the fetches off everywhere, including here */
  if (provider_is_off("descriptions") || !encoded[0] || !signature[0]) {
    send_status("204 No Content");
    send_response("");
    return 0;
  }

  char text[512];
  if (!description_from_token(encoded, signature, text, sizeof(text))) {
    send_status("204 No Content");
    send_response("");
    return 0;
  }
  serve_data(text, strlen(text), "text/plain; charset=utf-8");
  return 0;
}
