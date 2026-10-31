#include "ColourCode.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const unsigned char HEX_VALS[256] = {
    ['0'] = 0,  ['1'] = 1,  ['2'] = 2,  ['3'] = 3,  ['4'] = 4,  ['5'] = 5,
    ['6'] = 6,  ['7'] = 7,  ['8'] = 8,  ['9'] = 9,  ['a'] = 10, ['b'] = 11,
    ['c'] = 12, ['d'] = 13, ['e'] = 14, ['f'] = 15, ['A'] = 10, ['B'] = 11,
    ['C'] = 12, ['D'] = 13, ['E'] = 14, ['F'] = 15,
};

static int is_hex_digit(char c) {
  return HEX_VALS[(unsigned char)c] || c == '0';
}

static int is_valid_hex(const char *s, int len) {
  if (len != 3 && len != 6)
    return 0;
  for (int i = 0; i < len; i++) {
    if (!is_hex_digit(s[i]))
      return 0;
  }
  return 1;
}

static void hex_to_rgb(const char *hex, int *r, int *g, int *b, int len) {
  *r = *g = *b = 0;
  if (len == 3) {
    *r = HEX_VALS[(unsigned char)hex[0]] * 17;
    *g = HEX_VALS[(unsigned char)hex[1]] * 17;
    *b = HEX_VALS[(unsigned char)hex[2]] * 17;
  } else if (len == 6) {
    *r = HEX_VALS[(unsigned char)hex[0]] * 16 + HEX_VALS[(unsigned char)hex[1]];
    *g = HEX_VALS[(unsigned char)hex[2]] * 16 + HEX_VALS[(unsigned char)hex[3]];
    *b = HEX_VALS[(unsigned char)hex[4]] * 16 + HEX_VALS[(unsigned char)hex[5]];
  }
}

static void rgb_to_hsl(int r, int g, int b, int *h, int *s, int *l) {
  int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
  int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
  int delta = max - min;
  *l = (max + min) / 2;
  *s = 0;
  *h = 0;
  if (delta > 0) {
    *s = (int)((double)delta / (1.0 - fabs(2.0 * *l / 255.0 - 1.0)) * 100.0);
    if (max == r) {
      *h = (int)(60.0 * fmod((double)(g - b) / delta, 6.0));
    } else if (max == g) {
      *h = (int)(60.0 * (((b - r) / delta) + 2.0));
    } else {
      *h = (int)(60.0 * (((r - g) / delta) + 4.0));
    }
    if (*h < 0)
      *h += 360;
  }
}

static void rgb_to_hsv(int r, int g, int b, int *h, int *s, int *v) {
  int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
  int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
  int delta = max - min;
  *v = max * 100 / 255;
  *s = max > 0 ? delta * 100 / max : 0;
  *h = 0;
  if (delta > 0) {
    if (max == r) {
      *h = (int)(60.0 * fmod((double)(g - b) / delta, 6.0));
    } else if (max == g) {
      *h = (int)(60.0 * (((b - r) / delta) + 2.0));
    } else {
      *h = (int)(60.0 * (((r - g) / delta) + 4.0));
    }
    if (*h < 0)
      *h += 360;
  }
}

static void rgb_to_cmyk(int r, int g, int b, int *c, int *m, int *y, int *k) {
  int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
  double kf = 1.0 - max / 255.0;
  if (kf >= 0.99) {
    *c = *m = *y = *k = 100;
  } else {
    *c = (int)((1.0 - r / 255.0 - kf) / (1.0 - kf) * 100);
    *m = (int)((1.0 - g / 255.0 - kf) / (1.0 - kf) * 100);
    *y = (int)((1.0 - b / 255.0 - kf) / (1.0 - kf) * 100);
    *k = (int)(kf * 100);
  }
}

int is_colour_code_query(const char *query) {
  if (!query)
    return 0;
  const char *p = query;
  /* H4: a leading space is common when people paste, and the old check was
     p[0]=='#' so "  #123456" fell through to the network. */
  while (*p == ' ' || *p == '\t')
    p++;
  /* G4: rgb() is how most people paste a colour, and without this the answer
     box fell through to a network lookup for a question it can answer
     instantly. */
  if (strncasecmp(p, "rgb(", 4) == 0 || strncasecmp(p, "rgba(", 5) == 0) {
    int r = -1, g = -1, b = -1;
    const char *open = strchr(p, '(');
    if (!open)
      return 0;
    if (sscanf(open + 1, "%d ,%d ,%d", &r, &g, &b) != 3 &&
        sscanf(open + 1, "%d, %d, %d", &r, &g, &b) != 3)
      return 0;
    return r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255;
  }
  if (p[0] == '#') {
    p++;
    while (*p == ' ')
      p++;
    const char *end = p;
    while (is_hex_digit(*end))
      end++;
    int len = end - p;
    return is_valid_hex(p, len);
  }
  return 0;
}

InfoBox fetch_colour_data(char *query) {
  InfoBox info = {NULL, NULL, NULL, NULL};
  if (!query)
    return info;

  const char *p = query;
  while (*p == ' ' || *p == '\t')
    p++;
  if (strncasecmp(p, "rgb(", 4) == 0 || strncasecmp(p, "rgba(", 5) == 0) {
    int r = -1, g = -1, b = -1;
    const char *open = strchr(p, '(');
    if (!open)
      return info;
    if (sscanf(open + 1, "%d ,%d ,%d", &r, &g, &b) != 3 &&
        sscanf(open + 1, "%d, %d, %d", &r, &g, &b) != 3)
      return info;
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
      return info;

    int h, s2, l, hh, ss, v, c, m, y, k;
    rgb_to_hsl(r, g, b, &h, &s2, &l);
    rgb_to_hsv(r, g, b, &hh, &ss, &v);
    rgb_to_cmyk(r, g, b, &c, &m, &y, &k);

    char html[1024];
    snprintf(html, sizeof(html),
             "<div class='colour-container' style='line-height: 1.6;'>"
             "<div style='display: flex; align-items: flex-start; gap: 16px;'>"
             "<div style='width: 100px; height: 100px; border-radius: 12px; "
             "border: 1px solid #ddd; background-color: rgb(%d,%d,%d); "
             "flex-shrink: 0;'></div>"
             "<div>"
             "<div style='font-size: 1.3em; font-weight: bold; margin-bottom: "
             "8px;'>#%02X%02X%02X</div>"
             "<div style='color: #666; margin-bottom: 4px;'>RGB(%d, %d, "
             "%d)</div>"
             "<div style='color: #666; margin-bottom: 4px;'>HSL(%d, %d%%, "
             "%d%%)</div>"
             "<div style='color: #666; margin-bottom: 4px;'>HSV(%d, %d%%, "
             "%d%%)</div>"
             "<div style='color: #666;'>CMYK(%d%%, %d%%, %d%%, %d%%)</div>"
             "</div></div></div>",
             r, g, b, r, g, b, r, g, b, h, s2, l, hh, ss, v, c, m, y, k);
    info.title = strdup("Colour");
    info.extract = strdup(html);
    info.url = strdup("#");
    return info;
  }
  if (p[0] != '#')
    return info;
  p++;
  while (*p == ' ')
    p++;
  const char *end = p;
  while (is_hex_digit(*end))
    end++;
  int len = end - p;
  if (!is_valid_hex(p, len))
    return info;

  int r, g, b;
  hex_to_rgb(p, &r, &g, &b, len);

  int h, s, l;
  rgb_to_hsl(r, g, b, &h, &s, &l);

  int h2, s2, v;
  rgb_to_hsv(r, g, b, &h2, &s2, &v);

  int c, m, y, k;
  rgb_to_cmyk(r, g, b, &c, &m, &y, &k);

  char html[1024];
  snprintf(html, sizeof(html),
           "<div class='colour-container' style='line-height: 1.6;'>"
           "<div style='display: flex; align-items: flex-start; gap: 16px;'>"
           "<div style='width: 100px; height: 100px; border-radius: 12px; "
           "border: 1px solid #ddd; background-color: #%.*s; flex-shrink: 0;'></div>"
           "<div>"
           "<div style='font-size: 1.3em; font-weight: bold; margin-bottom: 8px;'>"
           "#%.*s</div>"
           "<div style='color: #666; margin-bottom: 4px;'>RGB(%d, %d, %d)</div>"
           "<div style='color: #666; margin-bottom: 4px;'>HSL(%d, %d%%, %d%%)</div>"
           "<div style='color: #666; margin-bottom: 4px;'>HSV(%d, %d%%, %d%%)</div>"
           "<div style='color: #666;'>CMYK(%d%%, %d%%, %d%%, %d%%)</div>"
           "</div>"
           "</div>"
           "</div>",
           len, p, len, p, r, g, b, h, s, l, h2, s2, v, c, m, y, k);

  info.title = strdup("Colour");
  info.extract = strdup(html);
  info.thumbnail_url = NULL;
  info.url = strdup("#");

  return info;
}
