#ifndef RAIJU_AUTH_H
#define RAIJU_AUTH_H

#include <stdbool.h>
#include <stddef.h>

#include "raiju/http.h"

/**
 * The name of the HTTP-only cookie that carries a device token. Extension
 * clients send the same value in an Authorization header instead.
 */
const char *raiju_auth_cookie_name(void);

/**
 * Extracts the device token from the request, preferring the named cookie
 * and falling back to an "Authorization: Bearer <token>" or
 * "Authorization: <token>" header. Returns false when no token is present;
 * out receives a NUL-terminated copy bounded by capacity.
 */
bool raiju_auth_token(const raiju_http_request *request, char *out, size_t capacity);

/**
 * Builds the Set-Cookie header value that installs the device cookie as
 * HttpOnly, SameSite=Lax and long-lived, into a caller buffer.
 */
void raiju_auth_set_cookie(const char *token, char *out, size_t capacity);

#endif
