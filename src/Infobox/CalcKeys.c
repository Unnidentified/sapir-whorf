#include "CalcKeys.h"

#include <stdio.h>
#include <string.h>

static const char *const KEYS[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "+", "-",
    "*", "/", "(", ")", "^", "\xc3\xb7",              /* ÷ */
    "\xc3\x97",                                        /* × */
    "\xe2\x88\x92",                                    /* − */
    "\xcf\x80",                                        /* π */
    "e", "AC", "ln(", "log(", "sqrt(", "abs(", "=", "%", "Ans", "back"};

int calc_key_known(const char *key) {
  if (!key || !key[0])
    return 0;
  for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++)
    if (strcmp(key, KEYS[i]) == 0)
      return 1;
  return 0;
}

int calc_apply_key(const char *expression, const char *key, char *out,
                   size_t size) {
  if (!out || size == 0)
    return 0;
  snprintf(out, size, "%s", expression ? expression : "");

  if (!key || !key[0] || strcmp(key, "=") == 0)
    return 0;
  if (!calc_key_known(key))
    return 0;

  if (strcmp(key, "AC") == 0) {
    /* a calculator shows 0 after AC; an empty expression would look like a
       render bug and would leave the card with nothing to submit */
    snprintf(out, size, "0");
    return 1;
  }

  if (strcmp(key, "back") == 0) {
    size_t len = strlen(out);
    /* one codepoint, not one byte: ÷ and π are two bytes each and half a
       character would make the parser reject the whole line */
    while (len > 0 && (out[len - 1] & 0xC0) == 0x80)
      len--;
    if (len > 0)
      len--;
    if (len == 0) {
      snprintf(out, size, "0");
      return 1;
    }
    out[len] = '\0';
    return 1;
  }

  size_t used = strlen(out);
  if (used + strlen(key) + 1 >= size)
    return 0;
  snprintf(out + used, size - used, "%s", key);
  return 1;
}
