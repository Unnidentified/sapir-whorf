#include "Utility.h"
#include <beaker.h>
#include <stdlib.h>
#include <string.h>

int hex_to_int(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

char *get_theme(const char *default_theme) {
  char *cookie = get_cookie("theme");
  if (cookie &&
      (strcmp(cookie, "light") == 0 ||
       strcmp(cookie, "dark") == 0)) {
    return cookie;
  }
  free(cookie);
  return strdup(default_theme);
}

char *get_locale(const char *default_locale) {
  char *cookie = get_cookie("locale");
  if (cookie && beaker_get_locale_meta(cookie) != NULL) {
    return cookie;
  }
  free(cookie);
  return strdup(default_locale);
}
