/* M1.5: a small, bounded parser for an
   application/x-www-form-urlencoded request body.

   Why this exists: beaker does not parse POST bodies into UrlParams, and the
   request PATH is capped at 255 characters (docs/probe-log.tsv, beaker-path-limit).
   A settings form has eight fields plus a 32-byte nonce, so as a GET it goes
   over that cap and the browser gets HTTP ERROR 400 - which is exactly what the
   owner saw. The raw request buffer DOES contain the body, so a POST form works;
   this is the parser for it.

   Bounded on purpose: the body is read from a fixed-size thread-local buffer,
   the parser never allocates, and a body over the cap is refused rather than
   truncated. */
#ifndef FORM_BODY_H
#define FORM_BODY_H

#include <stddef.h>

#define FORM_MAX_FIELDS 24
#define FORM_MAX_BODY 8192

typedef struct {
  char key[64];
  char value[512];
} FormField;

typedef struct {
  FormField fields[FORM_MAX_FIELDS];
  int count;
  int truncated; /* 1 when the body was larger than FORM_MAX_BODY */
} FormBody;

/* Parse the body out of the current request. Returns 0 on success (even when
   the body is empty), -1 when the request is unusable. */
int form_body_parse(FormBody *out);

/* The value for a key, or "" when absent. */
const char *form_get(const FormBody *body, const char *key);

/* Fill a UrlParams-shaped view so the existing handlers, which take UrlParams,
   keep working: keys and values are copied in, query string first, then body
   (so a body field wins over a query field of the same name). */
int form_to_params(const FormBody *body, void *url_params);

#endif
