#include "Unescape.h"
#include "Utility.h"
#include <stdlib.h>
#include <string.h>

char *unescape_search_url(const char *input) {
  if (!input)
    return NULL;

  const char *key = NULL;
  const char *start = NULL;
  const char *end = NULL;
  size_t len = 0;

  if (strstr(input, "uddg=")) {
    key = "uddg=";
    start = strstr(input, key);
    if (!start)
      return NULL;
    start += strlen(key);
    end = strchr(start, '&');
    len = end ? (size_t)(end - start) : strlen(start);
  }

  else if (strstr(input, "RU=")) {
    key = "RU=";
    start = strstr(input, key);
    if (!start)
      return strdup(input);
    start += strlen(key);
    end = strchr(start, '/');
    len = end ? (size_t)(end - start) : strlen(start);
  }

  else {
    return strdup(input);
  }

  char *output = (char *)malloc(len * 3 + 1);
  if (!output)
    return NULL;

  size_t i = 0, j = 0;
  while (i < len) {
    if (start[i] == '%' && i + 2 < len) {
      int high = hex_to_int(start[i + 1]);
      int low = hex_to_int(start[i + 2]);
      if (high != -1 && low != -1) {
        output[j++] = (char)((high << 4) | low);
        i += 3;
      } else {
        output[j++] = start[i++];
      }
    } else if (start[i] == '+') {
      output[j++] = ' ';
      i++;
    } else {
      output[j++] = start[i++];
    }
  }
  output[j] = '\0';

  return output;
}

char *url_decode_query(const char *src) {
  if (!src)
    return NULL;
  char *res = strdup(src);
  char *p = res;
  while (*src) {
    if (*src == '+') {
      *p++ = ' ';
    } else if (*src == '%' && src[1] && src[2]) {
      char hex[3] = {src[1], src[2], '\0'};
      *p++ = (char)strtol(hex, NULL, 16);
      src += 2;
    } else {
      *p++ = *src;
    }
    src++;
  }
  *p = '\0';
  return res;
}

/* J6.2: the panel body reaches the page through {{ … |safe}}, because the local
   generators emit HTML. Anything that is NOT HTML must therefore be escaped
   here, where the code knows which is which - Wikipedia's explaintext extract
   contains & and < as plain text, and marking it safe is how a page breaks or
   a script gets in. */
void html_escape(const char *in, char *out, size_t size) {
  size_t used = 0;
  if (!out || size == 0)
    return;
  out[0] = '\0';
  for (const char *p = in ? in : ""; *p; p++) {
    const char *rep = NULL;
    switch (*p) {
    case '&': rep = "&amp;"; break;
    case '<': rep = "&lt;"; break;
    case '>': rep = "&gt;"; break;
    case '"': rep = "&quot;"; break;
    case '\'': rep = "&#39;"; break;
    default: break;
    }
    if (!rep) {
      if (used + 1 < size)
        out[used++] = *p;
      continue;
    }
    for (const char *r = rep; *r; r++) {
      if (used + 1 < size)
        out[used++] = *r;
    }
    if (used + 1 >= size)
      break;
  }
  out[used] = '\0';
}
