#ifndef DESCRIBE_H
#define DESCRIBE_H
#include <beaker.h>
/* K7: GET /d?u=<base64url>&s=<hmac> - the description of a result whose search
   engine would not describe it. The URL must have been signed by this server,
   so the endpoint cannot be used to fetch an arbitrary address. */
int describe_handler(UrlParams *params);
#endif
