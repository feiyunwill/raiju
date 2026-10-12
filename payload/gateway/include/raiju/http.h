#ifndef RAIJU_HTTP_H
#define RAIJU_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#include "raiju/strbuf.h"

#define RAIJU_HTTP_MAX_PARAMS 8
#define RAIJU_HTTP_PARAM_MAX 128

/**
 * One parsed request, independent of the server library so handlers and
 * tests share the same shape. body, query and header values point into the
 * server's buffers and live only for the call; matched path parameters are
 * copied into param_values. peer_address is the address the server saw the
 * request come from, or NULL when a caller builds a request without a socket.
 */
typedef struct {
	const char *method;
	const char *path;
	const char *query;
	const char *cookie;
	const char *authorization;
	const char *content_type;
	const char *peer_address;
	const unsigned char *body;
	size_t body_length;
	const char *params[RAIJU_HTTP_MAX_PARAMS];
	size_t param_count;
	char param_values[RAIJU_HTTP_MAX_PARAMS][RAIJU_HTTP_PARAM_MAX];
} raiju_http_request;

/**
 * The response a handler builds: a status, headers and a body buffer. The
 * gateway flushes it to the socket; tests read it in memory.
 */
typedef struct {
	int status;
	raiju_strbuf headers[RAIJU_HTTP_MAX_PARAMS];
	size_t header_count;
	raiju_strbuf body;
} raiju_http_response;

/**
 * A route: an exact method and a path pattern where a segment beginning with
 * ':' matches any single segment and is captured as a parameter.
 */
typedef struct raiju_route {
	const char *method;
	const char *pattern;
	int (*handler)(const raiju_http_request *request, raiju_http_response *response, void *context);
	void *context;
	bool requires_auth;
} raiju_route;

/**
 * Initialises an empty response with status 200.
 */
void raiju_response_init(raiju_http_response *response);

/**
 * Releases the response's buffers.
 */
void raiju_response_free(raiju_http_response *response);

/**
 * Sets the response status code.
 */
void raiju_response_set_status(raiju_http_response *response, int status);

/**
 * Adds a response header. Silently ignored once the header slots are full.
 */
void raiju_response_set_header(raiju_http_response *response, const char *name, const char *value);

/**
 * Appends raw bytes to the response body.
 */
void raiju_response_write(raiju_http_response *response, const void *bytes, size_t length);

/**
 * Appends a NUL-terminated string to the response body.
 */
void raiju_response_write_str(raiju_http_response *response, const char *text);

/**
 * Replaces the body with a JSON document and sets Content-Type.
 */
void raiju_response_json(raiju_http_response *response, int status, const char *json);

/**
 * Produces a JSON error document { "error": message } with the given status.
 */
void raiju_response_error(raiju_http_response *response, int status, const char *message);

/**
 * Matches a request against the table, filling request->params on a match,
 * and returns the handler's status, or 404 when no route matches. A pattern
 * ending in "*" matches any suffix.
 */
int raiju_route_dispatch(const raiju_route *routes, size_t count, raiju_http_request *request,
                         raiju_http_response *response, bool *out_requires_auth, bool *out_matched);

/**
 * Finds the first route matching the request's method and path, filling
 * request->params and reporting the route's requires_auth flag. Returns the
 * matched index, or count when nothing matches. Use this to run the auth
 * gate before invoking the handler.
 */
size_t raiju_route_match(const raiju_route *routes, size_t count, raiju_http_request *request,
                         bool *out_requires_auth);

#endif
