#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "test.h"

#ifdef RAIJU_FETCH_TLS

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include "fake_platform.h"
#include "raiju/fetch.h"
#include "raiju/platform.h"

#define TLS_BODY "raiju tls body"
#define TLS_MAX_BYTES 4096
#define TLS_TIMEOUT_MS 5000

typedef struct {
	int listen_fd;
	uint16_t port;
	pthread_t thread;
	volatile int running;
	SSL_CTX *ctx;
} tls_server;

static void *tls_server_thread(void *argument) {
	tls_server *server = argument;
	while (__atomic_load_n(&server->running, __ATOMIC_SEQ_CST)) {
		int fd = accept(server->listen_fd, NULL, NULL);
		if (fd < 0) {
			continue;
		}
		SSL *ssl = SSL_new(server->ctx);
		if (ssl != NULL) {
			SSL_set_fd(ssl, fd);
			if (SSL_accept(ssl) == 1) {
				char request[1024];
				(void)SSL_read(ssl, request, sizeof request);
				char header[256];
				int length = snprintf(header, sizeof header,
				                      "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
				                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
				                      strlen(TLS_BODY));
				(void)SSL_write(ssl, header, length);
				(void)SSL_write(ssl, TLS_BODY, (int)strlen(TLS_BODY));
				SSL_shutdown(ssl);
			}
			SSL_free(ssl);
		}
		close(fd);
	}
	return NULL;
}

/**
 * Serves TLS on an ephemeral loopback port with one fixture certificate and
 * its key, so a test can drive a real handshake without leaving the host.
 */
static tls_server *tls_server_start(const char *cert_path, const char *key_path) {
	tls_server *server = calloc(1, sizeof *server);
	if (server == NULL) {
		return NULL;
	}
	server->ctx = SSL_CTX_new(TLS_server_method());
	if (server->ctx == NULL || SSL_CTX_use_certificate_chain_file(server->ctx, cert_path) != 1 ||
	    SSL_CTX_use_PrivateKey_file(server->ctx, key_path, SSL_FILETYPE_PEM) != 1) {
		SSL_CTX_free(server->ctx);
		free(server);
		return NULL;
	}
	server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server->listen_fd < 0) {
		SSL_CTX_free(server->ctx);
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
		SSL_CTX_free(server->ctx);
		free(server);
		return NULL;
	}
	socklen_t length = sizeof address;
	getsockname(server->listen_fd, (struct sockaddr *)&address, &length);
	server->port = ntohs(address.sin_port);
	__atomic_store_n(&server->running, 1, __ATOMIC_SEQ_CST);
	if (pthread_create(&server->thread, NULL, tls_server_thread, server) != 0) {
		close(server->listen_fd);
		SSL_CTX_free(server->ctx);
		free(server);
		return NULL;
	}
	return server;
}

static void tls_server_stop(tls_server *server) {
	if (server == NULL) {
		return;
	}
	__atomic_store_n(&server->running, 0, __ATOMIC_SEQ_CST);
	shutdown(server->listen_fd, SHUT_RDWR);
	close(server->listen_fd);
	pthread_join(server->thread, NULL);
	SSL_CTX_free(server->ctx);
	free(server);
}

/**
 * Writes the absolute path of one frozen fixture into out, and fails the run
 * rather than testing a truncated path.
 */
static void fixture_path(char *out, size_t capacity, const char *name) {
	int written = snprintf(out, capacity, "%s/tls/%s", RAIJU_TEST_FIXTURE_DIR, name);
	CHECK(written > 0 && (size_t)written < capacity);
}

/**
 * Fetches the served blob through the built-in client, with the platform hook
 * naming store_path as the trust store, or the default store when it is empty.
 */
static int fetch_blob(const tls_server *server, const char *store_path, raiju_fetch_response *response) {
	raiju_fake_platform fake;
	raiju_fake_platform_init(&fake);
	CHECK(strlen(store_path) < sizeof fake.ca_bundle);
	snprintf(fake.ca_bundle, sizeof fake.ca_bundle, "%s", store_path);
	char url[128];
	snprintf(url, sizeof url, "https://127.0.0.1:%u/blob.bin", (unsigned)server->port);
	return raiju_fetch_default()(&fake.base, RAIJU_FETCH_GET, url, NULL, TLS_MAX_BYTES, TLS_TIMEOUT_MS, response);
}

static void check_accepted(const tls_server *server, const char *store_path) {
	raiju_fetch_response response;
	int result = fetch_blob(server, store_path, &response);
	CHECK_INT(result, 0);
	if (result == 0) {
		CHECK_INT(response.status, 200);
		CHECK_STR(response.body, TLS_BODY);
	}
	raiju_fetch_response_free(&response);
}

static void check_refused(const tls_server *server, const char *store_path) {
	raiju_fetch_response response;
	CHECK(fetch_blob(server, store_path, &response) != 0);
	raiju_fetch_response_free(&response);
}

/**
 * Serves one leaf fixture, named without its extension, on an ephemeral
 * loopback port and runs one store against it.
 */
static void with_leaf_server(const char *leaf, const char *store_path, bool expect_accept) {
	char cert_name[64];
	char key_name[64];
	CHECK(strlen(leaf) + 8 < sizeof cert_name);
	snprintf(cert_name, sizeof cert_name, "%s.pem", leaf);
	snprintf(key_name, sizeof key_name, "%s-key.pem", leaf);
	char cert[1200];
	char key[1200];
	fixture_path(cert, sizeof cert, cert_name);
	fixture_path(key, sizeof key, key_name);
	tls_server *server = tls_server_start(cert, key);
	CHECK(server != NULL);
	if (server != NULL) {
		if (expect_accept) {
			check_accepted(server, store_path);
		} else {
			check_refused(server, store_path);
		}
		tls_server_stop(server);
	}
}

typedef struct {
	char previous[2048];
	bool had_previous;
} saved_store;

/**
 * Points the default trust store at path for the rest of the test, remembering
 * what SSL_CERT_FILE held so another test never inherits the redirection.
 */
static void redirect_default_store(saved_store *saved, const char *path) {
	const char *current = getenv("SSL_CERT_FILE");
	saved->had_previous = current != NULL;
	snprintf(saved->previous, sizeof saved->previous, "%s", current != NULL ? current : "");
	setenv("SSL_CERT_FILE", path, 1);
}

static void restore_default_store(const saved_store *saved) {
	if (saved->had_previous) {
		setenv("SSL_CERT_FILE", saved->previous, 1);
	} else {
		unsetenv("SSL_CERT_FILE");
	}
}

static void a_missing_store_refuses_the_fetch(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "tls-missing-store"), 0);
	char absent[1200];
	snprintf(absent, sizeof absent, "%s/absent-ca.pem", directory);
	with_leaf_server("leaf-ip", absent, false);
	raiju_test_rmtree(directory);
}

static void an_empty_store_refuses_the_fetch(void) {
	char store[1200];
	fixture_path(store, sizeof store, "empty-ca.pem");
	with_leaf_server("leaf-ip", store, false);
}

static void a_store_that_did_not_issue_the_chain_refuses_the_fetch(void) {
	char store[1200];
	fixture_path(store, sizeof store, "stranger-ca.pem");
	with_leaf_server("leaf-ip", store, false);
}

static void the_store_that_issued_the_chain_accepts_the_fetch(void) {
	char store[1200];
	fixture_path(store, sizeof store, "root-ca.pem");
	with_leaf_server("leaf-ip", store, true);
}

static void the_default_store_is_consulted_and_still_verifies(void) {
	char issuer[1200];
	char stranger[1200];
	fixture_path(issuer, sizeof issuer, "root-ca.pem");
	fixture_path(stranger, sizeof stranger, "stranger-ca.pem");
	saved_store saved;
	redirect_default_store(&saved, issuer);
	with_leaf_server("leaf-ip", "", true);
	redirect_default_store(&saved, stranger);
	with_leaf_server("leaf-ip", "", false);
	restore_default_store(&saved);
}

static void an_ip_literal_host_matches_the_ip_subject_alt_name(void) {
	char store[1200];
	fixture_path(store, sizeof store, "root-ca.pem");
	with_leaf_server("leaf-ip", store, true);
	with_leaf_server("leaf-dns", store, false);
}

void run_tls_tests(void) {
	RUN_TEST(a_missing_store_refuses_the_fetch);
	RUN_TEST(an_empty_store_refuses_the_fetch);
	RUN_TEST(a_store_that_did_not_issue_the_chain_refuses_the_fetch);
	RUN_TEST(the_store_that_issued_the_chain_accepts_the_fetch);
	RUN_TEST(the_default_store_is_consulted_and_still_verifies);
	RUN_TEST(an_ip_literal_host_matches_the_ip_subject_alt_name);
}

#else

/**
 * Fails the run when the build has no OpenSSL, so the trust-store contract
 * cannot drop out of the suite unnoticed on a builder that lost TLS.
 */
static void the_trust_store_contract_needs_a_tls_build(void) {
	fprintf(stderr,
	        "tls: this build has no OpenSSL, so RAIJU_FETCH_TLS is undefined and the trust-store "
	        "contract is untested; install pkg-config and openssl (libssl-dev, or brew openssl) and rerun\n");
	CHECK(false);
}

void run_tls_tests(void) {
	RUN_TEST(the_trust_store_contract_needs_a_tls_build);
}

#endif
