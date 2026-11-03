#include "FormBody.h"
#include <beaker.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

extern __thread char current_request_buffer[];

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* %XX and '+' decoding, in place, never past the destination */
static void url_decode_into(const char *src, size_t len, char *dst, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < len && o + 1 < cap; i++) {
    if (src[i] == '+') {
      dst[o++] = ' ';
    } else if (src[i] == '%' && i + 2 < len &&
               hexval((unsigned char)src[i + 1]) >= 0 &&
               hexval((unsigned char)src[i + 2]) >= 0) {
      dst[o++] = (char)((hexval((unsigned char)src[i + 1]) << 4) |
                        hexval((unsigned char)src[i + 2]));
      i += 2;
    } else {
      dst[o++] = src[i];
    }
  }
  dst[o] = '\0';
}

/* locate the body: after the blank line that ends the headers */
static const char *body_start(size_t *len) {
  *len = 0;
  const char *buf = current_request_buffer;
  if (!buf || !buf[0])
    return NULL;
  const char *p = strstr(buf, "\r\n\r\n");
  const char *sep = "\r\n\r\n";
  if (!p) {
    p = strstr(buf, "\n\n");
    sep = "\n\n";
  }
  if (!p)
    return NULL;
  p += strlen(sep);
  *len = strlen(p);
  return p;
}

int form_body_parse(FormBody *out) {
  if (!out)
    return -1;
  memset(out, 0, sizeof(*out));
  size_t len = 0;
  const char *body = body_start(&len);
  if (!body || len == 0)
    return 0; /* no body is not an error: a plain GET has none */
  if (len > FORM_MAX_BODY) {
    out->truncated = 1;
    return -1;
  }
  const char *p = body;
  const char *end = body + len;
  while (p < end && out->count < FORM_MAX_FIELDS) {
    const char *amp = memchr(p, '&', (size_t)(end - p));
    const char *stop = amp ? amp : end;
    const char *eq = memchr(p, '=', (size_t)(stop - p));
    if (eq) {
      url_decode_into(p, (size_t)(eq - p), out->fields[out->count].key,
                      sizeof(out->fields[out->count].key));
      url_decode_into(eq + 1, (size_t)(stop - eq - 1),
                      out->fields[out->count].value,
                      sizeof(out->fields[out->count].value));
      out->count++;
    }
    if (!amp)
      break;
    p = amp + 1;
  }
  return 0;
}

const char *form_get(const FormBody *body, const char *key) {
  if (!body || !key)
    return "";
  for (int i = 0; i < body->count; i++)
    if (strcmp(body->fields[i].key, key) == 0)
      return body->fields[i].value;
  return "";
}

int form_to_params(const FormBody *body, void *url_params) {
  UrlParams *params = (UrlParams *)url_params;
  if (!body || !params)
    return -1;
  for (int i = 0; i < body->count && params->count < MAX_URL_PARAMS; i++) {
    snprintf(params->params[params->count].key,
             sizeof(params->params[params->count].key), "%s",
             body->fields[i].key);
    snprintf(params->params[params->count].value,
             sizeof(params->params[params->count].value), "%s",
             body->fields[i].value);
    params->count++;
  }
  return 0;
}
