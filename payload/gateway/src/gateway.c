#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/gateway.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "raiju/assets.h"
#include "raiju/auth.h"
#include "raiju/fsutil.h"

#define RAIJU_STATIC_MAX (16u * 1024u * 1024u)

static const char *gateway_version(void) {
	return "0.0.0";
}

static const char *find_needle(const char *start, const char *limit, const char *needle, size_t needle_length) {
	for (const char *cursor = start; cursor + needle_length <= limit; cursor++) {
		if (memcmp(cursor, needle, needle_length) == 0) {
			return cursor;
		}
	}
	return NULL;
}

static bool json_get_string(const char *body, size_t length, const char *key, char *out, size_t capacity) {
	char needle[64];
	snprintf(needle, sizeof needle, "\"%s\"", key);
	const char *cursor = body;
	const char *limit = body + length;
	const char *found = NULL;
	size_t needle_length = strlen(needle);
	while (cursor < limit && (cursor = find_needle(cursor, limit, needle, needle_length)) != NULL) {
		const char *after = cursor + needle_length;
		while (after < limit && (*after == ' ' || *after == ':')) {
			after++;
		}
		if (after < limit && *after == '"') {
			found = after + 1;
			break;
		}
		cursor = after;
	}
	if (found == NULL) {
		return false;
	}
	const char *end = found;
	while (end < limit && *end != '"') {
		if (*end == '\\') {
			end++;
		}
		end++;
	}
	if (end >= limit) {
		return false;
	}
	size_t value_length = (size_t)(end - found);
	if (value_length >= capacity) {
		value_length = capacity - 1;
	}
	memcpy(out, found, value_length);
	out[value_length] = '\0';
	return true;
}

static bool serve_from_www(raiju_gateway *gateway, const char *path, raiju_http_response *response) {
	char candidate[RAIJU_PATH_MAX];
	int written = strcmp(path, "/") == 0
	                  ? snprintf(candidate, sizeof candidate, "%s/index.html", gateway->www_dir)
	                  : snprintf(candidate, sizeof candidate, "%s%s", gateway->www_dir, path);
	if (written < 0 || (size_t)written >= sizeof candidate) {
		return false;
	}
	char resolved[PATH_MAX];
	if (realpath(candidate, resolved) == NULL) {
		return false;
	}
	char root[PATH_MAX];
	if (realpath(gateway->www_dir, root) == NULL) {
		return false;
	}
	size_t root_length = strlen(root);
	if (strncmp(resolved, root, root_length) != 0 ||
	    (resolved[root_length] != '\0' && resolved[root_length] != '/')) {
		return false;
	}
	size_t length = 0;
	char *bytes = raiju_read_file(resolved, RAIJU_STATIC_MAX, &length);
	if (bytes == NULL) {
		return false;
	}
	raiju_response_set_status(response, 200);
	raiju_response_set_header(response, "Content-Type", raiju_assets_mime(resolved));
	raiju_response_write(response, bytes, length);
	free(bytes);
	return true;
}

static bool serve_static(raiju_gateway *gateway, const raiju_http_request *request, raiju_http_response *response) {
	if (gateway->www_dir[0] != '\0' && serve_from_www(gateway, request->path, response)) {
		return true;
	}
	const raiju_asset *asset = raiju_assets_find(request->path);
	if (asset == NULL) {
		return false;
	}
	raiju_response_set_status(response, 200);
	raiju_response_set_header(response, "Content-Type", raiju_assets_mime(asset->path));
	raiju_response_write(response, asset->bytes, asset->length);
	return true;
}

static int handle_status(const raiju_http_request *request, raiju_http_response *response, void *context) {
	(void)request;
	raiju_gateway *gateway = context;
	char address[64];
	gateway->config.platform->interface_ip(gateway->config.platform->ctx, address, sizeof address);
	uint64_t uptime = (uint64_t)time(NULL) - gateway->started_at;
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	raiju_strbuf_appendf(&document, "{\"name\":\"raiju\",\"version\":\"%s\",\"uptime\":%llu,"
	                               "\"firmware\":\"unknown\",\"platform\":\"%s\",\"ip\":\"%s\","
	                               "\"engine\":\"unknown\"}",
	                     gateway_version(), (unsigned long long)uptime,
	                     gateway->config.platform == raiju_platform_host() ? "host" : "ps5", address);
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

static int handle_pair(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_gateway *gateway = context;
	char code[RAIJU_PAIR_CODE_DIGITS + 8];
	char label[RAIJU_DEVICE_LABEL_MAX];
	if (!json_get_string((const char *)request->body, request->body_length, "code", code, sizeof code)) {
		raiju_response_error(response, 400, "missing code");
		return 400;
	}
	if (!json_get_string((const char *)request->body, request->body_length, "label", label, sizeof label)) {
		snprintf(label, sizeof label, "device");
	}
	uint64_t now = (uint64_t)time(NULL);
	raiju_pair_status status = raiju_pairing_verify(&gateway->pairing, code, now);
	if (status == RAIJU_PAIR_RATE_LIMITED) {
		raiju_response_error(response, 429, "too many attempts");
		return 429;
	}
	if (status != RAIJU_PAIR_OK) {
		raiju_response_error(response, 401, "wrong code");
		return 401;
	}
	raiju_device device;
	raiju_pair_status issued = raiju_pairing_issue(&gateway->pairing, label, &device);
	if (issued == RAIJU_PAIR_INVALID_LABEL) {
		raiju_response_error(response, 400, "invalid label");
		return 400;
	}
	if (issued != RAIJU_PAIR_OK) {
		raiju_response_error(response, 500, "could not issue token");
		return 500;
	}
	char cookie[RAIJU_PAIR_TOKEN_HEX + 128];
	raiju_auth_set_cookie(device.token, cookie, sizeof cookie);
	raiju_response_set_header(response, "Set-Cookie", cookie);
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	raiju_strbuf_append(&document, "{\"id\":");
	raiju_strbuf_append_json_string(&document, device.id);
	raiju_strbuf_append(&document, ",\"token\":");
	raiju_strbuf_append_json_string(&document, device.token);
	raiju_strbuf_append(&document, ",\"label\":");
	raiju_strbuf_append_json_string(&document, device.label);
	raiju_strbuf_append_char(&document, '}');
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

static int handle_devices_list(const raiju_http_request *request, raiju_http_response *response, void *context) {
	(void)request;
	raiju_gateway *gateway = context;
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	raiju_strbuf_append(&document, "{\"devices\":[");
	for (size_t i = 0; i < gateway->pairing.device_count; i++) {
		const raiju_device *device = &gateway->pairing.devices[i];
		if (i > 0) {
			raiju_strbuf_append_char(&document, ',');
		}
		raiju_strbuf_append(&document, "{\"id\":");
		raiju_strbuf_append_json_string(&document, device->id);
		raiju_strbuf_append(&document, ",\"label\":");
		raiju_strbuf_append_json_string(&document, device->label);
		raiju_strbuf_append_char(&document, '}');
	}
	raiju_strbuf_append(&document, "]}");
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

static int handle_device_delete(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_gateway *gateway = context;
	if (request->param_count < 1) {
		raiju_response_error(response, 400, "missing device id");
		return 400;
	}
	raiju_pair_status status = raiju_pairing_revoke(&gateway->pairing, request->params[0]);
	if (status == RAIJU_PAIR_NOT_FOUND) {
		raiju_response_error(response, 404, "unknown device");
		return 404;
	}
	if (status != RAIJU_PAIR_OK) {
		raiju_response_error(response, 500, "could not revoke");
		return 500;
	}
	raiju_response_json(response, 200, "{\"revoked\":true}");
	return 200;
}

static int handle_jsonrpc(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_gateway *gateway = context;
	if (request->body == NULL || request->body_length == 0) {
		raiju_response_error(response, 400, "empty body");
		return 400;
	}
	size_t injected_length = 0;
	char *injected = raiju_proxy_inject_secret((const char *)request->body, request->body_length, gateway->secret,
	                                           &injected_length);
	if (injected == NULL) {
		raiju_response_error(response, 500, "could not build request");
		return 500;
	}
	raiju_upstream_response upstream;
	int result = raiju_upstream_post(&gateway->upstream, (const unsigned char *)injected, injected_length, 15000,
	                                 &upstream);
	free(injected);
	if (result != 0) {
		raiju_response_error(response, 502, "engine unreachable");
		return 502;
	}
	raiju_response_set_status(response, upstream.status);
	if (upstream.content_type[0] != '\0') {
		raiju_response_set_header(response, "Content-Type", upstream.content_type);
	} else {
		raiju_response_set_header(response, "Content-Type", "application/json");
	}
	raiju_response_write(response, raiju_strbuf_cstr(&upstream.body), upstream.body.length);
	int status = upstream.status;
	raiju_upstream_response_free(&upstream);
	return status;
}

static bool register_default_routes(raiju_gateway *gateway) {
	const raiju_route routes[] = {
	    {"POST", "/api/pair", handle_pair, gateway, false},
	    {"GET", "/api/status", handle_status, gateway, false},
	    {"GET", "/api/devices", handle_devices_list, gateway, true},
	    {"DELETE", "/api/devices/:id", handle_device_delete, gateway, true},
	    {"POST", "/jsonrpc", handle_jsonrpc, gateway, true},
	};
	for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
		if (!raiju_gateway_add_route(gateway, &routes[i])) {
			return false;
		}
	}
	return true;
}

raiju_gateway *raiju_gateway_create(const raiju_gateway_config *config) {
	if (config == NULL || config->platform == NULL || config->data_dir == NULL || config->aria2_url == NULL) {
		return NULL;
	}
	raiju_gateway *gateway = calloc(1, sizeof *gateway);
	if (gateway == NULL) {
		return NULL;
	}
	gateway->config = *config;
	gateway->config.bind_address = config->bind_address == NULL ? "0.0.0.0" : config->bind_address;
	gateway->config.max_body_bytes = config->max_body_bytes != 0 ? config->max_body_bytes : 8u * 1024u * 1024u;
	gateway->allowlist = raiju_allowlist_default();
	if (!raiju_upstream_parse(config->aria2_url, &gateway->upstream)) {
		free(gateway);
		return NULL;
	}
	if (config->secret != NULL) {
		snprintf(gateway->secret, sizeof gateway->secret, "%s", config->secret);
	}
	if (config->www_dir != NULL) {
		snprintf(gateway->www_dir, sizeof gateway->www_dir, "%s", config->www_dir);
	} else {
		snprintf(gateway->www_dir, sizeof gateway->www_dir, "%s/www", config->data_dir);
	}
	gateway->config.www_dir = gateway->www_dir;
	char devices_path[RAIJU_PATH_MAX];
	snprintf(devices_path, sizeof devices_path, "%s/session/devices.json", config->data_dir);
	gateway->started_at = (uint64_t)time(NULL);
	if (!raiju_pairing_init(&gateway->pairing, config->platform, devices_path) || !register_default_routes(gateway)) {
		free(gateway);
		return NULL;
	}
	return gateway;
}

bool raiju_gateway_add_route(raiju_gateway *gateway, const raiju_route *route) {
	if (gateway->route_count >= RAIJU_GATEWAY_MAX_ROUTES) {
		return false;
	}
	gateway->routes[gateway->route_count++] = *route;
	return true;
}

const raiju_route *raiju_gateway_routes(raiju_gateway *gateway, size_t *out_count) {
	*out_count = gateway->route_count;
	return gateway->routes;
}

const char *raiju_gateway_code(raiju_gateway *gateway) {
	return gateway->pairing.code;
}

int raiju_gateway_handle(raiju_gateway *gateway, raiju_http_request *request, raiju_http_response *response) {
	bool requires_auth = true;
	size_t index = raiju_route_match(gateway->routes, gateway->route_count, request, &requires_auth);
	if (index >= gateway->route_count) {
		if (strcmp(request->method, "GET") == 0 && serve_static(gateway, request, response)) {
			return response->status;
		}
		raiju_response_error(response, 404, "not found");
		return 404;
	}
	if (requires_auth) {
		char token[RAIJU_PAIR_TOKEN_HEX + 1];
		if (!raiju_auth_token(request, token, sizeof token) ||
		    raiju_pairing_find(&gateway->pairing, token) == NULL) {
			raiju_response_error(response, 401, "unauthorized");
			return 401;
		}
	}
	return gateway->routes[index].handler(request, response, gateway->routes[index].context);
}

void raiju_gateway_destroy(raiju_gateway *gateway) {
	free(gateway);
}
