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

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "fake_platform.h"
#include "raiju/fetch.h"
#include "raiju/platform.h"

/**
 * Generates a self-signed RSA certificate with subjectAltName IP:<ip>,
 * basicConstraints CA:TRUE and a long validity, writing cert and key as PEM.
 */
static bool write_self_signed(const char *cert_path, const char *key_path, const char *ip) {
	EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)2048);
	if (key == NULL) {
		return false;
	}
	X509 *cert = X509_new();
	if (cert == NULL) {
		EVP_PKEY_free(key);
		return false;
	}
	X509_set_version(cert, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
	X509_gmtime_adj(X509_getm_notBefore(cert), 0);
	X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60L * 24L * 3650L);
	X509_set_pubkey(cert, key);
	X509_NAME *name = (X509_NAME *)X509_get_subject_name(cert);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"raiju-tls-test", -1, -1, 0);
	X509_set_issuer_name(cert, name);
	X509V3_CTX context;
	X509V3_set_ctx_nodb(&context);
	X509V3_set_ctx(&context, cert, cert, NULL, NULL, 0);
	X509_EXTENSION *extension = X509V3_EXT_conf_nid(NULL, &context, NID_basic_constraints, "critical,CA:TRUE");
	if (extension != NULL) {
		X509_add_ext(cert, extension, -1);
		X509_EXTENSION_free(extension);
	}
	char san[64];
	snprintf(san, sizeof san, "IP:%s", ip);
	extension = X509V3_EXT_conf_nid(NULL, &context, NID_subject_alt_name, san);
	if (extension != NULL) {
		X509_add_ext(cert, extension, -1);
		X509_EXTENSION_free(extension);
	}
	bool ok = X509_sign(cert, key, EVP_sha256()) != 0;
	FILE *cert_file = ok ? fopen(cert_path, "wb") : NULL;
	ok = cert_file != NULL && PEM_write_X509(cert_file, cert) == 1;
	if (cert_file != NULL) {
		fclose(cert_file);
	}
	FILE *key_file = ok ? fopen(key_path, "wb") : NULL;
	ok = key_file != NULL && PEM_write_PrivateKey(key_file, key, NULL, NULL, 0, NULL, NULL) == 1;
	if (key_file != NULL) {
		fclose(key_file);
	}
	X509_free(cert);
	EVP_PKEY_free(key);
	return ok;
}

typedef struct {
	int listen_fd;
	uint16_t port;
	pthread_t thread;
	volatile int running;
	SSL_CTX *ctx;
	char body[64];
	size_t body_length;
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
				                      server->body_length);
				(void)SSL_write(ssl, header, length);
				(void)SSL_write(ssl, server->body, (int)server->body_length);
				SSL_shutdown(ssl);
			}
			SSL_free(ssl);
		}
		close(fd);
	}
	return NULL;
}

static tls_server *tls_server_start(const char *cert_path, const char *key_path, const char *body) {
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
	snprintf(server->body, sizeof server->body, "%s", body);
	server->body_length = strlen(server->body);
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

typedef struct {
	raiju_platform base;
	char bundle[1200];
} tls_platform;

static int tls_ca_bundle(void *ctx, char *buffer, size_t capacity) {
	tls_platform *platform = ctx;
	snprintf(buffer, capacity, "%s", platform->bundle);
	return 0;
}

static void a_real_https_fetch_honours_the_ca_bundle(void) {
	char directory[1024];
	if (raiju_test_tmpdir(directory, sizeof directory, "tls") != 0) {
		CHECK(false);
		return;
	}
	char cert_path[1200];
	char key_path[1200];
	char other_cert_path[1200];
	char other_key_path[1200];
	snprintf(cert_path, sizeof cert_path, "%s/cert.pem", directory);
	snprintf(key_path, sizeof key_path, "%s/key.pem", directory);
	snprintf(other_cert_path, sizeof other_cert_path, "%s/other-cert.pem", directory);
	snprintf(other_key_path, sizeof other_key_path, "%s/other-key.pem", directory);
	CHECK(write_self_signed(cert_path, key_path, "127.0.0.1"));
	CHECK(write_self_signed(other_cert_path, other_key_path, "127.0.0.1"));
	static const char body[] = "raiju tls body";
	tls_server *server = tls_server_start(cert_path, key_path, body);
	CHECK(server != NULL);
	if (server != NULL) {
		tls_platform trusted;
		memset(&trusted, 0, sizeof trusted);
		trusted.base.ca_bundle_path = tls_ca_bundle;
		trusted.base.ctx = &trusted;
		snprintf(trusted.bundle, sizeof trusted.bundle, "%s", cert_path);
		char url[128];
		snprintf(url, sizeof url, "https://127.0.0.1:%u/blob.bin", server->port);
		raiju_fetch_response response;
		CHECK_INT(raiju_fetch_default()(&trusted.base, RAIJU_FETCH_GET, url, NULL, 4096, 5000, &response), 0);
		CHECK_INT(response.status, 200);
		CHECK_STR(response.body, body);
		raiju_fetch_response_free(&response);

		tls_platform stranger;
		memset(&stranger, 0, sizeof stranger);
		stranger.base.ca_bundle_path = tls_ca_bundle;
		stranger.base.ctx = &stranger;
		snprintf(stranger.bundle, sizeof stranger.bundle, "%s", other_cert_path);
		raiju_fetch_response failed;
		CHECK(raiju_fetch_default()(&stranger.base, RAIJU_FETCH_GET, url, NULL, 4096, 5000, &failed) != 0);
		raiju_fetch_response_free(&failed);

		tls_server_stop(server);
	}
	raiju_test_rmtree(directory);
}

void run_tls_tests(void) {
	RUN_TEST(a_real_https_fetch_honours_the_ca_bundle);
}

#else

void run_tls_tests(void) {}

#endif
