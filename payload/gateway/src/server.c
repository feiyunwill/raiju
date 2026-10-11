#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "civetweb.h"
#include "raiju/api.h"
#include "raiju/auth.h"
#include "raiju/gateway.h"
#include "raiju/strbuf.h"

#define RAIJU_WS_BRIDGE_REFS 2
#define RAIJU_GID_MAX 64

/**
 * Copies a JSON string value for key from a websocket message body into out.
 */
static bool ws_json_field(const char *body, size_t length, const char *key, char *out, size_t capacity) {
	const char *limit = body + length;
	char needle[64];
	int written = snprintf(needle, sizeof needle, "\"%s\":\"", key);
	if (written < 0 || (size_t)written >= sizeof needle) {
		return false;
	}
	size_t needle_length = strlen(needle);
	for (const char *cursor = body; cursor + needle_length <= limit; cursor++) {
		if (memcmp(cursor, needle, needle_length) != 0) {
			continue;
		}
		const char *value = cursor + needle_length;
		const char *end = value;
		while (end < limit && *end != '"') {
			end++;
		}
		if (end >= limit) {
			return false;
		}
		size_t value_length = (size_t)(end - value);
		if (value_length >= capacity) {
			value_length = capacity - 1;
		}
		memcpy(out, value, value_length);
		out[value_length] = '\0';
		return true;
	}
	return false;
}

/**
 * Parses an aria2 notification frame and hands it to the API worker: a
 * completed or failed download identified by its GID (the job ID).
 */
static bool bytes_contain(const char *haystack, size_t haystack_length, const char *needle) {
	size_t needle_length = strlen(needle);
	if (needle_length == 0 || haystack_length < needle_length) {
		return false;
	}
	for (size_t i = 0; i + needle_length <= haystack_length; i++) {
		if (memcmp(haystack + i, needle, needle_length) == 0) {
			return true;
		}
	}
	return false;
}

static void dispatch_notification(raiju_gateway *gateway, const char *data, size_t length) {
	bool complete = bytes_contain(data, length, "aria2.onDownloadComplete");
	bool error = !complete && bytes_contain(data, length, "aria2.onDownloadError");
	if (!complete && !error) {
		return;
	}
	char gid[RAIJU_GID_MAX + 1];
	if (!ws_json_field(data, length, "gid", gid, sizeof gid)) {
		return;
	}
	if (complete) {
		raiju_api_engine_complete(gateway, gid);
	} else {
		raiju_api_engine_error(gateway, gid, "download_error");
	}
}

typedef struct {
	raiju_gateway *gateway;
	struct mg_connection *client;
	struct mg_connection *upstream;
	pthread_mutex_t lock;
	int refs;
	bool client_closed;
} ws_bridge;

typedef struct {
	raiju_gateway *gateway;
	struct mg_context *context;
} server_state;

static const char *reason_phrase(int status) {
	switch (status) {
	case 200:
		return "OK";
	case 201:
		return "Created";
	case 400:
		return "Bad Request";
	case 401:
		return "Unauthorized";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 413:
		return "Payload Too Large";
	case 429:
		return "Too Many Requests";
	case 500:
		return "Internal Server Error";
	case 502:
		return "Bad Gateway";
	default:
		return "OK";
	}
}

static void bridge_unref(ws_bridge *bridge) {
	pthread_mutex_lock(&bridge->lock);
	int remaining = --bridge->refs;
	pthread_mutex_unlock(&bridge->lock);
	if (remaining == 0) {
		pthread_mutex_destroy(&bridge->lock);
		free(bridge);
	}
}

static bool rewrite_secret(const char *data, size_t length, const char *secret, raiju_strbuf *out) {
	size_t injected_length = 0;
	char *injected = raiju_proxy_inject_secret(data, length, secret, &injected_length);
	if (injected == NULL) {
		return false;
	}
	raiju_strbuf_append_bytes(out, injected, injected_length);
	free(injected);
	return true;
}

static int ws_client_data(struct mg_connection *conn, int bits, char *data, size_t length, void *user_data) {
	(void)conn;
	ws_bridge *bridge = user_data;
	pthread_mutex_lock(&bridge->lock);
	struct mg_connection *client = bridge->client;
	bool closed = bridge->client_closed;
	pthread_mutex_unlock(&bridge->lock);
	if (client != NULL && !closed) {
		mg_websocket_write(client, bits, data, length);
	}
	if ((bits & 0x0f) == MG_WEBSOCKET_OPCODE_TEXT) {
		dispatch_notification(bridge->gateway, data, length);
	}
	return 1;
}

static void ws_client_close(const struct mg_connection *conn, void *user_data) {
	(void)conn;
	ws_bridge *bridge = user_data;
	pthread_mutex_lock(&bridge->lock);
	struct mg_connection *client = bridge->client;
	bool client_closed = bridge->client_closed;
	bridge->upstream = NULL;
	pthread_mutex_unlock(&bridge->lock);
	if (client != NULL && !client_closed) {
		mg_close_connection(client);
	}
	bridge_unref(bridge);
}

static int ws_server_connect(const struct mg_connection *conn, void *cbdata) {
	raiju_gateway *gateway = cbdata;
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.cookie = mg_get_header(conn, "Cookie");
	request.authorization = mg_get_header(conn, "Authorization");
	char token[RAIJU_PAIR_TOKEN_HEX + 1];
	if (!raiju_auth_token(&request, token, sizeof token) || raiju_pairing_find(&gateway->pairing, token) == NULL) {
		return 1;
	}
	return 0;
}

static void ws_server_ready(struct mg_connection *conn, void *cbdata) {
	raiju_gateway *gateway = cbdata;
	ws_bridge *bridge = calloc(1, sizeof *bridge);
	if (bridge == NULL) {
		mg_close_connection(conn);
		return;
	}
	pthread_mutex_init(&bridge->lock, NULL);
	bridge->gateway = gateway;
	bridge->client = conn;
	bridge->refs = RAIJU_WS_BRIDGE_REFS;
	mg_set_user_connection_data(conn, bridge);
	char error[256];
	error[0] = '\0';
	struct mg_connection *upstream =
	    mg_connect_websocket_client(gateway->upstream.host, gateway->upstream.port, 0, error, sizeof error,
	                                gateway->upstream.path, NULL, ws_client_data, ws_client_close, bridge);
	if (upstream == NULL) {
		bridge->refs = 1;
		mg_close_connection(conn);
		return;
	}
	pthread_mutex_lock(&bridge->lock);
	bridge->upstream = upstream;
	pthread_mutex_unlock(&bridge->lock);
}

static int ws_server_data(struct mg_connection *conn, int bits, char *data, size_t length, void *cbdata) {
	raiju_gateway *gateway = cbdata;
	ws_bridge *bridge = mg_get_user_connection_data(conn);
	if (bridge == NULL) {
		return 1;
	}
	pthread_mutex_lock(&bridge->lock);
	struct mg_connection *upstream = bridge->upstream;
	pthread_mutex_unlock(&bridge->lock);
	if (upstream == NULL) {
		return 1;
	}
	if ((bits & 0x0f) == MG_WEBSOCKET_OPCODE_TEXT) {
		raiju_strbuf rewritten;
		raiju_strbuf_init(&rewritten);
		if (rewrite_secret(data, length, gateway->secret, &rewritten) && !rewritten.failed) {
			mg_websocket_client_write(upstream, MG_WEBSOCKET_OPCODE_TEXT, raiju_strbuf_cstr(&rewritten),
			                          rewritten.length);
		}
		raiju_strbuf_free(&rewritten);
	} else {
		mg_websocket_client_write(upstream, bits & 0x0f, data, length);
	}
	return 1;
}

static void ws_server_close(const struct mg_connection *conn, void *cbdata) {
	(void)cbdata;
	ws_bridge *bridge = mg_get_user_connection_data(conn);
	if (bridge == NULL) {
		return;
	}
	pthread_mutex_lock(&bridge->lock);
	bridge->client_closed = true;
	struct mg_connection *upstream = bridge->upstream;
	pthread_mutex_unlock(&bridge->lock);
	if (upstream != NULL) {
		mg_close_connection(upstream);
	}
	bridge_unref(bridge);
}

static bool header_contains(const struct mg_connection *conn, const char *name, const char *token) {
	const char *value = mg_get_header(conn, name);
	return value != NULL && strstr(value, token) != NULL;
}

static int begin_request(struct mg_connection *conn) {
	raiju_gateway *gateway = mg_get_user_context_data(conn);
	const struct mg_request_info *info = mg_get_request_info(conn);
	if (header_contains(conn, "Upgrade", "websocket")) {
		return 0;
	}
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = info->request_method;
	request.path = info->local_uri != NULL ? info->local_uri : info->request_uri;
	request.query = info->query_string;
	request.cookie = mg_get_header(conn, "Cookie");
	request.authorization = mg_get_header(conn, "Authorization");
	request.content_type = mg_get_header(conn, "Content-Type");

	raiju_http_response response;
	raiju_response_init(&response);

	unsigned char *body = NULL;
	bool too_large = false;
	if (info->content_length > 0) {
		if ((size_t)info->content_length > gateway->config.max_body_bytes) {
			too_large = true;
		} else {
			body = malloc((size_t)info->content_length + 1);
			if (body != NULL) {
				size_t total = 0;
				while (total < (size_t)info->content_length) {
					int got = mg_read(conn, body + total, (size_t)info->content_length - total);
					if (got <= 0) {
						break;
					}
					total += (size_t)got;
				}
				body[total] = '\0';
				request.body = body;
				request.body_length = total;
			}
		}
	}

	if (too_large) {
		raiju_response_error(&response, 413, "request body too large");
	} else {
		raiju_gateway_handle(gateway, &request, &response);
	}
	free(body);

	mg_printf(conn, "HTTP/1.1 %d %s\r\n", response.status, reason_phrase(response.status));
	for (size_t i = 0; i < response.header_count; i++) {
		mg_printf(conn, "%s\r\n", raiju_strbuf_cstr(&response.headers[i]));
	}
	mg_printf(conn, "Content-Length: %zu\r\n\r\n", response.body.length);
	if (response.body.length > 0) {
		mg_write(conn, response.body.data, response.body.length);
	}
	int status = response.status;
	raiju_response_free(&response);
	return status;
}

bool raiju_gateway_start(raiju_gateway *gateway) {
	server_state *state = calloc(1, sizeof *state);
	if (state == NULL) {
		return false;
	}
	state->gateway = gateway;
	mg_init_library(MG_FEATURES_DEFAULT);
	char listen[64];
	snprintf(listen, sizeof listen, "%s:%u", gateway->config.bind_address, (unsigned)gateway->config.port);
	const char *options[] = {
	    "listening_ports", listen, "num_threads", "8", "request_timeout_ms", "30000", NULL,
	};
	struct mg_callbacks callbacks;
	memset(&callbacks, 0, sizeof callbacks);
	callbacks.begin_request = begin_request;
	struct mg_context *context = mg_start(&callbacks, gateway, options);
	if (context == NULL) {
		free(state);
		return false;
	}
	mg_set_websocket_handler(context, "/jsonrpc", ws_server_connect, ws_server_ready, ws_server_data,
	                         ws_server_close, gateway);
	state->context = (struct mg_context *)context;
	gateway->context = state;
	struct mg_server_port ports[8];
	int count = mg_get_server_ports(context, 8, ports);
	gateway->bound_port = gateway->config.port;
	for (int i = 0; i < count; i++) {
		if (ports[i].is_ssl == 0 && ports[i].port > 0) {
			gateway->bound_port = (uint16_t)ports[i].port;
			break;
		}
	}
	return true;
}

void raiju_gateway_stop(raiju_gateway *gateway) {
	server_state *state = gateway->context;
	if (state == NULL) {
		return;
	}
	mg_stop((struct mg_context *)state->context);
	mg_exit_library();
	free(state);
	gateway->context = NULL;
}
