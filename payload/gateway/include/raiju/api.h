#ifndef RAIJU_API_H
#define RAIJU_API_H

#include "raiju/gateway.h"

struct raiju_api;

/**
 * Builds the /api state: the processor queue, the single worker thread that
 * owns it, and the route handlers for status, storage, folders, jobs, probe
 * and art. Appends them with raiju_gateway_add_route and stores the state in
 * gateway->api. Returns NULL on failure (the gateway then fails to start).
 * Every route but GET /api/status needs a device token.
 */
struct raiju_api *raiju_api_register(raiju_gateway *gateway);

/**
 * Stops the worker thread, joins it, closes the processor and releases the
 * state. Safe on NULL.
 */
void raiju_api_unregister(raiju_gateway *gateway);

/**
 * The engine's completion event from the WebSocket bridge: tells the worker
 * to look the job's path up through aria2 and run the pipeline. Unknown gids
 * are ignored.
 */
void raiju_api_engine_complete(raiju_gateway *gateway, const char *gid);

/**
 * The engine's failure event: moves the job to FAILED. Unknown gids are
 * ignored.
 */
void raiju_api_engine_error(raiju_gateway *gateway, const char *gid, const char *message);

#endif
