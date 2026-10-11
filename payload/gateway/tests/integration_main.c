#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "raiju/engine_options.h"
#include "raiju/fsutil.h"
#include "raiju/gateway.h"
#include "raiju/platform.h"
#include "raiju/proxy.h"

static int integration_checks = 0;
static int integration_failures = 0;

#define CHECK(condition)                                                                   \
	do {                                                                                   \
		integration_checks++;                                                              \
		if (!(condition)) {                                                                \
			integration_failures++;                                                        \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
		}                                                                                  \
	} while (0)

#define CHECK_INT(actual, expected)                                                             \
	do {                                                                                        \
		long long check_actual_ = (long long)(actual);                                          \
		long long check_expected_ = (long long)(expected);                                      \
		integration_checks++;                                                                   \
		if (check_actual_ != check_expected_) {                                                 \
			integration_failures++;                                                             \
			fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #actual,  \
			        check_actual_, check_expected_);                                            \
		}                                                                                       \
	} while (0)

static int connect_port(const char *host, uint16_t port, int timeout_ms) {
	char service[8];
	snprintf(service, sizeof service, "%u", (unsigned)port);
	struct addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo *result = NULL;
	if (getaddrinfo(host, service, &hints, &result) != 0) {
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

static int free_port(void) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return 0;
	}
	struct sockaddr_in address;
	memset(&address, 0, sizeof address);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0) {
		close(fd);
		return 0;
	}
	socklen_t length = sizeof address;
	if (getsockname(fd, (struct sockaddr *)&address, &length) != 0) {
		close(fd);
		return 0;
	}
	uint16_t port = ntohs(address.sin_port);
	close(fd);
	return port;
}

static int read_all(int fd, char *buffer, size_t capacity, size_t *out_length) {
	size_t total = 0;
	while (total + 1 < capacity) {
		ssize_t got = recv(fd, buffer + total, capacity - 1 - total, 0);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			break;
		}
		total += (size_t)got;
	}
	buffer[total] = '\0';
	*out_length = total;
	return 0;
}

static int http_request(const char *host, uint16_t port, const char *method, const char *path,
                        const char *extra_headers, const char *body, int *out_status, char *out_body,
                        size_t out_capacity) {
	int fd = connect_port(host, port, 5000);
	if (fd < 0) {
		return -1;
	}
	char request[2048];
	size_t body_length = body == NULL ? 0 : strlen(body);
	int length = snprintf(request, sizeof request,
	                      "%s %s HTTP/1.1\r\nHost: %s:%u\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
	                      method, path, host, (unsigned)port, extra_headers == NULL ? "" : extra_headers,
	                      body_length);
	if (length < 0 || (size_t)length >= sizeof request) {
		close(fd);
		return -1;
	}
	if (send(fd, request, (size_t)length, 0) != length) {
		close(fd);
		return -1;
	}
	if (body_length > 0 && send(fd, body, body_length, 0) != (ssize_t)body_length) {
		close(fd);
		return -1;
	}
	static char raw[65536];
	size_t raw_length = 0;
	read_all(fd, raw, sizeof raw, &raw_length);
	close(fd);
	*out_status = atoi(strchr(raw, ' ') + 1);
	char *separator = strstr(raw, "\r\n\r\n");
	if (separator == NULL) {
		return -1;
	}
	snprintf(out_body, out_capacity, "%s", separator + 4);
	return 0;
}

static const char *base64_encode(const unsigned char *data, size_t length, char *out, size_t capacity) {
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t written = 0;
	for (size_t i = 0; i < length && written + 4 < capacity; i += 3) {
		unsigned int value = data[i] << 16;
		if (i + 1 < length) {
			value |= data[i + 1] << 8;
		}
		if (i + 2 < length) {
			value |= data[i + 2];
		}
		out[written++] = table[(value >> 18) & 0x3f];
		out[written++] = table[(value >> 12) & 0x3f];
		out[written++] = i + 1 < length ? table[(value >> 6) & 0x3f] : '=';
		out[written++] = i + 2 < length ? table[value & 0x3f] : '=';
	}
	out[written] = '\0';
	return out;
}

static int ws_exchange(uint16_t port, const char *cookie, const char *message, char *out, size_t capacity) {
	int fd = connect_port("127.0.0.1", port, 5000);
	if (fd < 0) {
		return -1;
	}
	unsigned char key_bytes[16];
	for (size_t i = 0; i < sizeof key_bytes; i++) {
		key_bytes[i] = (unsigned char)(i * 7 + 3);
	}
	char key[32];
	base64_encode(key_bytes, sizeof key_bytes, key, sizeof key);
	char handshake[512];
	int length = snprintf(handshake, sizeof handshake,
	                      "GET /jsonrpc HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nUpgrade: websocket\r\n"
	                      "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
	                      "%s\r\n",
	                      (unsigned)port, key, cookie == NULL ? "" : cookie);
	if (send(fd, handshake, (size_t)length, 0) != length) {
		close(fd);
		return -1;
	}
	char response[1024];
	size_t response_length = 0;
	while (response_length + 1 < sizeof response) {
		ssize_t got = recv(fd, response + response_length, sizeof response - 1 - response_length, 0);
		if (got <= 0) {
			break;
		}
		response_length += (size_t)got;
		response[response_length] = '\0';
		if (strstr(response, "\r\n\r\n") != NULL) {
			break;
		}
	}
	if (response_length == 0 || strstr(response, "101") == NULL) {
		close(fd);
		return -1;
	}
	size_t message_length = strlen(message);
	unsigned char frame[1024];
	frame[0] = 0x81;
	frame[1] = (unsigned char)(0x80 | message_length);
	unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
	memcpy(frame + 2, mask, 4);
	for (size_t i = 0; i < message_length; i++) {
		frame[6 + i] = (unsigned char)message[i] ^ mask[i % 4];
	}
	if (send(fd, frame, 6 + message_length, 0) != (ssize_t)(6 + message_length)) {
		close(fd);
		return -1;
	}
	unsigned char header[2];
	if (recv(fd, header, 2, MSG_WAITALL) != 2) {
		close(fd);
		return -1;
	}
	size_t payload = header[1] & 0x7f;
	if (payload == 126) {
		unsigned char extended[2];
		if (recv(fd, extended, 2, MSG_WAITALL) != 2) {
			close(fd);
			return -1;
		}
		payload = ((size_t)extended[0] << 8) | extended[1];
	}
	if (payload >= capacity) {
		payload = capacity - 1;
	}
	size_t received = 0;
	while (received < payload) {
		ssize_t n = recv(fd, out + received, payload - received, 0);
		if (n <= 0) {
			break;
		}
		received += (size_t)n;
	}
	out[received] = '\0';
	close(fd);
	return 0;
}

static bool aria2_installed(void) {
	int status = system("command -v aria2c >/dev/null 2>&1");
	return status == 0;
}

static pid_t launch_aria2(const char *options_path) {
	pid_t pid = fork();
	if (pid == 0) {
		execlp("aria2c", "aria2c", "--conf-path", options_path, "--quiet=true", (char *)NULL);
		_exit(127);
	}
	return pid;
}

static int wait_for_port(const char *host, uint16_t port) {
	for (int i = 0; i < 60; i++) {
		int fd = connect_port(host, port, 500);
		if (fd >= 0) {
			close(fd);
			return 0;
		}
		struct timespec pause = {0, 100 * 1000 * 1000};
		nanosleep(&pause, NULL);
	}
	return -1;
}

static void extract_token(const char *body, char *out, size_t capacity) {
	const char *start = strstr(body, "\"token\":\"");
	if (start == NULL) {
		out[0] = '\0';
		return;
	}
	start += strlen("\"token\":\"");
	size_t length = 0;
	while (start[length] != '"' && start[length] != '\0' && length + 1 < capacity) {
		out[length] = start[length];
		length++;
	}
	out[length] = '\0';
}

int main(void) {
	if (!aria2_installed()) {
		fprintf(stderr, "SKIP: aria2c is not installed; engine integration test not run\n");
		return 0;
	}
	char directory[1024];
	char cwd[1024];
	if (getcwd(cwd, sizeof cwd) == NULL) {
		fprintf(stderr, "integration: getcwd failed\n");
		return 1;
	}
	char command[2048];
	if (snprintf(directory, sizeof directory, "%s/build/test-tmp/integration", cwd) >= (int)sizeof directory ||
	    snprintf(command, sizeof command, "rm -rf '%s' && mkdir -p '%s/session' '%s/www'", directory, directory,
	             directory) >= (int)sizeof command) {
		fprintf(stderr, "integration: path too long\n");
		return 1;
	}
	if (system(command) != 0) {
		fprintf(stderr, "integration: setup failed\n");
		return 1;
	}
	char override_path[1200];
	snprintf(override_path, sizeof override_path, "%s/www/index.html", directory);
	raiju_atomic_write(override_path, "OVERRIDE-INDEX", strlen("OVERRIDE-INDEX"));

	const raiju_platform *platform = raiju_platform_host();
	raiju_engine_config engine;
	char secret_path[1200];
	snprintf(secret_path, sizeof secret_path, "%s/session/aria2.secret", directory);
	CHECK(raiju_engine_config_init(&engine, platform, directory, secret_path));
	engine.rpc_port = (uint16_t)free_port();
	CHECK(engine.rpc_port != 0);
	char options_path[1200];
	snprintf(options_path, sizeof options_path, "%s/session/aria2.conf", directory);
	CHECK(raiju_engine_options_write(&engine, options_path));
	char session_path[1200];
	snprintf(session_path, sizeof session_path, "%s/session/aria2.session", directory);
	CHECK(raiju_atomic_write(session_path, "", 0));

	pid_t aria2 = launch_aria2(options_path);
	CHECK(aria2 > 0);
	CHECK_INT(wait_for_port("127.0.0.1", engine.rpc_port), 0);

	char aria2_url[128];
	snprintf(aria2_url, sizeof aria2_url, "http://127.0.0.1:%u/jsonrpc", (unsigned)engine.rpc_port);
	raiju_gateway_config config;
	memset(&config, 0, sizeof config);
	config.platform = platform;
	config.data_dir = directory;
	config.aria2_url = aria2_url;
	config.secret = engine.secret;
	config.bind_address = "0.0.0.0";
	config.port = 0;
	raiju_gateway *gateway = raiju_gateway_create(&config);
	CHECK(gateway != NULL);
	CHECK(gateway != NULL && raiju_gateway_start(gateway));
	uint16_t port = gateway->bound_port;
	CHECK(port != 0);

	char status[256];
	int code = 0;
	CHECK_INT(http_request("127.0.0.1", port, "GET", "/", NULL, NULL, &code, status, sizeof status), 0);
	CHECK_INT(code, 200);
	CHECK(strstr(status, "OVERRIDE-INDEX") != NULL);

	CHECK_INT(http_request("127.0.0.1", port, "GET", "/app.js", NULL, NULL, &code, status, sizeof status), 0);
	CHECK_INT(code, 200);
	CHECK(strstr(status, "raiju") != NULL);

	char pair_body[128];
	snprintf(pair_body, sizeof pair_body, "{\"code\":\"%s\",\"label\":\"itest\"}", raiju_gateway_code(gateway));
	CHECK_INT(http_request("127.0.0.1", port, "POST", "/api/pair", "Content-Type: application/json\r\n", pair_body,
	                       &code, status, sizeof status),
	          0);
	CHECK_INT(code, 200);
	char token[80];
	extract_token(status, token, sizeof token);
	CHECK(strlen(token) == RAIJU_PAIR_TOKEN_HEX);
	char cookie[160];
	snprintf(cookie, sizeof cookie, "Cookie: raiju_device=%s\r\n", token);

	char get_version[] = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"aria2.getVersion\"}";
	CHECK_INT(http_request("127.0.0.1", port, "POST", "/jsonrpc", cookie, get_version, &code, status,
	                       sizeof status),
	          0);
	CHECK_INT(code, 200);
	CHECK(strstr(status, "\"version\"") != NULL);
	CHECK(strstr(status, engine.secret) == NULL);

	CHECK_INT(http_request("127.0.0.1", port, "POST", "/jsonrpc", NULL, get_version, &code, status, sizeof status), 0);
	CHECK_INT(code, 401);

	raiju_upstream direct;
	CHECK(raiju_upstream_parse(aria2_url, &direct));
	raiju_upstream_response direct_response;
	if (raiju_upstream_post(&direct, (const unsigned char *)get_version, strlen(get_version), 5000,
	                        &direct_response) == 0) {
		CHECK(strstr(raiju_strbuf_cstr(&direct_response.body), "Unauthorized") != NULL);
		raiju_upstream_response_free(&direct_response);
	} else {
		CHECK(false);
	}

	char ws_out[2048];
	CHECK_INT(ws_exchange(port, cookie, get_version, ws_out, sizeof ws_out), 0);
	CHECK(strstr(ws_out, "\"version\"") != NULL);
	CHECK(ws_exchange(port, NULL, get_version, ws_out, sizeof ws_out) != 0);

	int loop_fd = connect_port("127.0.0.1", engine.rpc_port, 2000);
	CHECK(loop_fd >= 0);
	if (loop_fd >= 0) {
		close(loop_fd);
	}
	char lan_ip[64];
	platform->interface_ip(platform->ctx, lan_ip, sizeof lan_ip);
	if (strcmp(lan_ip, "127.0.0.1") != 0) {
		int lan_fd = connect_port(lan_ip, engine.rpc_port, 1500);
		CHECK(lan_fd < 0);
		if (lan_fd >= 0) {
			close(lan_fd);
		}
	}

	raiju_gateway_stop(gateway);
	raiju_gateway_destroy(gateway);
	kill(aria2, SIGTERM);
	waitpid(aria2, NULL, 0);
	snprintf(command, sizeof command, "rm -rf '%s'", directory);
	if (system(command) != 0) {
		fprintf(stderr, "integration: cleanup failed\n");
	}

	printf("integration: %d checks, %d failures\n", integration_checks, integration_failures);
	return integration_failures == 0 ? 0 : 1;
}
