#ifndef OMNISEARCH_INFobox_CALCKEYS_H
#define OMNISEARCH_INFobox_CALCKEYS_H

#include <stddef.h>

/* K9: the keypad protocol lives in one place.
 *
 * It was implemented twice - once in Search.c for the page and once in
 * Widget.c for the fragment endpoint - and the second copy appended the raw
 * key: "2+27" plus AC became the expression "2+27AC", which the parser read as
 * an error. A key the evaluator cannot parse is worse than a missing key, so
 * the allowed set, AC, backspace and "=" are applied by one function.
 *
 * Applies one key press to `expression` and writes the result to `out`.
 * Unknown keys and "=" leave the expression unchanged (they evaluate it).
 * Returns 1 when the expression changed, 0 when it did not. */
int calc_apply_key(const char *expression, const char *key, char *out,
                   size_t size);

/* 1 when `key` is one the keypad offers. */
int calc_key_known(const char *key);

#endif
