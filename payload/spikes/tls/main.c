#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_CA_PATH RAIJU_DATA_ROOT "/ca.pem"
#define RAIJU_PORT "443"

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

#ifndef RAIJU_TLS_HOST
#define RAIJU_TLS_HOST "example.com"
#endif

#ifndef RAIJU_TLS_PATH
#define RAIJU_TLS_PATH "/"
#endif

extern const unsigned char raiju_ca_bundle[];
extern const unsigned long raiju_ca_bundle_len;

int sceNotificationSend(int userId, bool isLogged, const char *payload);

static const char toast_tmpl[] =
	"{"
	"  \"rawData\": {"
	"    \"viewTemplateType\": \"InteractiveToastTemplateB\","
	"    \"channelType\": \"Downloads\","
	"    \"useCaseId\": \"IDC\","
	"    \"toastOverwriteType\": \"No\","
	"    \"isImmediate\": true,"
	"    \"priority\": 100,"
	"    \"viewData\": {"
	"      \"icon\": {\"type\": \"Predefined\", \"parameters\": {\"icon\": \"download\"}},"
	"      \"message\": {\"body\": \"%s\"},"
	"      \"subMessage\": {\"body\": \"%s\"}"
	"    },"
	"    \"platformViews\": {"
	"      \"previewDisabled\": {"
	"        \"viewData\": {"
	"          \"icon\": {\"type\": \"Predefined\", \"parameters\": {\"icon\": \"download\"}},"
	"          \"message\": {\"body\": \"%s\"}"
	"        }"
	"      }"
	"    }"
	"  },"
	"  \"localNotificationId\": \"1\""
	"}";

/** Raises a toast and reports what the notification service answered. */
static void notify(const char *title, const char *detail) {
	char payload[2048];
	int result;

	if (snprintf(payload, sizeof(payload), toast_tmpl, title, detail, title) < 0) {
		return;
	}
	result = sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true,
				     payload);
	printf("notify: %d\n", result);
	fflush(stdout);
}

/** Writes the embedded bundle to `/data/raiju/ca.pem`, which is where the gateway will read it. */
static int write_ca_bundle(void) {
	const unsigned char *bundle = raiju_ca_bundle;
	unsigned long length = raiju_ca_bundle_len;
	FILE *file;

#ifdef RAIJU_TLS_EMPTY_CA
	length = 0;
#endif

	mkdir(RAIJU_DATA_ROOT, 0755);

	if (!(file = fopen(RAIJU_CA_PATH, "wb"))) {
		printf("ca: cannot write %s: %s\n", RAIJU_CA_PATH, strerror(errno));
		return -1;
	}
	if (length > 0 && fwrite(bundle, 1, length, file) != length) {
		printf("ca: short write\n");
		fclose(file);
		return -1;
	}
	fclose(file);
	printf("ca: wrote %lu bytes to %s\n", length, RAIJU_CA_PATH);

	return 0;
}

/** Opens a TCP connection to a host and port, without TLS. */
static int connect_to(const char *host, const char *port) {
	struct addrinfo hints;
	struct addrinfo *addresses = NULL;
	struct addrinfo *address;
	int fd = -1;
	int result;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((result = getaddrinfo(host, port, &hints, &addresses)) != 0) {
		printf("dns: %s: %s\n", host, gai_strerror(result));
		return -1;
	}

	for (address = addresses; address; address = address->ai_next) {
		fd = socket(address->ai_family, address->ai_socktype,
			    address->ai_protocol);
		if (fd < 0) {
			continue;
		}
		printf("connect: %s:%s\n", host, port);
		if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
			break;
		}
		printf("connect: %s\n", strerror(errno));
		close(fd);
		fd = -1;
	}

	freeaddrinfo(addresses);

	return fd;
}

/** Sends the request and prints the response status line, headers it recognises and body head. */
static int read_response(SSL *ssl) {
	char buffer[4096];
	int total = 0;
	int printed = 0;
	int status = 0;
	int n;

	while ((n = SSL_read(ssl, buffer, sizeof(buffer) - 1)) > 0) {
		buffer[n] = '\0';
		if (printed < 600) {
			int room = 600 - printed;
			int chunk = n < room ? n : room;

			fwrite(buffer, 1, chunk, stdout);
			printed += chunk;
		}
		if (status == 0 && sscanf(buffer, "HTTP/%*s %d", &status) != 1) {
			status = -1;
		}
		total += n;
	}

	printf("\nbody: %d bytes read\n", total);
	fflush(stdout);

	return status;
}

/** Runs an HTTPS GET with certificate verification, printing what it learns on the way. */
static int https_get(const char *host, const char *path) {
	char request[512];
	int status = -1;
	int fd;
	SSL_CTX *context = NULL;
	SSL *ssl = NULL;
	X509 *peer = NULL;
	int verify;

	SSL_library_init();
	SSL_load_error_strings();

	if (!(context = SSL_CTX_new(TLS_client_method()))) {
		printf("tls: no context\n");
		return -1;
	}
	SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
	SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
	if (SSL_CTX_load_verify_locations(context, RAIJU_CA_PATH, NULL) != 1) {
		printf("tls: cannot load %s: %s\n", RAIJU_CA_PATH,
		       ERR_error_string(ERR_get_error(), NULL));
		goto done;
	}

	if ((fd = connect_to(host, RAIJU_PORT)) < 0) {
		goto done;
	}
	if (!(ssl = SSL_new(context))) {
		close(fd);
		goto done;
	}
	SSL_set_fd(ssl, fd);
	SSL_set_tlsext_host_name(ssl, host);
	SSL_set1_host(ssl, host);

	if (SSL_connect(ssl) != 1) {
		printf("tls: handshake failed: %s\n",
		       ERR_error_string(ERR_get_error(), NULL));
		close(fd);
		goto done;
	}

	verify = SSL_get_verify_result(ssl);
	printf("tls: %s, %s, verify %d\n", SSL_get_version(ssl),
	       SSL_get_cipher(ssl), verify);
	if (verify != X509_V_OK) {
		printf("tls: verification failed: %s\n",
		       X509_verify_cert_error_string(verify));
		goto done;
	}

	if ((peer = SSL_get1_peer_certificate(ssl))) {
		char subject[256];

		X509_NAME_oneline(X509_get_subject_name(peer), subject,
				  sizeof(subject));
		printf("tls: peer %s\n", subject);
	}

	snprintf(request, sizeof(request),
		 "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: raiju/%s\r\n"
		 "Accept: */*\r\nConnection: close\r\n\r\n",
		 path, host, RAIJU_VERSION);
	if (SSL_write(ssl, request, strlen(request)) <= 0) {
		printf("http: write failed\n");
		goto done;
	}
	status = read_response(ssl);

done:
	X509_free(peer);
	if (ssl) {
		SSL_shutdown(ssl);
		SSL_free(ssl);
	}
	if (context) {
		SSL_CTX_free(context);
	}

	return status;
}

/**
 * Proves the TLS stack on the console: writes the embedded CA bundle to `/data/raiju/ca.pem`,
 * fetches a URL over HTTPS with verification and says what came back. Built with
 * `-DRAIJU_TLS_EMPTY_CA` it ships an empty bundle instead, which must make the same request fail.
 */
int main(void) {
	char title[64];
	char detail[128];
	int status;

	printf("raiju tls spike %s, firmware %08x\n", RAIJU_VERSION,
	       kernel_get_fw_version());
#ifdef RAIJU_TLS_EMPTY_CA
	printf("trust: empty bundle on purpose\n");
#else
	printf("trust: %lu byte bundle\n", raiju_ca_bundle_len);
#endif

	write_ca_bundle();

	printf("get: https://%s%s\n", RAIJU_TLS_HOST, RAIJU_TLS_PATH);
	status = https_get(RAIJU_TLS_HOST, RAIJU_TLS_PATH);
	printf("status: %d\n", status);
	fflush(stdout);

	snprintf(title, sizeof(title), "Raiju %s", RAIJU_VERSION);
	if (status > 0) {
		snprintf(detail, sizeof(detail), "HTTPS %d from %s",
			 status, RAIJU_TLS_HOST);
	} else {
		snprintf(detail, sizeof(detail), "HTTPS failed for %s",
			 RAIJU_TLS_HOST);
	}
	notify(title, detail);

	payload_exit(status == 200 ? 0 : 1);

	return status == 200 ? 0 : 1;
}
