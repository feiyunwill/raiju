#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/proxy.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

bool raiju_upstream_parse(const char *url, raiju_upstream *out) {
	memset(out, 0, sizeof *out);
	const char *prefix = "http://";
	size_t prefix_length = strlen(prefix);
	if (strncmp(url, prefix, prefix_length) != 0) {
		return false;
	}
	const char *authority = url + prefix_length;
	const char *path = strchr(authority, '/');
	size_t authority_length = path == NULL ? strlen(authority) : (size_t)(path - authority);
	const char *colon = memchr(authority, ':', authority_length);
	if (colon == NULL) {
		return false;
	}
	size_t host_length = (size_t)(colon - authority);
	if (host_length == 0 || host_length >= sizeof out->host) {
		return false;
	}
	memcpy(out->host, authority, host_length);
	out->host[host_length] = '\0';
	const char *port_start = colon + 1;
	const char *port_end = authority + authority_length;
	if (port_start >= port_end) {
		return false;
	}
	unsigned long port = 0;
	for (const char *cursor = port_start; cursor < port_end; cursor++) {
		if (*cursor < '0' || *cursor > '9') {
			return false;
		}
		port = port * 10 + (unsigned long)(*cursor - '0');
		if (port > 65535) {
			return false;
		}
	}
	out->port = (uint16_t)port;
	snprintf(out->path, sizeof out->path, "%s", path == NULL ? "/jsonrpc" : path);
	return true;
}

static char *inject_into_params(const char *body, size_t length, const char *secret, const char *params) {
	const char *bracket = params;
	while (*bracket != '\0' && *bracket != '[') {
		bracket++;
	}
	raiju_strbuf buffer;
	raiju_strbuf_init(&buffer);
	if (*bracket == '[') {
		size_t head = (size_t)(bracket - body) + 1;
		raiju_strbuf_append_bytes(&buffer, body, head);
		const char *rest = bracket + 1;
		while (*rest == ' ' || *rest == '\n' || *rest == '\r' || *rest == '\t') {
			rest++;
		}
		raiju_strbuf_appendf(&buffer, "\"token:%s\"", secret);
		if (*rest != ']') {
			raiju_strbuf_append_char(&buffer, ',');
		}
		raiju_strbuf_append(&buffer, rest);
	} else {
		raiju_strbuf_append_bytes(&buffer, body, length);
		raiju_strbuf_appendf(&buffer, ",\"params\":[\"token:%s\"]", secret);
	}
	if (buffer.failed) {
		raiju_strbuf_free(&buffer);
		return NULL;
	}
	return strdup(raiju_strbuf_cstr(&buffer));
}

char *raiju_proxy_inject_secret(const char *body, size_t length, const char *secret, size_t *out_length) {
	if (strstr(body, "\"token:") != NULL) {
		char *copy = malloc(length + 1);
		if (copy == NULL) {
			return NULL;
		}
		memcpy(copy, body, length);
		copy[length] = '\0';
		if (out_length != NULL) {
			*out_length = length;
		}
		return copy;
	}
	const char *params = strstr(body, "\"params\"");
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	if (params != NULL) {
		char *result = inject_into_params(body, length, secret, params);
		if (result == NULL) {
			return NULL;
		}
		if (out_length != NULL) {
			*out_length = strlen(result);
		}
		return result;
	}
	const char *close = strrchr(body, '}');
	if (close == NULL) {
		return NULL;
	}
	raiju_strbuf_append_bytes(&document, body, (size_t)(close - body));
	raiju_strbuf_appendf(&document, ",\"params\":[\"token:%s\"]}", secret);
	if (document.failed) {
		raiju_strbuf_free(&document);
		return NULL;
	}
	if (out_length != NULL) {
		*out_length = document.length;
	}
	char *result = strdup(raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return result;
}

static int connect_upstream(const raiju_upstream *upstream, int timeout_ms) {
	char port_text[8];
	snprintf(port_text, sizeof port_text, "%u", (unsigned)upstream->port);
	struct addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo *result = NULL;
	if (getaddrinfo(upstream->host, port_text, &hints, &result) != 0) {
		return -1;
	}
	int fd = -1;
	for (struct addrinfo *entry = result; entry != NULL; entry = entry->ai_next) {
		fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
		if (fd < 0) {
			continue;
		}
		struct timeval timeout;
		timeout.tv_sec = timeout_ms / 1000;
		timeout.tv_usec = (timeout_ms % 1000) * 1000;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
		if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(result);
	return fd;
}

static int send_all(int fd, const void *bytes, size_t length) {
	const char *data = bytes;
	while (length > 0) {
		ssize_t written = send(fd, data, length, 0);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			return -1;
		}
		data += written;
		length -= (size_t)written;
	}
	return 0;
}

static int parse_status(const char *response, int *status, char *content_type, size_t content_type_capacity) {
	const char *space = strchr(response, ' ');
	if (space == NULL) {
		return -1;
	}
	*status = atoi(space + 1);
	const char *header = strstr(response, "\r\nContent-Type: ");
	if (header != NULL) {
		header += strlen("\r\nContent-Type: ");
		const char *end = strstr(header, "\r\n");
		if (end != NULL) {
			size_t length = (size_t)(end - header);
			if (length >= content_type_capacity) {
				length = content_type_capacity - 1;
			}
			memcpy(content_type, header, length);
			content_type[length] = '\0';
		}
	}
	return 0;
}

int raiju_upstream_post(const raiju_upstream *upstream, const unsigned char *body, size_t length, int timeout_ms,
                        raiju_upstream_response *response) {
	memset(response, 0, sizeof *response);
	raiju_strbuf_init(&response->body);
	int fd = connect_upstream(upstream, timeout_ms);
	if (fd < 0) {
		response->transport_error = -1;
		return -1;
	}
	raiju_strbuf request;
	raiju_strbuf_init(&request);
	raiju_strbuf_appendf(&request, "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: application/json\r\n"
	                               "Content-Length: %zu\r\nConnection: close\r\n\r\n",
	                     upstream->path, upstream->host, (unsigned)upstream->port, length);
	if (request.failed || send_all(fd, raiju_strbuf_cstr(&request), request.length) != 0 ||
	    send_all(fd, body, length) != 0) {
		raiju_strbuf_free(&request);
		close(fd);
		response->transport_error = -1;
		return -1;
	}
	raiju_strbuf_free(&request);
	char chunk[4096];
	for (;;) {
		ssize_t got = recv(fd, chunk, sizeof chunk, 0);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got < 0) {
			close(fd);
			response->transport_error = -1;
			return -1;
		}
		if (got == 0) {
			break;
		}
		raiju_strbuf_append_bytes(&response->body, chunk, (size_t)got);
		if (response->body.failed) {
			close(fd);
			response->transport_error = -1;
			return -1;
		}
	}
	close(fd);
	char *raw = response->body.data;
	const char *separator = raw == NULL ? NULL : strstr(raw, "\r\n\r\n");
	if (separator == NULL) {
		response->transport_error = -1;
		return -1;
	}
	parse_status(raw, &response->status, response->content_type, sizeof response->content_type);
	size_t header_length = (size_t)(separator - raw) + 4;
	size_t body_length = response->body.length - header_length;
	memmove(raw, raw + header_length, body_length);
	raw[body_length] = '\0';
	response->body.length = body_length;
	return 0;
}

void raiju_upstream_response_free(raiju_upstream_response *response) {
	raiju_strbuf_free(&response->body);
}
