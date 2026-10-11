#include <stdlib.h>
#include <string.h>

#include "raiju/proxy.h"
#include "test.h"

static void parses_upstream_urls(void) {
	raiju_upstream upstream;
	CHECK(raiju_upstream_parse("http://127.0.0.1:6800/jsonrpc", &upstream));
	CHECK_STR(upstream.host, "127.0.0.1");
	CHECK_INT(upstream.port, 6800);
	CHECK_STR(upstream.path, "/jsonrpc");
	CHECK(!raiju_upstream_parse("https://127.0.0.1:6800/jsonrpc", &upstream));
	CHECK(!raiju_upstream_parse("http://127.0.0.1/jsonrpc", &upstream));
	CHECK(!raiju_upstream_parse("http://127.0.0.1:notaport/x", &upstream));
	CHECK(raiju_upstream_parse("http://127.0.0.1:6800", &upstream));
	CHECK_STR(upstream.path, "/jsonrpc");
}

static void injects_secret_as_first_param(void) {
	const char *body = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"aria2.addUri\",\"params\":[[\"http://x\"]]}";
	size_t length = 0;
	char *injected = raiju_proxy_inject_secret(body, strlen(body), "abc123", &length);
	CHECK(injected != NULL);
	CHECK(strstr(injected, "\"params\":[\"token:abc123\",") != NULL);
	free(injected);
}

static void injects_secret_without_params(void) {
	const char *body = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"aria2.getVersion\"}";
	size_t length = 0;
	char *injected = raiju_proxy_inject_secret(body, strlen(body), "sekret", &length);
	CHECK(injected != NULL);
	CHECK(strstr(injected, "\"params\":[\"token:sekret\"]") != NULL);
	free(injected);
}

static void does_not_double_inject(void) {
	const char *body = "{\"method\":\"aria2.getVersion\",\"params\":[\"token:already\"]}";
	size_t length = 0;
	char *injected = raiju_proxy_inject_secret(body, strlen(body), "sekret", &length);
	CHECK(injected != NULL);
	CHECK_STR(injected, body);
	free(injected);
}

void run_proxy_tests(void) {
	RUN_TEST(parses_upstream_urls);
	RUN_TEST(injects_secret_as_first_param);
	RUN_TEST(injects_secret_without_params);
	RUN_TEST(does_not_double_inject);
}
