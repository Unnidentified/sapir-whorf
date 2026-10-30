#ifndef WIKIPEDIA_H
#define WIKIPEDIA_H

#include "Infobox.h"

InfoBox fetch_wiki_data(char *api_url);
char *construct_wiki_url(const char *search_term);
char *construct_wiki_url_lang(const char *search_term, const char *lang);

#endif
