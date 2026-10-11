#ifndef RAIJU_PROXY_H
#define RAIJU_PROXY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raiju/strbuf.h"

/**
 * A parsed aria2 RPC endpoint: host, port and the request path.
 */
typedef struct {
	char host[256];
	uint16_t port;
	char path[512];
} raiju_upstream;

/**
 * A response from the upstream engine: its status, Content-Type and body.
 */
typedef struct {
	int status;
	int transport_error;
	char content_type[128];
	raiju_strbuf body;
} raiju_upstream_response;

/**
 * Parses an http://host:port/path URL into an endpoint. Only http is
 * accepted; https, a missing host or a malformed port fail. A URL without a
 * path uses "/jsonrpc".
 */
bool raiju_upstream_parse(const char *url, raiju_upstream *out);

/**
 * Ensures the aria2 secret is carried as the first element of the request's
 * "params" array, so clients never send it. Returns a newly allocated
 * NUL-terminated body and its length, or NULL. When the body already carries
 * a "token:" parameter it is copied unchanged. A request without a "params"
 * key gains one.
 */
char *raiju_proxy_inject_secret(const char *body, size_t length, const char *secret, size_t *out_length);

/**
 * Sends a POST to the endpoint with the given JSON body and reads the whole
 * response, with the socket timeout in milliseconds. Returns 0 when a
 * response was received, -1 on a transport failure (recorded in
 * response->transport_error). Release the body with
 * raiju_upstream_response_free.
 */
int raiju_upstream_post(const raiju_upstream *upstream, const unsigned char *body, size_t length, int timeout_ms,
                        raiju_upstream_response *response);

/**
 * Releases a response's buffers.
 */
void raiju_upstream_response_free(raiju_upstream_response *response);

#endif
