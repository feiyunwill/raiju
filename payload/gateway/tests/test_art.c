#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "fake_platform.h"
#include "raiju/art.h"
#include "raiju/gateway.h"
#include "raiju/http.h"
#include "test.h"

#define PPSA_HASH "0fc378c528c70c9e1df85472c51743f708b151221b97c3d2d87ee8d9eb01651b"
#define CUSA_HASH "ee91ab19c76a44bec5b591462cf4576abc77fcd658c101851f10d91a63962bca"

static const char ps5_page[] =
    "<html><head><title>PPSA25380: ELDEN RING NIGHTREIGN</title>\n"
    "<meta property=\"og:title\" content=\"PPSA25380: ELDEN RING NIGHTREIGN\">\n"
    "<meta name=\"twitter:image\" content=\"https://cdn.prosperopatches.com/titles/PPSA25380_" PPSA_HASH "/icon0.webp\">\n"
    "</head><body><img src=\"https://cdn.prosperopatches.com/titles/PPSA25380_" PPSA_HASH "/pic0.webp\">\n"
    "<img src=\"https://cdn.prosperopatches.com/titles/PPSA99999_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/icon0.webp\"></body></html>";

static const char ps4_page[] =
    "<title>CUSA00003: DRIVECLUB&#8482; | ORBISPatches.com</title>\n"
    "<img src=\"https://cdn.orbispatches.com/titles/CUSA00003_" CUSA_HASH "/icon0.webp\">\n"
    "<img src=\"https://cdn.orbispatches.com/titles/CUSA00003_" CUSA_HASH "/logo.webp\">";

typedef struct {
	unsigned calls;
} fake_fetch_state;

static int fake_fetch(void *context, raiju_fetch_method method, const char *url, const char *range, size_t max_bytes,
                      int timeout_ms, raiju_fetch_response *response) {
	(void)method;
	(void)range;
	(void)max_bytes;
	(void)timeout_ms;
	fake_fetch_state *state = context;
	state->calls++;
	memset(response, 0, sizeof *response);
	response->content_length = RAIJU_FETCH_NO_LENGTH;
	const char *page = NULL;
	if (strcmp(url, "https://prosperopatches.com/PPSA25380") == 0) {
		page = ps5_page;
	} else if (strcmp(url, "https://orbispatches.com/CUSA00003") == 0) {
		page = ps4_page;
	}
	if (page == NULL) {
		response->status = 404;
		return 0;
	}
	response->status = 200;
	response->body = strdup(page);
	response->body_length = strlen(page);
	return 0;
}

static void page_urls_follow_the_prefix(void) {
	char url[128];
	CHECK(raiju_art_page_url("CUSA00003", url, sizeof url));
	CHECK_STR(url, "https://orbispatches.com/CUSA00003");
	CHECK(raiju_art_page_url("PPSA25380", url, sizeof url));
	CHECK_STR(url, "https://prosperopatches.com/PPSA25380");
	CHECK(!raiju_art_page_url("UNKNOWN", url, sizeof url));
	CHECK(!raiju_art_page_url("PPSA2538", url, sizeof url));
	CHECK(!raiju_art_page_url("PPSA25380/../admin", url, sizeof url));
}

static void parses_a_ps5_page(void) {
	raiju_art *art = raiju_art_parse("PPSA25380", ps5_page, strlen(ps5_page));
	CHECK(art != NULL);
	if (art != NULL) {
		CHECK_STR(art->name, "ELDEN RING NIGHTREIGN");
		CHECK(art->icon != NULL && strstr(art->icon, "PPSA25380_" PPSA_HASH "/icon0.webp") != NULL);
		CHECK(art->background != NULL && strstr(art->background, "/pic0.webp") != NULL);
		CHECK(art->logo == NULL);
		CHECK_STR(art->source, "prosperopatches");
	}
	raiju_art_free(art);
}

static void parses_a_ps4_page_and_decodes_entities(void) {
	raiju_art *art = raiju_art_parse("CUSA00003", ps4_page, strlen(ps4_page));
	CHECK(art != NULL);
	if (art != NULL) {
		CHECK_STR(art->name, "DRIVECLUB\xE2\x84\xA2");
		CHECK(art->logo != NULL && strstr(art->logo, "/logo.webp") != NULL);
		CHECK(art->background == NULL);
		CHECK_STR(art->source, "orbispatches");
	}
	raiju_art_free(art);
}

static void rejects_other_titles_hosts_and_missing_icons(void) {
	CHECK(raiju_art_parse("PPSA99998", ps5_page, strlen(ps5_page)) == NULL);
	const char *spoofed = "<img src=\"https://evil.example/titles/PPSA25380_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	                      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/icon0.webp\">";
	CHECK(raiju_art_parse("PPSA25380", spoofed, strlen(spoofed)) == NULL);
	CHECK(raiju_art_parse("PPSA99999", "<title>PPSA99999</title>", 21) == NULL);
}

static void caches_within_the_ttl(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "art-cache"), 0);
	static const char cached[] = "{\"titleId\":\"PPSA25380\"}";
	CHECK(raiju_art_cache_write(directory, "PPSA25380", cached, sizeof cached - 1));
	uint64_t now = (uint64_t)time(NULL);
	size_t length = 0;
	char *fresh = raiju_art_cache_read(directory, "PPSA25380", now, &length);
	CHECK(fresh != NULL);
	free(fresh);
	CHECK(raiju_art_cache_read(directory, "PPSA25380", now + RAIJU_ART_TTL_SECONDS + 1, &length) == NULL);
	raiju_test_rmtree(directory);
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

static raiju_gateway *make_gateway(raiju_fake_platform *fake, fake_fetch_state *state, char *directory,
                                   size_t capacity) {
	if (raiju_test_tmpdir(directory, capacity, "art-endpoint") != 0) {
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
	config.fetch = fake_fetch;
	config.fetch_context = state;
	return raiju_gateway_create(&config);
}

static void pair_the_gateway(raiju_gateway *gateway, char *cookie, size_t capacity) {
	char body[128];
	snprintf(body, sizeof body, "{\"code\":\"%s\",\"label\":\"test\"}", raiju_gateway_code(gateway));
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/pair";
	request.body = (const unsigned char *)body;
	request.body_length = strlen(body);
	raiju_http_response response;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	const char *token = strstr(raiju_strbuf_cstr(&response.body), "\"token\":\"");
	CHECK(token != NULL);
	char value[80] = {0};
	if (token != NULL) {
		token += strlen("\"token\":\"");
		memcpy(value, token, RAIJU_PAIR_TOKEN_HEX);
	}
	snprintf(cookie, capacity, "raiju_device=%s", value);
	raiju_response_free(&response);
}

static void the_endpoint_serves_and_caches(void) {
	raiju_fake_platform fake;
	fake_fetch_state state;
	memset(&state, 0, sizeof state);
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, &state, directory, sizeof directory);
	CHECK(gateway != NULL);
	char cookie[160];
	pair_the_gateway(gateway, cookie, sizeof cookie);

	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/art/PPSA25380";
	request.cookie = cookie;
	raiju_http_response response;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	CHECK(header_value(&response, "Cache-Control") != NULL);
	CHECK(header_value(&response, "Content-Type") != NULL);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), "ELDEN RING NIGHTREIGN") != NULL);
	raiju_response_free(&response);
	CHECK_INT(state.calls, 1);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/art/PPSA25380";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	raiju_response_free(&response);
	CHECK_INT(state.calls, 1);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/art/PPSA99999";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 404);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), "not_found") != NULL);
	CHECK(header_value(&response, "Cache-Control") != NULL);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/art/lowercase";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 404);
	raiju_response_free(&response);

	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
}

static void the_cache_refuses_a_symlink(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "art-symlink"), 0);
	char target[1200];
	char link[1200];
	snprintf(target, sizeof target, "%s/target.json", directory);
	snprintf(link, sizeof link, "%s/PPSA25380.json", directory);
	FILE *file = fopen(target, "wb");
	CHECK(file != NULL);
	if (file != NULL) {
		fputs("{\"titleId\":\"PPSA25380\"}", file);
		fclose(file);
	}
	unlink(link);
	CHECK_INT(symlink(target, link), 0);
	size_t length = 0;
	char *cached = raiju_art_cache_read(directory, "PPSA25380", 0, &length);
	CHECK(cached == NULL);
	free(cached);
	raiju_test_rmtree(directory);
}

void run_art_tests(void) {
	RUN_TEST(page_urls_follow_the_prefix);
	RUN_TEST(parses_a_ps5_page);
	RUN_TEST(parses_a_ps4_page_and_decodes_entities);
	RUN_TEST(rejects_other_titles_hosts_and_missing_icons);
	RUN_TEST(caches_within_the_ttl);
	RUN_TEST(the_cache_refuses_a_symlink);
	RUN_TEST(the_endpoint_serves_and_caches);
}
