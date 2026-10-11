#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/fetch.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef RAIJU_FETCH_TLS
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

#define FETCH_HEADER_CAP (64u * 1024u)
#define FETCH_READ_CHUNK 16384

typedef struct {
	char host[256];
	char port[8];
	char path[2048];
	bool tls;
} fetch_url;

/**
 * Whether a URL component is safe to write into a request: no control
 * characters, no space and no DEL, so a client-supplied URI cannot inject a
 * header or a second request line.
 */
static bool url_component_valid(const char *text) {
	for (const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0'; cursor++) {
		if (*cursor <= 0x20 || *cursor == 0x7f) {
			return false;
		}
	}
	return true;
}

static bool parse_url(const char *url, fetch_url *out) {
	memset(out, 0, sizeof *out);
	const char *authority;
	if (strncmp(url, "http://", 7) == 0) {
		authority = url + 7;
		snprintf(out->port, sizeof out->port, "80");
	} else if (strncmp(url, "https://", 8) == 0) {
		authority = url + 8;
		out->tls = true;
		snprintf(out->port, sizeof out->port, "443");
	} else {
		return false;
	}
	const char *slash = strchr(authority, '/');
	size_t authority_length = slash == NULL ? strlen(authority) : (size_t)(slash - authority);
	const char *colon = memchr(authority, ':', authority_length);
	size_t host_length = colon == NULL ? authority_length : (size_t)(colon - authority);
	if (host_length == 0 || host_length >= sizeof out->host) {
		return false;
	}
	memcpy(out->host, authority, host_length);
	out->host[host_length] = '\0';
	if (colon != NULL && colon + 1 < authority + authority_length) {
		size_t port_length = (size_t)(authority + authority_length - colon - 1);
		if (port_length >= sizeof out->port) {
			return false;
		}
		memcpy(out->port, colon + 1, port_length);
		out->port[port_length] = '\0';
	}
	snprintf(out->path, sizeof out->path, "%s", slash == NULL ? "/" : slash);
	if (!url_component_valid(out->host) || !url_component_valid(out->path)) {
		return false;
	}
	for (const char *cursor = out->port; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9') {
			return false;
		}
	}
	return true;
}

typedef struct {
	int fd;
#ifdef RAIJU_FETCH_TLS
	SSL *ssl;
#endif
} fetch_stream;

#ifdef RAIJU_FETCH_TLS
static SSL_CTX *tls_context;

static void tls_context_init(void) {
	tls_context = SSL_CTX_new(TLS_client_method());
	if (tls_context != NULL) {
		SSL_CTX_set_default_verify_paths(tls_context);
		SSL_CTX_set_verify(tls_context, SSL_VERIFY_PEER, NULL);
	}
}
#endif

static int stream_write(fetch_stream *stream, const void *bytes, size_t length) {
	const char *data = bytes;
	size_t done = 0;
	while (done < length) {
		ssize_t written;
#ifdef RAIJU_FETCH_TLS
		if (stream->ssl != NULL) {
			int result = SSL_write(stream->ssl, data + done, (int)(length - done));
			if (result <= 0) {
				return -1;
			}
			written = result;
		} else {
#else
		{
#endif
			written = send(stream->fd, data + done, length - done, 0);
			if (written < 0 && errno == EINTR) {
				continue;
			}
			if (written <= 0) {
				return -1;
			}
		}
		done += (size_t)written;
	}
	return 0;
}

static ssize_t stream_read(fetch_stream *stream, void *buffer, size_t length) {
#ifdef RAIJU_FETCH_TLS
	if (stream->ssl != NULL) {
		int result = SSL_read(stream->ssl, buffer, (int)length);
		if (result > 0) {
			return result;
		}
		return SSL_get_error(stream->ssl, result) == SSL_ERROR_ZERO_RETURN ? 0 : -1;
	}
#endif
	for (;;) {
		ssize_t got = recv(stream->fd, buffer, length, 0);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		return got;
	}
}

static int connect_host(const fetch_url *url, int timeout_ms) {
	struct addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo *result = NULL;
	if (getaddrinfo(url->host, url->port, &hints, &result) != 0) {
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

static int open_stream(const fetch_url *url, int timeout_ms, fetch_stream *stream) {
	memset(stream, 0, sizeof *stream);
	stream->fd = connect_host(url, timeout_ms);
	if (stream->fd < 0) {
		return -1;
	}
	if (!url->tls) {
		return 0;
	}
#ifdef RAIJU_FETCH_TLS
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	pthread_once(&once, tls_context_init);
	if (tls_context == NULL) {
		close(stream->fd);
		return -1;
	}
	SSL *ssl = SSL_new(tls_context);
	if (ssl == NULL) {
		close(stream->fd);
		return -1;
	}
	SSL_set_fd(ssl, stream->fd);
	SSL_set_tlsext_host_name(ssl, url->host);
	X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
	X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
	X509_VERIFY_PARAM_set1_host(param, url->host, 0);
	if (SSL_connect(ssl) != 1) {
		SSL_free(ssl);
		close(stream->fd);
		return -1;
	}
	stream->ssl = ssl;
	return 0;
#else
	close(stream->fd);
	return -1;
#endif
}

static void close_stream(fetch_stream *stream) {
#ifdef RAIJU_FETCH_TLS
	if (stream->ssl != NULL) {
		SSL_shutdown(stream->ssl);
		SSL_free(stream->ssl);
	}
#endif
	if (stream->fd >= 0) {
		close(stream->fd);
	}
}

static char *find_header(const char *headers, const char *name, char *value, size_t value_capacity) {
	size_t name_length = strlen(name);
	const char *line = headers;
	while (line != NULL && *line != '\0') {
		const char *next = strstr(line, "\r\n");
		size_t line_length = next == NULL ? strlen(line) : (size_t)(next - line);
		if (line_length > name_length && strncasecmp(line, name, name_length) == 0 && line[name_length] == ':') {
			const char *cursor = line + name_length + 1;
			const char *limit = line + line_length;
			while (cursor < limit && (*cursor == ' ' || *cursor == '\t')) {
				cursor++;
			}
			size_t length = (size_t)(limit - cursor);
			if (length >= value_capacity) {
				length = value_capacity - 1;
			}
			memcpy(value, cursor, length);
			value[length] = '\0';
			return value;
		}
		if (next == NULL) {
			break;
		}
		line = next + 2;
	}
	return NULL;
}

static int parse_announced_length(const char *headers, raiju_fetch_response *response) {
	char value[128];
	if (find_header(headers, "Content-Range", value, sizeof value) != NULL) {
		snprintf(response->content_range, sizeof response->content_range, "%s", value);
		const char *slash = strchr(value, '/');
		if (slash != NULL && slash[1] != '\0' && slash[1] != '*') {
			response->content_length = (size_t)strtoull(slash + 1, NULL, 10);
		}
	}
	if (find_header(headers, "Content-Length", value, sizeof value) != NULL) {
		unsigned long long length = strtoull(value, NULL, 10);
		if (response->content_length == RAIJU_FETCH_NO_LENGTH) {
			response->content_length = (size_t)length;
		}
	}
	return 0;
}

/**
 * Milliseconds since started, so a transfer cannot be dribbled out forever
 * even though each read has its own timeout.
 */
static long elapsed_ms(const struct timespec *started) {
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0;
	}
	return (long)(now.tv_sec - started->tv_sec) * 1000 + (long)(now.tv_nsec - started->tv_nsec) / 1000000;
}

static int fetch_run(void *context, raiju_fetch_method method, const char *url, const char *range, size_t max_bytes,
                     int timeout_ms, raiju_fetch_response *response) {
	(void)context;
	memset(response, 0, sizeof *response);
	response->content_length = RAIJU_FETCH_NO_LENGTH;
	fetch_url parsed;
	if (!parse_url(url, &parsed)) {
		return -1;
	}
	fetch_stream stream;
	if (open_stream(&parsed, timeout_ms, &stream) != 0) {
		return -1;
	}
	char host_header[320];
	if (strcmp(parsed.port, "80") == 0 || strcmp(parsed.port, "443") == 0) {
		snprintf(host_header, sizeof host_header, "%s", parsed.host);
	} else {
		snprintf(host_header, sizeof host_header, "%s:%s", parsed.host, parsed.port);
	}
	char request[8192];
	int length;
	if (range == NULL) {
		length = snprintf(request, sizeof request,
		                  "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Raiju (+https://github.com/cherfia/raiju)\r\n"
		                  "Accept: */*\r\nConnection: close\r\n\r\n",
		                  method == RAIJU_FETCH_HEAD ? "HEAD" : "GET", parsed.path, host_header);
	} else {
		length = snprintf(request, sizeof request,
		                  "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Raiju (+https://github.com/cherfia/raiju)\r\n"
		                  "Accept: */*\r\nConnection: close\r\nRange: %s\r\n\r\n",
		                  method == RAIJU_FETCH_HEAD ? "HEAD" : "GET", parsed.path, host_header, range);
	}
	if (length < 0 || (size_t)length >= sizeof request || stream_write(&stream, request, (size_t)length) != 0) {
		close_stream(&stream);
		return -1;
	}
	size_t cap = max_bytes + FETCH_HEADER_CAP;
	char *raw = malloc(cap + 1);
	if (raw == NULL) {
		close_stream(&stream);
		return -1;
	}
	size_t total = 0;
	bool overran = false;
	struct timespec started;
	clock_gettime(CLOCK_MONOTONIC, &started);
	for (;;) {
		if (total >= cap || elapsed_ms(&started) > (long)timeout_ms * 2) {
			overran = true;
			break;
		}
		ssize_t got = stream_read(&stream, raw + total, cap - total);
		if (got < 0) {
			free(raw);
			close_stream(&stream);
			return -1;
		}
		if (got == 0) {
			break;
		}
		total += (size_t)got;
	}
	close_stream(&stream);
	raw[total] = '\0';
	char *separator = strstr(raw, "\r\n\r\n");
	if (separator == NULL) {
		free(raw);
		return -1;
	}
	char *headers_end = separator + 4;
	response->status = atoi(strchr(raw, ' ') != NULL ? strchr(raw, ' ') + 1 : "0");
	parse_announced_length(raw, response);
	size_t body_length = total - (size_t)(headers_end - raw);
	if (body_length > max_bytes) {
		response->truncated = true;
		free(raw);
		return -1;
	}
	if (overran) {
		response->truncated = true;
		free(raw);
		return -1;
	}
	char *body = malloc(body_length + 1);
	if (body == NULL) {
		free(raw);
		return -1;
	}
	memcpy(body, headers_end, body_length);
	body[body_length] = '\0';
	response->body = body;
	response->body_length = body_length;
	free(raw);
	return 0;
}

raiju_fetch_fn raiju_fetch_default(void) {
	return fetch_run;
}

void raiju_fetch_response_free(raiju_fetch_response *response) {
	free(response->body);
	response->body = NULL;
	response->body_length = 0;
}
