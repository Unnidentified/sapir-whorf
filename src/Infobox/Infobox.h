#ifndef INFOBOX_H
#define INFOBOX_H

typedef struct {
  char *title;
  char *thumbnail_url;
  char *extract;
  char *url;
  /* J6.2: 1 when extract is HTML built by a local generator (arithmetic,
     colour, units, currency, dictionary). 0 for anything fetched from the web,
     whose text is escaped before it reaches the template. Zero-initialised
     structs - which is every existing generator - mean "plain text", which is
     the safe default. */
  int is_html;
} InfoBox;

void free_infobox(InfoBox *info);

#endif
