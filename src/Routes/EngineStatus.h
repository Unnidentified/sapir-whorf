#ifndef OMNISEARCH_ROUTES_ENGINESTATUS_H
#include <beaker.h>

#define OMNISEARCH_ROUTES_ENGINESTATUS_H

/* K3: the same rows /settings renders, as JSON, so the page can refresh them
   while an engine is suspended without reloading. Contains no query text and
   echoes no cookie: it reports the health table the dispatcher already uses. */
int engine_status_handler(UrlParams *params);

#endif
