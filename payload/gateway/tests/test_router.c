#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fake_platform.h"
#include "raiju/gateway.h"
#include "raiju/http.h"
#include "test.h"

static raiju_gateway *make_gateway(raiju_fake_platform *fake, char *directory, size_t capacity) {
	if (raiju_test_tmpdir(directory, capacity, "router") != 0) {
		return NULL;
	}
	char session[1200];
	snprintf(session, sizeof session, "%s/session", directory);
	mkdir(session, 0755);
	const raiju_platform *platform = raiju_fake_platform_init(fake);
	raiju_gateway_config config;
	memset(&config, 0, sizeof config);
	config.platform = platform;
	config.data_dir = directory;
	config.aria2_url = "http://127.0.0.1:6800/jsonrpc";
	config.secret = "supersecretvalue";
	return raiju_gateway_create(&config);
}

static const char *header_value(const raiju_http_response *response, const char *name) {
	size_t name_length = strlen(name);
	for (size_t i = 0; i < response->header_count; i++) {
		const char *header = raiju_strbuf_cstr((raiju_strbuf *)&response->headers[i]);
		if (strncmp(header, name, name_length) == 0 && header[name_length] == ':') {
			return header;
		}
	}
	return NULL;
}

static void status_is_public_and_leaks_nothing(void) {
	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, directory, sizeof directory);
	CHECK(gateway != NULL);
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/status";
	raiju_http_response response;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	const char *body = raiju_strbuf_cstr(&response.body);
	CHECK(strstr(body, "\"name\":\"raiju\"") != NULL);
	CHECK(strstr(body, "supersecretvalue") == NULL);
	CHECK(strstr(body, "\"token\"") == NULL);
	raiju_response_free(&response);
	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
}

static void protected_endpoints_need_a_token(void) {
	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, directory, sizeof directory);
	CHECK(gateway != NULL);
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/devices";
	raiju_http_response response;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 401);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/jsonrpc";
	request.body = (const unsigned char *)"{}";
	request.body_length = 2;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 401);
	raiju_response_free(&response);
	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
}

static void pair_then_use_and_revoke_token(void) {
	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, directory, sizeof directory);
	CHECK(gateway != NULL);
	char body[128];
	snprintf(body, sizeof body, "{\"code\":\"%s\",\"label\":\"laptop\"}", raiju_gateway_code(gateway));
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/pair";
	request.body = (const unsigned char *)body;
	request.body_length = strlen(body);
	raiju_http_response response;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	CHECK(header_value(&response, "Set-Cookie") != NULL);
	const char *pair_body = raiju_strbuf_cstr(&response.body);
	const char *token_start = strstr(pair_body, "\"token\":\"");
	CHECK(token_start != NULL);
	char token[80] = {0};
	char id[80] = {0};
	if (token_start != NULL) {
		token_start += strlen("\"token\":\"");
		memcpy(token, token_start, RAIJU_PAIR_TOKEN_HEX);
	}
	const char *id_start = strstr(pair_body, "\"id\":\"");
	if (id_start != NULL) {
		id_start += strlen("\"id\":\"");
		const char *id_end = strchr(id_start, '"');
		if (id_end != NULL) {
			memcpy(id, id_start, (size_t)(id_end - id_start));
		}
	}
	raiju_response_free(&response);

	char cookie[160];
	snprintf(cookie, sizeof cookie, "raiju_device=%s", token);
	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/devices";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), id) != NULL);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), token) == NULL);
	raiju_response_free(&response);

	char delete_path[96];
	snprintf(delete_path, sizeof delete_path, "/api/devices/%s", id);
	memset(&request, 0, sizeof request);
	request.method = "DELETE";
	request.path = delete_path;
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	raiju_response_free(&response);

	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
}

static void serves_embedded_assets_and_404s(void) {
	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, directory, sizeof directory);
	CHECK(gateway != NULL);
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/";
	raiju_http_response response;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	CHECK(header_value(&response, "Content-Type") != NULL);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), "doctype") != NULL);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/does-not-exist";
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 404);
	raiju_response_free(&response);
	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
}

static void pairing_refuses_an_unsafe_label(void) {
	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, directory, sizeof directory);
	CHECK(gateway != NULL);
	char body[160];
	snprintf(body, sizeof body, "{\"code\":\"%s\",\"label\":\"bad\\\"label\"}", raiju_gateway_code(gateway));
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/pair";
	request.body = (const unsigned char *)body;
	request.body_length = strlen(body);
	raiju_http_response response;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 400);
	CHECK_INT(gateway->pairing.device_count, 0);
	raiju_response_free(&response);
	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
}

void run_router_tests(void) {
	RUN_TEST(status_is_public_and_leaks_nothing);
	RUN_TEST(protected_endpoints_need_a_token);
	RUN_TEST(pair_then_use_and_revoke_token);
	RUN_TEST(pairing_refuses_an_unsafe_label);
	RUN_TEST(serves_embedded_assets_and_404s);
}
