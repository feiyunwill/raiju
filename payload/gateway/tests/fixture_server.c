#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "fixture_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
	char path[64];
	unsigned char *bytes;
	size_t length;
	unsigned hits;
} fixture_file;

struct raiju_fixture_server {
	int listen_fd;
	uint16_t port;
	pthread_t thread;
	bool running;
	pthread_mutex_t lock;
	char jsonrpc[8192];
	size_t jsonrpc_length;
	fixture_file files[RAIJU_FIXTURE_MAX_FILES];
	size_t file_count;
	char last_range[64];
	char last_method[8];
	char last_host[128];
};

static int send_all(int fd, const void *bytes, size_t length) {
	const char *data = bytes;
	size_t done = 0;
	while (done < length) {
		ssize_t written = send(fd, data + done, length - done, 0);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			return -1;
		}
		done += (size_t)written;
	}
	return 0;
}

static const char *header_value(const char *request, size_t length, const char *name, char *out, size_t capacity) {
	const char *limit = request + length;
	size_t name_length = strlen(name);
	const char *line = request;
	while (line < limit) {
		const char *next = strstr(line, "\r\n");
		size_t line_length = next == NULL ? (size_t)(limit - line) : (size_t)(next - line);
		if (line_length > name_length && strncasecmp(line, name, name_length) == 0 && line[name_length] == ':') {
			const char *cursor = line + name_length + 1;
			const char *line_end = line + line_length;
			while (cursor < line_end && (*cursor == ' ' || *cursor == '\t')) {
				cursor++;
			}
			size_t value_length = (size_t)(line_end - cursor);
			if (value_length >= capacity) {
				value_length = capacity - 1;
			}
			memcpy(out, cursor, value_length);
			out[value_length] = '\0';
			return out;
		}
		if (next == NULL) {
			break;
		}
		line = next + 2;
	}
	return NULL;
}

static fixture_file *find_file(raiju_fixture_server *server, const char *path) {
	for (size_t i = 0; i < server->file_count; i++) {
		if (strcmp(server->files[i].path, path) == 0) {
			return &server->files[i];
		}
	}
	return NULL;
}

static void parse_range(const char *value, size_t total, size_t *start, size_t *end) {
	*start = 0;
	*end = total == 0 ? 0 : total - 1;
	if (value == NULL || strncmp(value, "bytes=", 6) != 0) {
		return;
	}
	const char *cursor = value + 6;
	if (*cursor == '-') {
		size_t suffix = (size_t)strtoul(cursor + 1, NULL, 10);
		*start = suffix >= total ? 0 : total - suffix;
		*end = total == 0 ? 0 : total - 1;
		return;
	}
	*start = (size_t)strtoul(cursor, NULL, 10);
	const char *dash = strchr(cursor, '-');
	*end = dash != NULL && dash[1] != '\0' ? (size_t)strtoul(dash + 1, NULL, 10) : total - 1;
	if (*end >= total) {
		*end = total == 0 ? 0 : total - 1;
	}
}

static void serve_file(int fd, fixture_file *file, const char *method, const char *range) {
	size_t start;
	size_t end;
	bool ranged = range != NULL && strncmp(range, "bytes=", 6) == 0;
	parse_range(range, file->length, &start, &end);
	size_t content_length = end >= start ? end - start + 1 : 0;
	char header[512];
	if (ranged) {
		int length = snprintf(header, sizeof header,
		                      "HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\n"
		                      "Accept-Ranges: bytes\r\nContent-Range: bytes %zu-%zu/%zu\r\n"
		                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
		                      start, end, file->length, content_length);
		send_all(fd, header, (size_t)length);
	} else {
		int length = snprintf(header, sizeof header,
		                      "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
		                      "Accept-Ranges: bytes\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		                      file->length);
		send_all(fd, header, (size_t)length);
	}
	if (strcmp(method, "HEAD") != 0 && content_length > 0) {
		send_all(fd, file->bytes + start, content_length);
	}
}

static void handle_connection(raiju_fixture_server *server, int fd) {
	char request[16384];
	size_t total = 0;
	size_t required = 0;
	while (total + 1 < sizeof request) {
		ssize_t got = recv(fd, request + total, sizeof request - 1 - total, 0);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			break;
		}
		total += (size_t)got;
		request[total] = '\0';
		const char *separator = strstr(request, "\r\n\r\n");
		if (separator == NULL) {
			continue;
		}
		if (required == 0) {
			char length[32] = "";
			header_value(request, total, "Content-Length", length, sizeof length);
			required = (size_t)(separator + 4 - request) + (size_t)strtoul(length, NULL, 10);
		}
		if (total >= required) {
			break;
		}
	}
	request[total] = '\0';
	char method[8] = "";
	char path[512] = "";
	sscanf(request, "%7s %511s", method, path);
	char range[64] = "";
	char host[128] = "";
	header_value(request, total, "Range", range, sizeof range);
	header_value(request, total, "Host", host, sizeof host);
	pthread_mutex_lock(&server->lock);
	snprintf(server->last_method, sizeof server->last_method, "%s", method);
	snprintf(server->last_range, sizeof server->last_range, "%s", range);
	snprintf(server->last_host, sizeof server->last_host, "%s", host);
	fixture_file *file = strcmp(path, "/jsonrpc") == 0 ? NULL : find_file(server, path);
	if (file != NULL) {
		file->hits++;
	}
	pthread_mutex_unlock(&server->lock);
	if (strcmp(path, "/jsonrpc") == 0) {
		pthread_mutex_lock(&server->lock);
		size_t length = server->jsonrpc_length;
		char body[8192];
		memcpy(body, server->jsonrpc, length);
		pthread_mutex_unlock(&server->lock);
		char header[256];
		int written = snprintf(header, sizeof header,
		                       "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
		                       "Connection: close\r\n\r\n",
		                       length);
		send_all(fd, header, (size_t)written);
		send_all(fd, body, length);
		return;
	}
	if (file == NULL) {
		const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
		send_all(fd, not_found, strlen(not_found));
		return;
	}
	serve_file(fd, file, method, range[0] != '\0' ? range : NULL);
}

static void *server_thread(void *argument) {
	raiju_fixture_server *server = argument;
	while (server->running) {
		int fd = accept(server->listen_fd, NULL, NULL);
		if (fd < 0) {
			continue;
		}
		handle_connection(server, fd);
		close(fd);
	}
	return NULL;
}

raiju_fixture_server *raiju_fixture_server_start(void) {
	raiju_fixture_server *server = calloc(1, sizeof *server);
	if (server == NULL) {
		return NULL;
	}
	pthread_mutex_init(&server->lock, NULL);
	server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server->listen_fd < 0) {
		free(server);
		return NULL;
	}
	int reuse = 1;
	setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
	struct sockaddr_in address;
	memset(&address, 0, sizeof address);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	if (bind(server->listen_fd, (struct sockaddr *)&address, sizeof address) != 0 ||
	    listen(server->listen_fd, 4) != 0) {
		close(server->listen_fd);
		free(server);
		return NULL;
	}
	socklen_t length = sizeof address;
	getsockname(server->listen_fd, (struct sockaddr *)&address, &length);
	server->port = ntohs(address.sin_port);
	server->running = true;
	if (pthread_create(&server->thread, NULL, server_thread, server) != 0) {
		close(server->listen_fd);
		free(server);
		return NULL;
	}
	return server;
}

void raiju_fixture_server_stop(raiju_fixture_server *server) {
	if (server == NULL) {
		return;
	}
	server->running = false;
	shutdown(server->listen_fd, SHUT_RDWR);
	close(server->listen_fd);
	pthread_join(server->thread, NULL);
	for (size_t i = 0; i < server->file_count; i++) {
		free(server->files[i].bytes);
	}
	pthread_mutex_destroy(&server->lock);
	free(server);
}

uint16_t raiju_fixture_server_port(const raiju_fixture_server *server) {
	return server->port;
}

void raiju_fixture_server_set_jsonrpc(raiju_fixture_server *server, const char *body) {
	pthread_mutex_lock(&server->lock);
	server->jsonrpc_length = body == NULL ? 0 : strlen(body);
	if (server->jsonrpc_length >= sizeof server->jsonrpc) {
		server->jsonrpc_length = 0;
	}
	if (server->jsonrpc_length > 0) {
		memcpy(server->jsonrpc, body, server->jsonrpc_length);
	}
	pthread_mutex_unlock(&server->lock);
}

bool raiju_fixture_server_add_file(raiju_fixture_server *server, const char *path, const void *bytes, size_t length) {
	if (server->file_count >= RAIJU_FIXTURE_MAX_FILES) {
		return false;
	}
	fixture_file *file = &server->files[server->file_count];
	snprintf(file->path, sizeof file->path, "%s", path);
	file->bytes = malloc(length == 0 ? 1 : length);
	if (file->bytes == NULL) {
		return false;
	}
	memcpy(file->bytes, bytes, length);
	file->length = length;
	file->hits = 0;
	server->file_count++;
	return true;
}

const char *raiju_fixture_server_last_range(const raiju_fixture_server *server) {
	return server->last_range;
}

const char *raiju_fixture_server_last_method(const raiju_fixture_server *server) {
	return server->last_method;
}

const char *raiju_fixture_server_last_host(const raiju_fixture_server *server) {
	return server->last_host;
}

unsigned raiju_fixture_server_hits(const raiju_fixture_server *server, const char *path) {
	for (size_t i = 0; i < server->file_count; i++) {
		if (strcmp(server->files[i].path, path) == 0) {
			return server->files[i].hits;
		}
	}
	return 0;
}

void raiju_fixture_server_reset(raiju_fixture_server *server) {
	pthread_mutex_lock(&server->lock);
	for (size_t i = 0; i < server->file_count; i++) {
		server->files[i].hits = 0;
	}
	server->last_range[0] = '\0';
	server->last_method[0] = '\0';
	server->last_host[0] = '\0';
	pthread_mutex_unlock(&server->lock);
}
