/* M4: the owner pastes one of three things from the Programmable Search
   Overview page, and all three carry the same identifier:

     the Search engine ID    a25d329ae5f514a70
     the Public URL          https://cse.google.com/cse?cx=a25d329ae5f514a70
     the embed code          <script async src="https://cse.google.com/cse.js?cx=a25d329ae5f514a70"></script>

   Accepting only the bare id meant the hint "copy the Search engine ID" had to
   be followed exactly, and anything else was silently rejected. */
#ifndef CSE_ID_H
#define CSE_ID_H

#include <stddef.h>

/* Extract the cx from any of the three forms. Returns 0 and fills out, or -1
   when no id can be recognised. */
int cse_extract_id(const char *input, char *out, size_t cap);

#endif
