/* The scope string a search's cache entry is filed under.

   Its own function, in its own file, so it can be tested without dragging in
   the whole search handler: the category is part of the identity of a result
   set, and leaving it out made cat=media share an entry with cat=web. */
#ifndef SEARCH_CACHE_KEY_H
#define SEARCH_CACHE_KEY_H

#include <stddef.h>

/* Fills out with e.g.
     search_request:all:<client>:cat=any
     search_request:ddg:<client>:cat=media

   engine_id and category may be NULL or "". client_key may not. */
void search_cache_scope(char *out, size_t cap, const char *engine_id,
                        const char *client_key, const char *category);

#endif
