#ifndef RAIJU_GATEWAY_H
#define RAIJU_GATEWAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raiju/allowlist.h"
#include "raiju/http.h"
#include "raiju/pairing.h"
#include "raiju/platform.h"
#include "raiju/proxy.h"

#define RAIJU_GATEWAY_MAX_ROUTES 24

/**
 * The gateway's runtime inputs. Port 0 asks the OS for an ephemeral port the
 * caller reads back; bind_address NULL uses "0.0.0.0"; www_dir NULL defaults
 * to <data_dir>/www, and that directory overrides the embedded table whenever
 * the file asked for exists there.
 */
typedef struct {
	const raiju_platform *platform;
	const char *bind_address;
	uint16_t port;
	const char *data_dir;
	const char *www_dir;
	const char *aria2_url;
	const char *secret;
	size_t max_body_bytes;
} raiju_gateway_config;

/**
 * The running gateway: its config, the pairing state, the allowlist, the
 * aria2 endpoint, the routing table consumers extend, and the server handle.
 * Issue #17 adds /api/status detail, /api/storage, /api/folders, /api/jobs,
 * /api/probe, /api/art and /api/power/keep-awake by appending routes with
 * raiju_gateway_add_route and implementing handlers of the raiju_route shape.
 */
typedef struct raiju_gateway {
	raiju_gateway_config config;
	raiju_pairing pairing;
	raiju_allowlist allowlist;
	char www_dir[RAIJU_PATH_MAX];
	raiju_upstream upstream;
	char secret[128];
	raiju_route routes[RAIJU_GATEWAY_MAX_ROUTES];
	size_t route_count;
	void *context;
	uint16_t bound_port;
	uint64_t started_at;
} raiju_gateway;

/**
 * Prepares the gateway: copies config, loads pairing, builds the allowlist,
 * parses the aria2 endpoint and registers the built-in routes. Returns NULL
 * on invalid config or allocation failure.
 */
raiju_gateway *raiju_gateway_create(const raiju_gateway_config *config);

/**
 * Appends a route so later issues extend the API without touching the core.
 */
bool raiju_gateway_add_route(raiju_gateway *gateway, const raiju_route *route);

/**
 * Returns the routing table and its length. Handlers receive the gateway as
 * context, so they can reach pairing, allowlist and the upstream endpoint.
 */
const raiju_route *raiju_gateway_routes(raiju_gateway *gateway, size_t *out_count);

/**
 * The current 4-digit pairing code, for the supervisor notification.
 */
const char *raiju_gateway_code(raiju_gateway *gateway);

/**
 * Runs the routing table against a parsed request without a socket, applying
 * the auth gate. Returns the response status. Used by tests and by the
 * CivetWeb adapter.
 */
int raiju_gateway_handle(raiju_gateway *gateway, raiju_http_request *request, raiju_http_response *response);

/**
 * Starts the HTTP server bound to the configured address and port and sets
 * bound_port to the actual port. Returns false when the server can't start.
 */
bool raiju_gateway_start(raiju_gateway *gateway);

/**
 * Stops the server.
 */
void raiju_gateway_stop(raiju_gateway *gateway);

/**
 * Releases the gateway.
 */
void raiju_gateway_destroy(raiju_gateway *gateway);

#endif
