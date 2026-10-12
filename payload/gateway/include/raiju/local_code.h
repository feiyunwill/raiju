#ifndef RAIJU_LOCAL_CODE_H
#define RAIJU_LOCAL_CODE_H

#include "raiju/http.h"

/**
 * Answers GET /api/pairing/code: the running instance's pairing code, for the
 * second copy of the payload that could not take the lock and has to re-show
 * the "already running" notification. Only a peer on the console itself has
 * that privilege, and it has to present the engine secret both copies read
 * from the session directory; anything else is refused with 403.
 */
int raiju_local_code_handler(const raiju_http_request *request, raiju_http_response *response, void *context);

#endif
