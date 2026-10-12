#define _POSIX_C_SOURCE 200809L

#include "raiju/local_code.h"

#include <string.h>

#include "raiju/gateway.h"
#include "raiju/strbuf.h"

/** True when an address belongs to the console itself rather than the network. */
static bool is_loopback(const char *address) {
	if (address == NULL) {
		return false;
	}
	return strncmp(address, "127.", 4) == 0 || strcmp(address, "::1") == 0 ||
	       strncmp(address, "::ffff:127.", 11) == 0;
}

/** Compares a presented secret with the engine's without leaking where they differ. */
static bool secret_matches(const char *presented, const char *expected) {
	size_t length = strlen(expected);
	unsigned char difference = 0;

	if (presented == NULL || length == 0) {
		return false;
	}
	if (strncmp(presented, "Bearer ", 7) == 0) {
		presented += 7;
	}
	if (strlen(presented) != length) {
		return false;
	}
	for (size_t i = 0; i < length; i++) {
		difference |= (unsigned char)presented[i] ^ (unsigned char)expected[i];
	}
	return difference == 0;
}

int raiju_local_code_handler(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_gateway *gateway = context;
	raiju_strbuf document;

	if (!is_loopback(request->peer_address) || !secret_matches(request->authorization, gateway->secret)) {
		raiju_response_error(response, 403, "forbidden");
		return 403;
	}
	raiju_strbuf_init(&document);
	raiju_strbuf_append(&document, "{\"code\":");
	raiju_strbuf_append_json_string(&document, gateway->pairing.code);
	raiju_strbuf_append(&document, "}");
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}
