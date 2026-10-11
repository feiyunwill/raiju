#ifndef RAIJU_FETCH_H
#define RAIJU_FETCH_H

#include <stdbool.h>
#include <stddef.h>

#define RAIJU_FETCH_DEFAULT_TIMEOUT_MS 15000
#define RAIJU_FETCH_DEFAULT_MAX_BYTES (4u * 1024u * 1024u)
#define RAIJU_FETCH_NO_LENGTH ((size_t)-1)

typedef enum {
	RAIJU_FETCH_GET = 0,
	RAIJU_FETCH_HEAD
} raiju_fetch_method;

/**
 * One outbound response: the HTTP status, the body as a NUL-terminated
 * buffer the caller frees with raiju_fetch_response_free, the announced
 * Content-Length (RAIJU_FETCH_NO_LENGTH when absent), the raw Content-Range
 * header, and whether the body overran the byte cap.
 */
typedef struct {
	int status;
	char *body;
	size_t body_length;
	size_t content_length;
	char content_range[128];
	bool truncated;
} raiju_fetch_response;

/**
 * The outbound HTTP(S) client seam. It performs exactly one request without
 * following redirects, and returns 0 once a response was read (any HTTP
 * status) or -1 on a transport failure or when the body overran max_bytes.
 * The range string, when not NULL, is sent as the Range header. Tests
 * substitute a fake or a local fixture server; the gateway never reaches the
 * internet from a test.
 */
typedef int (*raiju_fetch_fn)(void *context, raiju_fetch_method method, const char *url, const char *range,
                              size_t max_bytes, int timeout_ms, raiju_fetch_response *response);

/**
 * The built-in client: plain HTTP over a socket, and HTTPS through OpenSSL
 * when the library was compiled with RAIJU_FETCH_TLS (and the build linked
 * OpenSSL). Without TLS an https URL fails as a transport error.
 */
raiju_fetch_fn raiju_fetch_default(void);

/**
 * Releases the body a fetch allocated and zeroes the response.
 */
void raiju_fetch_response_free(raiju_fetch_response *response);

#endif
