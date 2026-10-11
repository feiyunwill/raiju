#include "raiju/http.h"

#include <stdio.h>
#include <string.h>

void raiju_response_init(raiju_http_response *response) {
	memset(response, 0, sizeof *response);
	response->status = 200;
	raiju_strbuf_init(&response->body);
	for (size_t i = 0; i < RAIJU_HTTP_MAX_PARAMS; i++) {
		raiju_strbuf_init(&response->headers[i]);
	}
}

void raiju_response_free(raiju_http_response *response) {
	raiju_strbuf_free(&response->body);
	for (size_t i = 0; i < response->header_count && i < RAIJU_HTTP_MAX_PARAMS; i++) {
		raiju_strbuf_free(&response->headers[i]);
	}
	response->header_count = 0;
}

void raiju_response_set_status(raiju_http_response *response, int status) {
	response->status = status;
}

void raiju_response_set_header(raiju_http_response *response, const char *name, const char *value) {
	if (response->header_count >= RAIJU_HTTP_MAX_PARAMS) {
		return;
	}
	raiju_strbuf *header = &response->headers[response->header_count];
	raiju_strbuf_append(header, name);
	raiju_strbuf_append(header, ": ");
	raiju_strbuf_append(header, value);
	response->header_count++;
}

void raiju_response_write(raiju_http_response *response, const void *bytes, size_t length) {
	raiju_strbuf_append_bytes(&response->body, bytes, length);
}

void raiju_response_write_str(raiju_http_response *response, const char *text) {
	raiju_response_write(response, text, strlen(text));
}

void raiju_response_json(raiju_http_response *response, int status, const char *json) {
	raiju_response_set_status(response, status);
	raiju_response_set_header(response, "Content-Type", "application/json");
	raiju_response_write_str(response, json);
}

void raiju_response_error(raiju_http_response *response, int status, const char *message) {
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	raiju_strbuf_append(&document, "{\"error\":\"");
	for (const char *cursor = message; *cursor != '\0'; cursor++) {
		char c = *cursor;
		if (c == '"' || c == '\\') {
			raiju_strbuf_append_char(&document, '\\');
			raiju_strbuf_append_char(&document, c);
		} else if (c == '\n') {
			raiju_strbuf_append(&document, "\\n");
		} else {
			raiju_strbuf_append_char(&document, c);
		}
	}
	raiju_strbuf_append(&document, "\"}");
	raiju_response_json(response, status, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
}

static bool pattern_matches(const char *pattern, const char *path, raiju_http_request *request) {
	size_t pattern_length = strlen(pattern);
	if (pattern_length >= 1 && pattern[pattern_length - 1] == '*') {
		size_t prefix = pattern_length - 1;
		return strncmp(pattern, path, prefix) == 0;
	}
	const char *p = pattern;
	const char *r = path;
	for (;;) {
		const char *p_slash = strchr(p, '/');
		const char *r_slash = strchr(r, '/');
		size_t p_len = p_slash == NULL ? strlen(p) : (size_t)(p_slash - p);
		size_t r_len = r_slash == NULL ? strlen(r) : (size_t)(r_slash - r);
		bool placeholder = p_len >= 1 && p[0] == ':';
		if (placeholder) {
			if (request->param_count >= RAIJU_HTTP_MAX_PARAMS || r_len >= RAIJU_HTTP_PARAM_MAX) {
				return false;
			}
			char *slot = request->param_values[request->param_count];
			memcpy(slot, r, r_len);
			slot[r_len] = '\0';
			request->params[request->param_count] = slot;
			request->param_count++;
		} else if (r_len != p_len || strncmp(p, r, p_len) != 0) {
			return false;
		}
		bool p_done = p_slash == NULL;
		bool r_done = r_slash == NULL;
		if (p_done && r_done) {
			return true;
		}
		if (p_done != r_done) {
			return false;
		}
		p = p_slash + 1;
		r = r_slash + 1;
	}
}

size_t raiju_route_match(const raiju_route *routes, size_t count, raiju_http_request *request,
                         bool *out_requires_auth) {
	*out_requires_auth = true;
	for (size_t i = 0; i < count; i++) {
		const raiju_route *route = &routes[i];
		if (strcmp(route->method, request->method) != 0) {
			continue;
		}
		request->param_count = 0;
		if (!pattern_matches(route->pattern, request->path, request)) {
			continue;
		}
		*out_requires_auth = route->requires_auth;
		return i;
	}
	return count;
}

int raiju_route_dispatch(const raiju_route *routes, size_t count, raiju_http_request *request,
                         raiju_http_response *response, bool *out_requires_auth, bool *out_matched) {
	bool requires_auth = true;
	size_t index = raiju_route_match(routes, count, request, &requires_auth);
	*out_requires_auth = requires_auth;
	*out_matched = index < count;
	if (index >= count) {
		raiju_response_error(response, 404, "not found");
		return 404;
	}
	return routes[index].handler(request, response, routes[index].context);
}
