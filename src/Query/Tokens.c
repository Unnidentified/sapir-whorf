#include "Tokens.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Diacritic folding, generated from Unicode NFKD data: every precomposed
   Latin letter in U+00C0-U+024F and U+1E00-U+1EFF whose decomposition is
   plain ASCII. This is what makes "espana" find "España" without touching
   the query the engines are sent. */
typedef struct {
  uint32_t cp;
  const char *ascii;
} FoldEntry;

static const FoldEntry FOLD_TABLE[] = {
  {0x00C0, "a"},
  {0x00C1, "a"},
  {0x00C2, "a"},
  {0x00C3, "a"},
  {0x00C4, "a"},
  {0x00C5, "a"},
  {0x00C7, "c"},
  {0x00C8, "e"},
  {0x00C9, "e"},
  {0x00CA, "e"},
  {0x00CB, "e"},
  {0x00CC, "i"},
  {0x00CD, "i"},
  {0x00CE, "i"},
  {0x00CF, "i"},
  {0x00D1, "n"},
  {0x00D2, "o"},
  {0x00D3, "o"},
  {0x00D4, "o"},
  {0x00D5, "o"},
  {0x00D6, "o"},
  {0x00D9, "u"},
  {0x00DA, "u"},
  {0x00DB, "u"},
  {0x00DC, "u"},
  {0x00DD, "y"},
  {0x00E0, "a"},
  {0x00E1, "a"},
  {0x00E2, "a"},
  {0x00E3, "a"},
  {0x00E4, "a"},
  {0x00E5, "a"},
  {0x00E7, "c"},
  {0x00E8, "e"},
  {0x00E9, "e"},
  {0x00EA, "e"},
  {0x00EB, "e"},
  {0x00EC, "i"},
  {0x00ED, "i"},
  {0x00EE, "i"},
  {0x00EF, "i"},
  {0x00F1, "n"},
  {0x00F2, "o"},
  {0x00F3, "o"},
  {0x00F4, "o"},
  {0x00F5, "o"},
  {0x00F6, "o"},
  {0x00F9, "u"},
  {0x00FA, "u"},
  {0x00FB, "u"},
  {0x00FC, "u"},
  {0x00FD, "y"},
  {0x00FF, "y"},
  {0x0100, "a"},
  {0x0101, "a"},
  {0x0102, "a"},
  {0x0103, "a"},
  {0x0104, "a"},
  {0x0105, "a"},
  {0x0106, "c"},
  {0x0107, "c"},
  {0x0108, "c"},
  {0x0109, "c"},
  {0x010A, "c"},
  {0x010B, "c"},
  {0x010C, "c"},
  {0x010D, "c"},
  {0x010E, "d"},
  {0x010F, "d"},
  {0x0112, "e"},
  {0x0113, "e"},
  {0x0114, "e"},
  {0x0115, "e"},
  {0x0116, "e"},
  {0x0117, "e"},
  {0x0118, "e"},
  {0x0119, "e"},
  {0x011A, "e"},
  {0x011B, "e"},
  {0x011C, "g"},
  {0x011D, "g"},
  {0x011E, "g"},
  {0x011F, "g"},
  {0x0120, "g"},
  {0x0121, "g"},
  {0x0122, "g"},
  {0x0123, "g"},
  {0x0124, "h"},
  {0x0125, "h"},
  {0x0128, "i"},
  {0x0129, "i"},
  {0x012A, "i"},
  {0x012B, "i"},
  {0x012C, "i"},
  {0x012D, "i"},
  {0x012E, "i"},
  {0x012F, "i"},
  {0x0130, "i"},
  {0x0132, "ij"},
  {0x0133, "ij"},
  {0x0134, "j"},
  {0x0135, "j"},
  {0x0136, "k"},
  {0x0137, "k"},
  {0x0139, "l"},
  {0x013A, "l"},
  {0x013B, "l"},
  {0x013C, "l"},
  {0x013D, "l"},
  {0x013E, "l"},
  {0x013F, "l"},
  {0x0140, "l"},
  {0x0143, "n"},
  {0x0144, "n"},
  {0x0145, "n"},
  {0x0146, "n"},
  {0x0147, "n"},
  {0x0148, "n"},
  {0x0149, "n"},
  {0x014C, "o"},
  {0x014D, "o"},
  {0x014E, "o"},
  {0x014F, "o"},
  {0x0150, "o"},
  {0x0151, "o"},
  {0x0154, "r"},
  {0x0155, "r"},
  {0x0156, "r"},
  {0x0157, "r"},
  {0x0158, "r"},
  {0x0159, "r"},
  {0x015A, "s"},
  {0x015B, "s"},
  {0x015C, "s"},
  {0x015D, "s"},
  {0x015E, "s"},
  {0x015F, "s"},
  {0x0160, "s"},
  {0x0161, "s"},
  {0x0162, "t"},
  {0x0163, "t"},
  {0x0164, "t"},
  {0x0165, "t"},
  {0x0168, "u"},
  {0x0169, "u"},
  {0x016A, "u"},
  {0x016B, "u"},
  {0x016C, "u"},
  {0x016D, "u"},
  {0x016E, "u"},
  {0x016F, "u"},
  {0x0170, "u"},
  {0x0171, "u"},
  {0x0172, "u"},
  {0x0173, "u"},
  {0x0174, "w"},
  {0x0175, "w"},
  {0x0176, "y"},
  {0x0177, "y"},
  {0x0178, "y"},
  {0x0179, "z"},
  {0x017A, "z"},
  {0x017B, "z"},
  {0x017C, "z"},
  {0x017D, "z"},
  {0x017E, "z"},
  {0x017F, "s"},
  {0x01A0, "o"},
  {0x01A1, "o"},
  {0x01AF, "u"},
  {0x01B0, "u"},
  {0x01C4, "dz"},
  {0x01C5, "dz"},
  {0x01C6, "dz"},
  {0x01C7, "lj"},
  {0x01C8, "lj"},
  {0x01C9, "lj"},
  {0x01CA, "nj"},
  {0x01CB, "nj"},
  {0x01CC, "nj"},
  {0x01CD, "a"},
  {0x01CE, "a"},
  {0x01CF, "i"},
  {0x01D0, "i"},
  {0x01D1, "o"},
  {0x01D2, "o"},
  {0x01D3, "u"},
  {0x01D4, "u"},
  {0x01D5, "u"},
  {0x01D6, "u"},
  {0x01D7, "u"},
  {0x01D8, "u"},
  {0x01D9, "u"},
  {0x01DA, "u"},
  {0x01DB, "u"},
  {0x01DC, "u"},
  {0x01DE, "a"},
  {0x01DF, "a"},
  {0x01E0, "a"},
  {0x01E1, "a"},
  {0x01E6, "g"},
  {0x01E7, "g"},
  {0x01E8, "k"},
  {0x01E9, "k"},
  {0x01EA, "o"},
  {0x01EB, "o"},
  {0x01EC, "o"},
  {0x01ED, "o"},
  {0x01F0, "j"},
  {0x01F1, "dz"},
  {0x01F2, "dz"},
  {0x01F3, "dz"},
  {0x01F4, "g"},
  {0x01F5, "g"},
  {0x01F8, "n"},
  {0x01F9, "n"},
  {0x01FA, "a"},
  {0x01FB, "a"},
  {0x0200, "a"},
  {0x0201, "a"},
  {0x0202, "a"},
  {0x0203, "a"},
  {0x0204, "e"},
  {0x0205, "e"},
  {0x0206, "e"},
  {0x0207, "e"},
  {0x0208, "i"},
  {0x0209, "i"},
  {0x020A, "i"},
  {0x020B, "i"},
  {0x020C, "o"},
  {0x020D, "o"},
  {0x020E, "o"},
  {0x020F, "o"},
  {0x0210, "r"},
  {0x0211, "r"},
  {0x0212, "r"},
  {0x0213, "r"},
  {0x0214, "u"},
  {0x0215, "u"},
  {0x0216, "u"},
  {0x0217, "u"},
  {0x0218, "s"},
  {0x0219, "s"},
  {0x021A, "t"},
  {0x021B, "t"},
  {0x021E, "h"},
  {0x021F, "h"},
  {0x0226, "a"},
  {0x0227, "a"},
  {0x0228, "e"},
  {0x0229, "e"},
  {0x022A, "o"},
  {0x022B, "o"},
  {0x022C, "o"},
  {0x022D, "o"},
  {0x022E, "o"},
  {0x022F, "o"},
  {0x0230, "o"},
  {0x0231, "o"},
  {0x0232, "y"},
  {0x0233, "y"},
  {0x1E00, "a"},
  {0x1E01, "a"},
  {0x1E02, "b"},
  {0x1E03, "b"},
  {0x1E04, "b"},
  {0x1E05, "b"},
  {0x1E06, "b"},
  {0x1E07, "b"},
  {0x1E08, "c"},
  {0x1E09, "c"},
  {0x1E0A, "d"},
  {0x1E0B, "d"},
  {0x1E0C, "d"},
  {0x1E0D, "d"},
  {0x1E0E, "d"},
  {0x1E0F, "d"},
  {0x1E10, "d"},
  {0x1E11, "d"},
  {0x1E12, "d"},
  {0x1E13, "d"},
  {0x1E14, "e"},
  {0x1E15, "e"},
  {0x1E16, "e"},
  {0x1E17, "e"},
  {0x1E18, "e"},
  {0x1E19, "e"},
  {0x1E1A, "e"},
  {0x1E1B, "e"},
  {0x1E1C, "e"},
  {0x1E1D, "e"},
  {0x1E1E, "f"},
  {0x1E1F, "f"},
  {0x1E20, "g"},
  {0x1E21, "g"},
  {0x1E22, "h"},
  {0x1E23, "h"},
  {0x1E24, "h"},
  {0x1E25, "h"},
  {0x1E26, "h"},
  {0x1E27, "h"},
  {0x1E28, "h"},
  {0x1E29, "h"},
  {0x1E2A, "h"},
  {0x1E2B, "h"},
  {0x1E2C, "i"},
  {0x1E2D, "i"},
  {0x1E2E, "i"},
  {0x1E2F, "i"},
  {0x1E30, "k"},
  {0x1E31, "k"},
  {0x1E32, "k"},
  {0x1E33, "k"},
  {0x1E34, "k"},
  {0x1E35, "k"},
  {0x1E36, "l"},
  {0x1E37, "l"},
  {0x1E38, "l"},
  {0x1E39, "l"},
  {0x1E3A, "l"},
  {0x1E3B, "l"},
  {0x1E3C, "l"},
  {0x1E3D, "l"},
  {0x1E3E, "m"},
  {0x1E3F, "m"},
  {0x1E40, "m"},
  {0x1E41, "m"},
  {0x1E42, "m"},
  {0x1E43, "m"},
  {0x1E44, "n"},
  {0x1E45, "n"},
  {0x1E46, "n"},
  {0x1E47, "n"},
  {0x1E48, "n"},
  {0x1E49, "n"},
  {0x1E4A, "n"},
  {0x1E4B, "n"},
  {0x1E4C, "o"},
  {0x1E4D, "o"},
  {0x1E4E, "o"},
  {0x1E4F, "o"},
  {0x1E50, "o"},
  {0x1E51, "o"},
  {0x1E52, "o"},
  {0x1E53, "o"},
  {0x1E54, "p"},
  {0x1E55, "p"},
  {0x1E56, "p"},
  {0x1E57, "p"},
  {0x1E58, "r"},
  {0x1E59, "r"},
  {0x1E5A, "r"},
  {0x1E5B, "r"},
  {0x1E5C, "r"},
  {0x1E5D, "r"},
  {0x1E5E, "r"},
  {0x1E5F, "r"},
  {0x1E60, "s"},
  {0x1E61, "s"},
  {0x1E62, "s"},
  {0x1E63, "s"},
  {0x1E64, "s"},
  {0x1E65, "s"},
  {0x1E66, "s"},
  {0x1E67, "s"},
  {0x1E68, "s"},
  {0x1E69, "s"},
  {0x1E6A, "t"},
  {0x1E6B, "t"},
  {0x1E6C, "t"},
  {0x1E6D, "t"},
  {0x1E6E, "t"},
  {0x1E6F, "t"},
  {0x1E70, "t"},
  {0x1E71, "t"},
  {0x1E72, "u"},
  {0x1E73, "u"},
  {0x1E74, "u"},
  {0x1E75, "u"},
  {0x1E76, "u"},
  {0x1E77, "u"},
  {0x1E78, "u"},
  {0x1E79, "u"},
  {0x1E7A, "u"},
  {0x1E7B, "u"},
  {0x1E7C, "v"},
  {0x1E7D, "v"},
  {0x1E7E, "v"},
  {0x1E7F, "v"},
  {0x1E80, "w"},
  {0x1E81, "w"},
  {0x1E82, "w"},
  {0x1E83, "w"},
  {0x1E84, "w"},
  {0x1E85, "w"},
  {0x1E86, "w"},
  {0x1E87, "w"},
  {0x1E88, "w"},
  {0x1E89, "w"},
  {0x1E8A, "x"},
  {0x1E8B, "x"},
  {0x1E8C, "x"},
  {0x1E8D, "x"},
  {0x1E8E, "y"},
  {0x1E8F, "y"},
  {0x1E90, "z"},
  {0x1E91, "z"},
  {0x1E92, "z"},
  {0x1E93, "z"},
  {0x1E94, "z"},
  {0x1E95, "z"},
  {0x1E96, "h"},
  {0x1E97, "t"},
  {0x1E98, "w"},
  {0x1E99, "y"},
  {0x1E9A, "a"},
  {0x1E9B, "s"},
  {0x1EA0, "a"},
  {0x1EA1, "a"},
  {0x1EA2, "a"},
  {0x1EA3, "a"},
  {0x1EA4, "a"},
  {0x1EA5, "a"},
  {0x1EA6, "a"},
  {0x1EA7, "a"},
  {0x1EA8, "a"},
  {0x1EA9, "a"},
  {0x1EAA, "a"},
  {0x1EAB, "a"},
  {0x1EAC, "a"},
  {0x1EAD, "a"},
  {0x1EAE, "a"},
  {0x1EAF, "a"},
  {0x1EB0, "a"},
  {0x1EB1, "a"},
  {0x1EB2, "a"},
  {0x1EB3, "a"},
  {0x1EB4, "a"},
  {0x1EB5, "a"},
  {0x1EB6, "a"},
  {0x1EB7, "a"},
  {0x1EB8, "e"},
  {0x1EB9, "e"},
  {0x1EBA, "e"},
  {0x1EBB, "e"},
  {0x1EBC, "e"},
  {0x1EBD, "e"},
  {0x1EBE, "e"},
  {0x1EBF, "e"},
  {0x1EC0, "e"},
  {0x1EC1, "e"},
  {0x1EC2, "e"},
  {0x1EC3, "e"},
  {0x1EC4, "e"},
  {0x1EC5, "e"},
  {0x1EC6, "e"},
  {0x1EC7, "e"},
  {0x1EC8, "i"},
  {0x1EC9, "i"},
  {0x1ECA, "i"},
  {0x1ECB, "i"},
  {0x1ECC, "o"},
  {0x1ECD, "o"},
  {0x1ECE, "o"},
  {0x1ECF, "o"},
  {0x1ED0, "o"},
  {0x1ED1, "o"},
  {0x1ED2, "o"},
  {0x1ED3, "o"},
  {0x1ED4, "o"},
  {0x1ED5, "o"},
  {0x1ED6, "o"},
  {0x1ED7, "o"},
  {0x1ED8, "o"},
  {0x1ED9, "o"},
  {0x1EDA, "o"},
  {0x1EDB, "o"},
  {0x1EDC, "o"},
  {0x1EDD, "o"},
  {0x1EDE, "o"},
  {0x1EDF, "o"},
  {0x1EE0, "o"},
  {0x1EE1, "o"},
  {0x1EE2, "o"},
  {0x1EE3, "o"},
  {0x1EE4, "u"},
  {0x1EE5, "u"},
  {0x1EE6, "u"},
  {0x1EE7, "u"},
  {0x1EE8, "u"},
  {0x1EE9, "u"},
  {0x1EEA, "u"},
  {0x1EEB, "u"},
  {0x1EEC, "u"},
  {0x1EED, "u"},
  {0x1EEE, "u"},
  {0x1EEF, "u"},
  {0x1EF0, "u"},
  {0x1EF1, "u"},
  {0x1EF2, "y"},
  {0x1EF3, "y"},
  {0x1EF4, "y"},
  {0x1EF5, "y"},
  {0x1EF6, "y"},
  {0x1EF7, "y"},
  {0x1EF8, "y"},
  {0x1EF9, "y"},
};

/* One UTF-8 codepoint at p. Returns 0xFFFFFFFF on a malformed sequence and
   consumes exactly one byte, so a stray byte cannot derail the scan. */
static uint32_t utf8_next(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  uint32_t cp;
  int extra;
  if (s[0] < 0x80) {
    (*p)++;
    return s[0];
  } else if ((s[0] & 0xE0) == 0xC0) {
    cp = s[0] & 0x1F;
    extra = 1;
  } else if ((s[0] & 0xF0) == 0xE0) {
    cp = s[0] & 0x0F;
    extra = 2;
  } else if ((s[0] & 0xF8) == 0xF0) {
    cp = s[0] & 0x07;
    extra = 3;
  } else {
    (*p)++;
    return 0xFFFFFFFFu;
  }
  for (int i = 1; i <= extra; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      (*p)++;
      return 0xFFFFFFFFu;
    }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  *p += extra + 1;
  return cp;
}

/* ASCII replacement for one codepoint, or NULL to keep it as it is. */
static const char *fold_ascii(uint32_t cp) {
  int lo = 0;
  int hi = (int)(sizeof(FOLD_TABLE) / sizeof(FOLD_TABLE[0])) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (FOLD_TABLE[mid].cp == cp)
      return FOLD_TABLE[mid].ascii;
    if (FOLD_TABLE[mid].cp < cp)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return NULL;
}

/* Scripts that are written without spaces between words: one character is
   already a word, so each becomes its own token instead of one giant run. */
static int cp_is_cjk(uint32_t cp) {
  return (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x20000 && cp <= 0x2FA1F);
}

static char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int ascii_alnum(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

/* Append the folded form of one codepoint to buf. Returns bytes written. */
static int fold_into(uint32_t cp, char *buf, size_t room) {
  const char *ascii = fold_ascii(cp);
  size_t n;
  if (ascii) {
    n = strlen(ascii);
    if (n > room)
      n = room;
    for (size_t i = 0; i < n; i++)
      buf[i] = ascii_lower(ascii[i]);
    return (int)n;
  }
  if (cp < 0x80) {
    if (room < 1)
      return 0;
    buf[0] = ascii_lower((char)cp);
    return 1;
  }
  /* keep the raw bytes: script we have no folding for still tokenizes */
  {
    char tmp[4];
    int len = 0;
    if (cp < 0x800) {
      tmp[len++] = (char)(0xC0 | (cp >> 6));
      tmp[len++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      tmp[len++] = (char)(0xE0 | (cp >> 12));
      tmp[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      tmp[len++] = (char)(0x80 | (cp & 0x3F));
    } else {
      tmp[len++] = (char)(0xF0 | (cp >> 18));
      tmp[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      tmp[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      tmp[len++] = (char)(0x80 | (cp & 0x3F));
    }
    for (int i = 0; i < len && (size_t)i < room; i++)
      buf[i] = tmp[i];
    return len;
  }
}

void tokens_fold(char *dst, size_t size, const char *src) {
  size_t used = 0;
  const char *p = src ? src : "";
  while (*p) {
    uint32_t cp = utf8_next(&p);
    char buf[8];
    int n;
    if (cp == 0xFFFFFFFFu)
      continue;
    n = fold_into(cp, buf, sizeof(buf));
    for (int i = 0; i < n && used + 1 < size; i++)
      dst[used++] = buf[i];
  }
  dst[used] = '\0';
}

static int token_is_word_byte(char c) { return (unsigned char)c >= 0x80; }

static void token_flush(char *buf, size_t *len, char out[][TOKEN_LEN], int max,
                        int *count, int unique) {
  if (*len == 0)
    return;
  buf[*len] = '\0';
  /* digits are a word at length 1; letters and mixed runs need 2 */
  int keep = (*len >= 2) || (*len == 1 && buf[0] >= '0' && buf[0] <= '9');
  if (keep) {
    int dup = 0;
    if (unique) {
      for (int i = 0; i < *count; i++)
        if (strcmp(out[i], buf) == 0) {
          dup = 1;
          break;
        }
    }
    if (!dup && *count < max)
      snprintf(out[(*count)++], TOKEN_LEN, "%s", buf);
  }
  *len = 0;
}

int tokens_split(const char *text, char out[][TOKEN_LEN], int max, int unique) {
  char buf[TOKEN_LEN * 4];
  size_t len = 0;
  int count = 0;
  const char *p = text ? text : "";
  if (max <= 0)
    return 0;
  while (*p) {
    uint32_t cp = utf8_next(&p);
    if (cp == 0xFFFFFFFFu)
      continue;
    if (cp < 0x80) {
      if (ascii_alnum((unsigned char)cp)) {
        if (len + 1 < sizeof(buf))
          buf[len++] = ascii_lower((char)cp);
      } else {
        token_flush(buf, &len, out, max, &count, unique);
      }
      continue;
    }
    if (cp_is_cjk(cp)) {
      token_flush(buf, &len, out, max, &count, unique);
      char tmp[4];
      char keep[8];
      int n = fold_into(cp, tmp, sizeof(tmp));
      for (int i = 0; i < n && (size_t)i + 1 < sizeof(keep); i++)
        keep[i] = tmp[i];
      keep[n] = '\0';
      if (count < max)
        snprintf(out[count++], TOKEN_LEN, "%s", keep);
      continue;
    }
    {
      char tmp[8];
      int n = fold_into(cp, tmp, sizeof(tmp));
      for (int i = 0; i < n; i++) {
        if (len + 1 >= sizeof(buf))
          break;
        if (token_is_word_byte(tmp[i]) || ascii_alnum((unsigned char)tmp[i]))
          buf[len++] = tmp[i];
      }
    }
  }
  token_flush(buf, &len, out, max, &count, unique);
  return count;
}

/* Folded substring search: does `needle` (already folded) occur in `hay`? */
static int literal_contains(const char *hay, const char *needle) {
  size_t want = strlen(needle);
  if (want == 0 || !hay)
    return 0;
  for (const char *start = hay; *start; start++) {
    const char *h = start;
    size_t i = 0;
    int ok = 1;
    while (*h && i < want) {
      uint32_t cp = utf8_next(&h);
      char buf[8];
      int n;
      if (cp == 0xFFFFFFFFu)
        continue;
      n = fold_into(cp, buf, sizeof(buf));
      for (int k = 0; k < n; k++) {
        if (i >= want || buf[k] != needle[i]) {
          ok = 0;
          break;
        }
        i++;
      }
      if (!ok)
        break;
    }
    if (ok && i == want)
      return 1;
  }
  return 0;
}

/* One edit apart (insert, delete, substitute). Term length is the floor. */
static int edit_within_one(const char *a, const char *b) {
  int la = (int)strlen(a);
  int lb = (int)strlen(b);
  if (la - lb > 1 || lb - la > 1)
    return 0;
  int i = 0;
  int j = 0;
  int edits = 0;
  while (i < la && j < lb) {
    if (a[i] == b[j]) {
      i++;
      j++;
      continue;
    }
    if (++edits > 1)
      return 0;
    if (la > lb)
      i++;
    else if (lb > la)
      j++;
    else {
      i++;
      j++;
    }
  }
  if (i < la || j < lb)
    edits++;
  return edits <= 1;
}

/* Keeps the pre-existing tolerance: a term of five characters or more also
   matches a token containing it, or one edit away (typos). */
static int sub_token_matches(const char *token, const char *sub) {
  if (strcmp(token, sub) == 0)
    return 1;
  if (strlen(sub) < 5)
    return 0;
  if (strstr(token, sub))
    return 1;
  return edit_within_one(token, sub);
}

/* A term carrying punctuation or a symbol is the case the literal path
   exists for: "1+1", "covid-19", "c++", ".net", "node.js", "AT&T", "50%".
   A plain word must match through tokens, because the literal path also
   matches the SERP's own echo of the query ("... - Search results"), which
   made "zzqqxx vbbnmm" render 11 results instead of none. */
static int term_is_punctuated(const char *term) {
  for (const unsigned char *p = (const unsigned char *)term; *p; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
        (*p >= '0' && *p <= '9') || *p >= 0x80)
      continue;
    return 1;
  }
  return 0;
}

int tokens_term_matches(const char *term, const char tokens[][TOKEN_LEN],
                        int count, const char *text) {
  char sub[16][TOKEN_LEN];
  int n;
  if (!term || !*term)
    return 0;
  if (text && *text && term_is_punctuated(term) && literal_contains(text, term))
    return 1;
  n = tokens_split(term, sub, 16, 1);
  if (n == 0)
    return 0; /* symbol-only term: the literal path above is all there is */
  for (int i = 0; i < n; i++) {
    int found = 0;
    for (int k = 0; k < count; k++) {
      if (sub_token_matches(tokens[k], sub[i])) {
        found = 1;
        break;
      }
    }
    if (!found)
      return 0;
  }
  return 1;
}

int tokens_term_has_letter(const char *term) {
  if (!term)
    return 0;
  for (const unsigned char *p = (const unsigned char *)term; *p; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
      return 1;
    if (*p >= 0x80)
      return 1;
  }
  return 0;
}
