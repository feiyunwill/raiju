#ifndef RAIJU_FIXTURE_SERVER_H
#define RAIJU_FIXTURE_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAIJU_FIXTURE_MAX_FILES 8

typedef struct raiju_fixture_server raiju_fixture_server;

/**
 * Starts a local plain-HTTP server on 127.0.0.1 with an ephemeral port. It
 * serves stored byte ranges (GET and HEAD, honouring Range) and answers any
 * POST body with the configured JSON-RPC response. No internet is used.
 */
raiju_fixture_server *raiju_fixture_server_start(void);

/**
 * Stops the server, joins its thread and frees the stored files.
 */
void raiju_fixture_server_stop(raiju_fixture_server *server);

/**
 * The port the server is bound to.
 */
uint16_t raiju_fixture_server_port(const raiju_fixture_server *server);

/**
 * Sets the body returned for POST /jsonrpc; NULL or empty makes it 500.
 */
void raiju_fixture_server_set_jsonrpc(raiju_fixture_server *server, const char *body);

/**
 * Stores a byte array under a path (e.g. "/game.zip") the server then serves.
 * The server keeps its own copy.
 */
bool raiju_fixture_server_add_file(raiju_fixture_server *server, const char *path, const void *bytes, size_t length);

/**
 * The Range header of the most recent request, or "".
 */
const char *raiju_fixture_server_last_range(const raiju_fixture_server *server);

/**
 * The method of the most recent request, or "".
 */
const char *raiju_fixture_server_last_method(const raiju_fixture_server *server);

/**
 * The Host header of the most recent request, or "".
 */
const char *raiju_fixture_server_last_host(const raiju_fixture_server *server);

/**
 * The body of the most recent request, or "", so a test can assert what the
 * gateway sent to the engine.
 */
const char *raiju_fixture_server_last_body(const raiju_fixture_server *server);

/**
 * How many requests have hit a path.
 */
unsigned raiju_fixture_server_hits(const raiju_fixture_server *server, const char *path);

/**
 * Resets the request counters.
 */
void raiju_fixture_server_reset(raiju_fixture_server *server);

#endif
