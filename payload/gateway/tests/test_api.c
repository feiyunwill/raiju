#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "api_internal.h"
#include "fake_platform.h"
#include "fixture_server.h"
#include "raiju/gateway.h"
#include "raiju/http.h"
#include "test.h"

static raiju_gateway *make_gateway(raiju_fake_platform *fake, const char *aria2_url, char *directory,
                                   size_t capacity) {
	if (raiju_test_tmpdir(directory, capacity, "api") != 0) {
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
	config.aria2_url = aria2_url;
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
	char value[80] = {0};
	if (token != NULL) {
		token += strlen("\"token\":\"");
		memcpy(value, token, RAIJU_PAIR_TOKEN_HEX);
	}
	snprintf(cookie, capacity, "raiju_device=%s", value);
	raiju_response_free(&response);
}

static void status_is_public_and_reports_engine(void) {
	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, "http://127.0.0.1:6800/jsonrpc", directory, sizeof directory);
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
	CHECK(strstr(body, "\"firmware\":\"unknown\"") != NULL);
	CHECK(strstr(body, "\"engine\":{") != NULL);
	CHECK(strstr(body, "\"up\":") != NULL);
	CHECK(strstr(body, "\"token\"") == NULL);
	CHECK(strstr(body, gateway->pairing.code) == NULL);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/storage";
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 401);
	raiju_response_free(&response);
	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
}

static void storage_reports_the_platform_figures(void) {
	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, "http://127.0.0.1:6800/jsonrpc", directory, sizeof directory);
	CHECK(gateway != NULL);
	char cookie[160];
	pair_the_gateway(gateway, cookie, sizeof cookie);
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/storage";
	request.cookie = cookie;
	raiju_http_response response;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	const char *body = raiju_strbuf_cstr(&response.body);
	CHECK(strstr(body, "\"totalBytes\":1000000000") != NULL);
	CHECK(strstr(body, "\"freeBytes\":500000000") != NULL);
	CHECK(strstr(body, "\"gamesBytes\":") != NULL);
	CHECK(strstr(body, "\"stagingBytes\":") != NULL);
	raiju_response_free(&response);
	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
}

static void folders_browse_only_inside_the_roots(void) {
	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, "http://127.0.0.1:6800/jsonrpc", directory, sizeof directory);
	CHECK(gateway != NULL);
	char root[1200];
	snprintf(root, sizeof root, "%s/games", directory);
	mkdir(root, 0755);
	char subdir[1300];
	snprintf(subdir, sizeof subdir, "%s/Game", root);
	mkdir(subdir, 0755);
	char file[1300];
	snprintf(file, sizeof file, "%s/readme.txt", root);
	FILE *handle = fopen(file, "wb");
	if (handle != NULL) {
		fputs("x", handle);
		fclose(handle);
	}
	raiju_allowlist list;
	memset(&list, 0, sizeof list);
	CHECK(raiju_allowlist_add(&list, root));
	gateway->allowlist = list;
	char cookie[160];
	pair_the_gateway(gateway, cookie, sizeof cookie);
	char query[1500];
	snprintf(query, sizeof query, "path=%s", root);
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/folders";
	request.query = query;
	request.cookie = cookie;
	raiju_http_response response;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	const char *body = raiju_strbuf_cstr(&response.body);
	CHECK(strstr(body, "\"Game\"") != NULL);
	CHECK(strstr(body, "\"readme.txt\"") != NULL);
	CHECK(strstr(body, "\"directory\":true") != NULL);
	CHECK(strstr(body, "\"roots\":[") != NULL);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/folders";
	request.query = "path=/etc";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 403);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/folders";
	request.query = "path=/data/raiju/../secret";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 403);
	raiju_response_free(&response);

	char escape[1300];
	snprintf(escape, sizeof escape, "%s/escape", root);
	unlink(escape);
	if (symlink("/etc", escape) == 0) {
		char target[1400];
		snprintf(target, sizeof target, "path=%s/escape/passwd", root);
		memset(&request, 0, sizeof request);
		request.method = "GET";
		request.path = "/api/folders";
		request.query = target;
		request.cookie = cookie;
		raiju_response_init(&response);
		CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 403);
		raiju_response_free(&response);
	}

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/folders";
	request.query = "path=/data/homebrew/nope";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 404);
	raiju_response_free(&response);

	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
}

static void the_stage_status_mapping_is_stable(void) {
	struct {
		raiju_stage stage;
		raiju_stage failed;
		const char *ui_stage;
		const char *status;
	} cases[] = {
	    {RAIJU_STAGE_DOWNLOADING, RAIJU_STAGE_DOWNLOADING, "download", "active"},
	    {RAIJU_STAGE_VERIFYING, RAIJU_STAGE_VERIFYING, "verify", "active"},
	    {RAIJU_STAGE_EXTRACTING, RAIJU_STAGE_EXTRACTING, "extract", "active"},
	    {RAIJU_STAGE_INSTALLING, RAIJU_STAGE_INSTALLING, "install", "active"},
	    {RAIJU_STAGE_AWAITING_TITLE_ID, RAIJU_STAGE_AWAITING_TITLE_ID, "verify", "waiting"},
	    {RAIJU_STAGE_CONFLICT, RAIJU_STAGE_CONFLICT, "install", "waiting"},
	    {RAIJU_STAGE_READY, RAIJU_STAGE_READY, "ready", "done"},
	    {RAIJU_STAGE_CANCELLED, RAIJU_STAGE_CANCELLED, "ready", "failed"},
	    {RAIJU_STAGE_FAILED, RAIJU_STAGE_EXTRACTING, "extract", "failed"},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		raiju_job job;
		memset(&job, 0, sizeof job);
		snprintf(job.id, sizeof job.id, "job-1");
		job.stage = cases[i].stage;
		job.failed_stage = cases[i].failed;
		raiju_strbuf document;
		raiju_strbuf_init(&document);
		raiju_api_job_json(&document, &job);
		const char *text = raiju_strbuf_cstr(&document);
		CHECK(strstr(text, raiju_stage_name(cases[i].stage)) != NULL);
		char needle[64];
		snprintf(needle, sizeof needle, "\"uiStage\":\"%s\"", cases[i].ui_stage);
		CHECK(strstr(text, needle) != NULL);
		snprintf(needle, sizeof needle, "\"status\":\"%s\"", cases[i].status);
		CHECK(strstr(text, needle) != NULL);
		raiju_strbuf_free(&document);
	}
}

static void jobs_create_list_get_and_refuse(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	raiju_fixture_server_set_jsonrpc(server, "{\"jsonrpc\":\"2.0\",\"id\":\"raiju\",\"result\":\"gid1\"}");
	char aria2_url[128];
	snprintf(aria2_url, sizeof aria2_url, "http://127.0.0.1:%u/jsonrpc", raiju_fixture_server_port(server));
	raiju_fake_platform fake;
	fake.free_bytes = 100ull * 1024 * 1024 * 1024;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, aria2_url, directory, sizeof directory);
	CHECK(gateway != NULL);
	fake.free_bytes = 100ull * 1024 * 1024 * 1024;
	char cookie[160];
	pair_the_gateway(gateway, cookie, sizeof cookie);
	raiju_http_request request;
	raiju_http_response response;
	memset(&response, 0, sizeof response);

	const char *create = "{\"id\":\"gid1\",\"uri\":\"http://example/x.zip\",\"destination\":\"/data/homebrew\","
	                     "\"kind\":\"zip\",\"size\":100000}";
	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/jobs";
	request.cookie = cookie;
	request.body = (const unsigned char *)create;
	request.body_length = strlen(create);
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 201);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), "\"id\":\"gid1\"") != NULL);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), "\"stage\":\"downloading\"") != NULL);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/jobs";
	request.cookie = cookie;
	request.body = (const unsigned char *)create;
	request.body_length = strlen(create);
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 409);
	raiju_response_free(&response);

	const char *bad = "{\"id\":\"../escape\",\"uri\":\"http://example/x.zip\"}";
	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/jobs";
	request.cookie = cookie;
	request.body = (const unsigned char *)bad;
	request.body_length = strlen(bad);
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 400);
	raiju_response_free(&response);

	const char *outside = "{\"id\":\"gid2\",\"uri\":\"http://example/x.zip\",\"destination\":\"/etc\"}";
	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/jobs";
	request.cookie = cookie;
	request.body = (const unsigned char *)outside;
	request.body_length = strlen(outside);
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 403);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/jobs";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), "\"gid1\"") != NULL);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), "\"uiStage\":\"download\"") != NULL);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/jobs/gid1";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/jobs/missing";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 404);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/jobs/gid1/retry";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 409);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/jobs/gid1/cancel";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), "\"stage\":\"cancelled\"") != NULL);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/jobs/missing/cancel";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 404);
	raiju_response_free(&response);

	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
	raiju_fixture_server_stop(server);
}

static void every_new_endpoint_needs_a_token(void) {
	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, "http://127.0.0.1:6800/jsonrpc", directory, sizeof directory);
	CHECK(gateway != NULL);
	const char *protected_paths[] = {"/api/storage", "/api/folders", "/api/jobs", "/api/jobs/x"};
	const char *methods[] = {"GET", "GET", "GET", "GET"};
	for (size_t i = 0; i < sizeof protected_paths / sizeof protected_paths[0]; i++) {
		raiju_http_request request;
		memset(&request, 0, sizeof request);
		request.method = methods[i];
		request.path = protected_paths[i];
		raiju_http_response response;
		raiju_response_init(&response);
		CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 401);
		raiju_response_free(&response);
	}
	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
}

/**
 * Reads the whole job list on its own thread while the main thread holds
 * queue_lock, so a read handler that still took that lock would hang here.
 */
static void *jobs_read_thread(void *argument) {
	struct {
		raiju_gateway *gateway;
		char cookie[160];
		volatile int done;
		int status;
		char body[2048];
	} *args = argument;
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/jobs";
	request.cookie = args->cookie;
	raiju_http_response response;
	raiju_response_init(&response);
	args->status = raiju_gateway_handle(args->gateway, &request, &response);
	snprintf(args->body, sizeof args->body, "%s", raiju_strbuf_cstr(&response.body));
	raiju_response_free(&response);
	__atomic_store_n(&args->done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

static void jobs_reads_never_wait_for_the_queue(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	raiju_fixture_server_set_jsonrpc(server, "{\"jsonrpc\":\"2.0\",\"id\":\"raiju\",\"result\":\"gid1\"}");
	char aria2_url[128];
	snprintf(aria2_url, sizeof aria2_url, "http://127.0.0.1:%u/jsonrpc", raiju_fixture_server_port(server));
	raiju_fake_platform fake;
	fake.free_bytes = 100ull * 1024 * 1024 * 1024;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, aria2_url, directory, sizeof directory);
	CHECK(gateway != NULL);
	fake.free_bytes = 100ull * 1024 * 1024 * 1024;
	char cookie[160];
	pair_the_gateway(gateway, cookie, sizeof cookie);
	raiju_http_request request;
	raiju_http_response response;
	const char *create = "{\"id\":\"gid1\",\"uri\":\"http://example/x.zip\",\"destination\":\"/data/homebrew\","
	                     "\"kind\":\"zip\",\"size\":100000}";
	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/jobs";
	request.cookie = cookie;
	request.body = (const unsigned char *)create;
	request.body_length = strlen(create);
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 201);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/jobs/gid1";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), "\"stage\":\"downloading\"") != NULL);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/jobs/gid1/cancel";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	raiju_response_free(&response);

	memset(&request, 0, sizeof request);
	request.method = "GET";
	request.path = "/api/jobs/gid1";
	request.cookie = cookie;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), "\"stage\":\"cancelled\"") != NULL);
	CHECK(strstr(raiju_strbuf_cstr(&response.body), "\"status\":\"failed\"") != NULL);
	raiju_response_free(&response);

	raiju_api *api = gateway->api;
	CHECK(api != NULL);
	struct {
		raiju_gateway *gateway;
		char cookie[160];
		volatile int done;
		int status;
		char body[2048];
	} args;
	memset(&args, 0, sizeof args);
	args.gateway = gateway;
	snprintf(args.cookie, sizeof args.cookie, "%s", cookie);
	pthread_mutex_lock(&api->queue_lock);
	pthread_t thread;
	CHECK_INT(pthread_create(&thread, NULL, jobs_read_thread, &args), 0);
	bool completed = false;
	for (int i = 0; i < 2000 && !completed; i++) {
		if (__atomic_load_n(&args.done, __ATOMIC_SEQ_CST)) {
			completed = true;
			break;
		}
		struct timespec pause = {0, 1000 * 1000};
		nanosleep(&pause, NULL);
	}
	CHECK(completed);
	pthread_mutex_unlock(&api->queue_lock);
	pthread_join(thread, NULL);
	CHECK_INT(args.status, 200);
	CHECK(strstr(args.body, "\"gid1\"") != NULL);

	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
	raiju_fixture_server_stop(server);
}

void run_api_tests(void) {
	RUN_TEST(status_is_public_and_reports_engine);
	RUN_TEST(storage_reports_the_platform_figures);
	RUN_TEST(folders_browse_only_inside_the_roots);
	RUN_TEST(the_stage_status_mapping_is_stable);
	RUN_TEST(jobs_create_list_get_and_refuse);
	RUN_TEST(every_new_endpoint_needs_a_token);
	RUN_TEST(jobs_reads_never_wait_for_the_queue);
}
