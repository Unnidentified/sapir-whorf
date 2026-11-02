#include "CseId.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* an id is the letters, digits, underscore, colon and hyphen that follow cx=,
   or the whole input when it is already just an id */
static int id_char(int c) {
  return isalnum(c) || c == '_' || c == ':' || c == '-';
}

int cse_extract_id(const char *input, char *out, size_t cap) {
  if (!out || cap == 0)
    return -1;
  out[0] = '\0';
  if (!input)
    return -1;

  /* strip leading whitespace */
  const char *p = input;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  if (!*p)
    return -1;

  /* the cx= form, wherever it appears: a URL query, an embed src, or a pasted
     fragment */
  for (const char *scan = p; (scan = strstr(scan, "cx=")); scan += 3) {
    const char *v = scan + 3;
    if (!*v)
      continue;
    size_t n = 0;
    while (v[n] && id_char((unsigned char)v[n]))
      n++;
    if (n >= 6 && n < cap) {
      memcpy(out, v, n);
      out[n] = '\0';
      return 0;
    }
  }

  /* otherwise the input must itself be an id: nothing but id characters */
  size_t n = strcspn(p, " \t\r\n");
  if (n < 6 || n >= cap)
    return -1;
  for (size_t i = 0; i < n; i++)
    if (!id_char((unsigned char)p[i]))
      return -1;
  memcpy(out, p, n);
  out[n] = '\0';
  return 0;
}
