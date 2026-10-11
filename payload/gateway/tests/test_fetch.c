#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture_server.h"
#include "raiju/fetch.h"
#include "test.h"

static void get_returns_the_body(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	static const char blob[] = "0123456789";
	CHECK(raiju_fixture_server_add_file(server, "/blob.bin", blob, sizeof blob - 1));
	char url[128];
	snprintf(url, sizeof url, "http://127.0.0.1:%u/blob.bin", raiju_fixture_server_port(server));
	raiju_fetch_response response;
	CHECK_INT(raiju_fetch_default()(NULL, RAIJU_FETCH_GET, url, NULL, 4096, 5000, &response), 0);
	CHECK_INT(response.status, 200);
	CHECK_INT(response.body_length, 10);
	CHECK_STR(response.body, "0123456789");
	raiju_fetch_response_free(&response);
	raiju_fixture_server_stop(server);
}

static void a_range_request_is_sent_and_honoured(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	static const char blob[] = "0123456789";
	CHECK(raiju_fixture_server_add_file(server, "/blob.bin", blob, sizeof blob - 1));
	char url[128];
	snprintf(url, sizeof url, "http://127.0.0.1:%u/blob.bin", raiju_fixture_server_port(server));
	raiju_fetch_response response;
	CHECK_INT(raiju_fetch_default()(NULL, RAIJU_FETCH_GET, url, "bytes=2-5", 4096, 5000, &response), 0);
	CHECK_INT(response.status, 206);
	CHECK_STR(raiju_fixture_server_last_range(server), "bytes=2-5");
	CHECK_STR(response.body, "2345");
	CHECK_INT(response.content_length, 10);
	raiju_fetch_response_free(&response);
	raiju_fixture_server_stop(server);
}

static void head_reports_the_length_without_a_body(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	static const char blob[] = "abcdefghij";
	CHECK(raiju_fixture_server_add_file(server, "/blob.bin", blob, sizeof blob - 1));
	char url[128];
	snprintf(url, sizeof url, "http://127.0.0.1:%u/blob.bin", raiju_fixture_server_port(server));
	raiju_fetch_response response;
	CHECK_INT(raiju_fetch_default()(NULL, RAIJU_FETCH_HEAD, url, NULL, 0, 5000, &response), 0);
	CHECK_INT(response.status, 200);
	CHECK_INT(response.content_length, 10);
	CHECK_INT(response.body_length, 0);
	CHECK_STR(raiju_fixture_server_last_method(server), "HEAD");
	raiju_fetch_response_free(&response);
	raiju_fixture_server_stop(server);
}

static void an_oversized_response_is_refused(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	unsigned char blob[4096];
	memset(blob, 'x', sizeof blob);
	CHECK(raiju_fixture_server_add_file(server, "/big.bin", blob, sizeof blob));
	char url[128];
	snprintf(url, sizeof url, "http://127.0.0.1:%u/big.bin", raiju_fixture_server_port(server));
	raiju_fetch_response response;
	CHECK_INT(raiju_fetch_default()(NULL, RAIJU_FETCH_GET, url, NULL, 128, 5000, &response), -1);
	CHECK(response.truncated);
	raiju_fetch_response_free(&response);
	raiju_fixture_server_stop(server);
}

static void a_missing_path_is_returned_as_is(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	char url[128];
	snprintf(url, sizeof url, "http://127.0.0.1:%u/nope", raiju_fixture_server_port(server));
	raiju_fetch_response response;
	CHECK_INT(raiju_fetch_default()(NULL, RAIJU_FETCH_GET, url, NULL, 4096, 5000, &response), 0);
	CHECK_INT(response.status, 404);
	raiju_fetch_response_free(&response);
	raiju_fixture_server_stop(server);
}

static void an_unreachable_host_fails(void) {
	raiju_fetch_response response;
	CHECK_INT(raiju_fetch_default()(NULL, RAIJU_FETCH_GET, "http://127.0.0.1:1/x", NULL, 4096, 500, &response), -1);
	raiju_fetch_response_free(&response);
}

static void a_url_that_could_inject_a_request_is_refused(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	static const char blob[] = "0123456789";
	CHECK(raiju_fixture_server_add_file(server, "/blob.bin", blob, sizeof blob - 1));
	char url[160];
	raiju_fetch_response response;
	snprintf(url, sizeof url, "http://127.0.0.1:%u/blob.bin HTTP/1.1", raiju_fixture_server_port(server));
	CHECK_INT(raiju_fetch_default()(NULL, RAIJU_FETCH_GET, url, NULL, 4096, 5000, &response), -1);
	raiju_fetch_response_free(&response);
	snprintf(url, sizeof url, "http://127.0.0.1:%u/blob.bin\r\nHost: evil", raiju_fixture_server_port(server));
	CHECK_INT(raiju_fetch_default()(NULL, RAIJU_FETCH_GET, url, NULL, 4096, 5000, &response), -1);
	raiju_fetch_response_free(&response);
	CHECK_INT(raiju_fixture_server_hits(server, "/blob.bin"), 0);
	CHECK_INT(raiju_fetch_default()(NULL, RAIJU_FETCH_GET, "http://127.0.0.1:notaport/x", NULL, 4096, 500, &response), -1);
	raiju_fetch_response_free(&response);
	CHECK_INT(raiju_fetch_default()(NULL, RAIJU_FETCH_GET, "ftp://127.0.0.1/x", NULL, 4096, 500, &response), -1);
	raiju_fetch_response_free(&response);
	raiju_fixture_server_stop(server);
}

static void the_host_header_carries_a_non_default_port(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	static const char blob[] = "0123456789";
	CHECK(raiju_fixture_server_add_file(server, "/blob.bin", blob, sizeof blob - 1));
	char url[128];
	snprintf(url, sizeof url, "http://127.0.0.1:%u/blob.bin", raiju_fixture_server_port(server));
	raiju_fetch_response response;
	CHECK_INT(raiju_fetch_default()(NULL, RAIJU_FETCH_GET, url, NULL, 4096, 5000, &response), 0);
	CHECK_INT(response.status, 200);
	char expected[128];
	snprintf(expected, sizeof expected, "127.0.0.1:%u", raiju_fixture_server_port(server));
	CHECK_STR(raiju_fixture_server_last_host(server), expected);
	raiju_fetch_response_free(&response);
	raiju_fixture_server_stop(server);
}

void run_fetch_tests(void) {
	RUN_TEST(get_returns_the_body);
	RUN_TEST(a_range_request_is_sent_and_honoured);
	RUN_TEST(head_reports_the_length_without_a_body);
	RUN_TEST(an_oversized_response_is_refused);
	RUN_TEST(a_missing_path_is_returned_as_is);
	RUN_TEST(an_unreachable_host_fails);
	RUN_TEST(a_url_that_could_inject_a_request_is_refused);
	RUN_TEST(the_host_header_carries_a_non_default_port);
}
