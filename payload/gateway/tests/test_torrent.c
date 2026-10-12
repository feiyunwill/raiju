#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <stdio.h>
#include <string.h>

#include "raiju/job.h"
#include "raiju/torrent.h"
#include "test.h"

#define TORRENT_FIXTURE "torrent/PPSA24003-dump.torrent.b64"

/** Reads a text fixture under tests/fixtures, without its trailing newline. */
static bool read_fixture(const char *name, char *out, size_t capacity) {
	char path[1024];
	size_t length;
	FILE *file;

	if (snprintf(path, sizeof path, "%s/%s", RAIJU_TEST_FIXTURE_DIR, name) >= (int)sizeof path) {
		return false;
	}
	file = fopen(path, "rb");
	if (file == NULL) {
		return false;
	}
	length = fread(out, 1, capacity - 1, file);
	fclose(file);
	out[length] = '\0';
	while (length > 0 && (out[length - 1] == '\n' || out[length - 1] == '\r')) {
		out[--length] = '\0';
	}
	return length > 0;
}

static void the_torrent_names_itself(void) {
	char text[8192];
	char name[RAIJU_JOB_NAME_MAX];
	CHECK(read_fixture(TORRENT_FIXTURE, text, sizeof text));
	CHECK(raiju_torrent_name_from_base64(text, strlen(text), name, sizeof name));
	CHECK_STR(name, "PPSA24003-dump");
}

static void refuses_text_that_is_not_base64(void) {
	char name[RAIJU_JOB_NAME_MAX];
	CHECK(!raiju_torrent_name_from_base64("not base64!", strlen("not base64!"), name, sizeof name));
	CHECK(!raiju_torrent_name_from_base64("AAAAA", strlen("AAAAA"), name, sizeof name));
	CHECK(!raiju_torrent_name_from_base64("", 0, name, sizeof name));
	CHECK(!raiju_torrent_name_from_base64("ZDQ6=W5mb2Rl", strlen("ZDQ6=W5mb2Rl"), name, sizeof name));
}

static void refuses_a_torrent_without_a_name(void) {
	char name[RAIJU_JOB_NAME_MAX];
	const char *no_info = "ZDQ6bmFtZTQ6dGVzdGU=";
	const char *info_without_name = "ZDQ6aW5mb2Rl";
	const char *name_is_an_integer = "ZDQ6aW5mb2Q0Om5hbWVpN2VlZQ==";
	CHECK(!raiju_torrent_name_from_base64(no_info, strlen(no_info), name, sizeof name));
	CHECK(!raiju_torrent_name_from_base64(info_without_name, strlen(info_without_name), name, sizeof name));
	CHECK(!raiju_torrent_name_from_base64(name_is_an_integer, strlen(name_is_an_integer), name, sizeof name));
}

static void refuses_a_name_that_does_not_fit(void) {
	char text[8192];
	char name[4];
	CHECK(read_fixture(TORRENT_FIXTURE, text, sizeof text));
	CHECK(!raiju_torrent_name_from_base64(text, strlen(text), name, sizeof name));
	CHECK(!raiju_torrent_name_from_base64(text, strlen(text), NULL, 0));
}

void run_torrent_tests(void) {
	RUN_TEST(the_torrent_names_itself);
	RUN_TEST(refuses_text_that_is_not_base64);
	RUN_TEST(refuses_a_torrent_without_a_name);
	RUN_TEST(refuses_a_name_that_does_not_fit);
}
