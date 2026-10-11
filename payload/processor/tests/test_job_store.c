#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fixtures.h"
#include "raiju/job_store.h"
#include "raiju/processor.h"
#include "test.h"

#define VALID_JOB                                                                               \
	"{\"id\":\"a\",\"stage\":\"installing\",\"sourcePath\":\"/data/raiju/downloads/x.zip\","     \
	"\"fileName\":\"x.zip\",\"installRoot\":\"/data/homebrew\",\"payloadType\":\"folder\","      \
	"\"payloadPath\":\"/data/raiju/staging/a/Game\",\"titleId\":\"PPSA24003\",\"kind\":\"zip\"}"

/**
 * Builds a job with every field set to a distinctive value.
 */
static raiju_job sample_job(const char *id) {
	raiju_job job;
	memset(&job, 0, sizeof job);
	snprintf(job.id, sizeof job.id, "%s", id);
	job.stage = RAIJU_STAGE_FAILED;
	job.failed_stage = RAIJU_STAGE_RESCAN;
	snprintf(job.error, sizeof job.error, "rescan_failed: \"quoted\" \\ back\x01slash");
	snprintf(job.source_path, sizeof job.source_path, "/data/raiju/downloads/Jeu \xc3\xa9t\xc3\xa9.7z");
	snprintf(job.file_name, sizeof job.file_name, "Jeu \xc3\xa9t\xc3\xa9.7z");
	job.expected_size = UINT64_MAX;
	job.uncompressed_size = 123456789012ull;
	snprintf(job.install_root, sizeof job.install_root, "/data/etaHEN/games");
	job.keep_archive = true;
	job.conflict_policy = RAIJU_CONFLICT_REPLACE;
	job.kind = RAIJU_KIND_7Z;
	job.payload_type = RAIJU_PAYLOAD_IMAGE;
	snprintf(job.payload_path, sizeof job.payload_path, "/data/raiju/staging/%s/Game.ffpkg", id);
	snprintf(job.title_id, sizeof job.title_id, "PPSA24003");
	job.title_source = RAIJU_TITLE_SOURCE_FILE_NAME;
	job.names_disagree = true;
	snprintf(job.install_path, sizeof job.install_path, "/data/etaHEN/games/PPSA24003-2.ffpkg");
	job.conflict_choice = RAIJU_CONFLICT_RENAME;
	job.progress_done = 7;
	job.progress_total = 9;
	return job;
}

/**
 * Wraps one job object in a version 1 document and parses it.
 */
static raiju_store_status parse_single(const char *job_json) {
	char document[4096];
	snprintf(document, sizeof document, "{\"version\":1,\"jobs\":[%s]}", job_json);
	raiju_job_list list;
	raiju_store_status status = raiju_jobs_from_json(document, strlen(document), &list);
	raiju_job_list_free(&list);
	return status;
}

/**
 * Parses a whole document.
 */
static raiju_store_status parse_document(const char *document) {
	raiju_job_list list;
	raiju_store_status status = raiju_jobs_from_json(document, strlen(document), &list);
	raiju_job_list_free(&list);
	return status;
}

static void jobs_round_trip_through_json(void) {
	raiju_job_list list = {NULL, 0, 0};
	raiju_job first = sample_job("job-1");
	snprintf(first.display_name, sizeof first.display_name, "Elden Ring Nightreign");
	raiju_job second = sample_job("job_2");
	second.stage = RAIJU_STAGE_DOWNLOADING;
	second.failed_stage = RAIJU_STAGE_DOWNLOADING;
	CHECK(raiju_job_list_add(&list, &first) != NULL);
	CHECK(raiju_job_list_add(&list, &second) != NULL);
	size_t length = 0;
	char *json = raiju_jobs_to_json(&list, &length);
	CHECK(json != NULL && length == strlen(json));
	raiju_job_list parsed;
	CHECK_INT(raiju_jobs_from_json(json, length, &parsed), RAIJU_STORE_OK);
	CHECK_INT(parsed.count, 2);
	if (parsed.count == 2) {
		CHECK(memcmp(&parsed.jobs[0], &first, sizeof first) == 0);
		CHECK(memcmp(&parsed.jobs[1], &second, sizeof second) == 0);
	}
	CHECK(json != NULL && strstr(json, "\\u0001") != NULL);
	free(json);
	raiju_job_list_free(&parsed);
	raiju_job_list_free(&list);
}

static void list_operations_find_and_remove(void) {
	raiju_job_list list = {NULL, 0, 0};
	for (int i = 0; i < 20; i++) {
		char id[16];
		snprintf(id, sizeof id, "j%d", i);
		raiju_job job = sample_job(id);
		CHECK(raiju_job_list_add(&list, &job) != NULL);
	}
	CHECK(raiju_job_list_find(&list, "j7") != NULL);
	CHECK(raiju_job_list_remove(&list, "j7"));
	CHECK(!raiju_job_list_remove(&list, "j7"));
	CHECK(raiju_job_list_find(&list, "j7") == NULL);
	CHECK(raiju_job_list_find(&list, "j19") != NULL);
	CHECK_INT(list.count, 19);
	raiju_job_list_free(&list);
	CHECK_INT(list.count, 0);
}

static void well_formed_documents_load(void) {
	CHECK_INT(parse_document("{\"version\":1,\"jobs\":[]}"), RAIJU_STORE_OK);
	CHECK_INT(parse_document(" \n{ \"jobs\" : [ ] , \"version\" : 1 }\n "), RAIJU_STORE_OK);
	CHECK_INT(parse_single(VALID_JOB), RAIJU_STORE_OK);
	CHECK_INT(parse_document("{\"version\":1,\"future\":{\"a\":[1,2.5e3,null,true]},\"jobs\":[]}"), RAIJU_STORE_OK);
	CHECK_INT(parse_single("{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r\",\"newField\":[{}]}"),
	          RAIJU_STORE_OK);
	CHECK_INT(parse_single("{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r\",\"error\":\"\\u00e9\\ud83d"
	                       "\\ude00\"}"),
	          RAIJU_STORE_OK);
}

static void malformed_documents_are_rejected(void) {
	const char *const malformed[] = {
	    "",
	    "[]",
	    "{\"version\":1}",
	    "{\"jobs\":[]}",
	    "{\"version\":1,\"jobs\":[]",
	    "{\"version\":1,\"jobs\":[]}x",
	    "{\"version\":1,\"jobs\":[],}",
	    "{\"version\":1,\"version\":1,\"jobs\":[]}",
	    "{\"version\":1,\"jobs\":[],\"jobs\":[]}",
	    "{\"version\":-1,\"jobs\":[]}",
	    "{\"version\":1.0,\"jobs\":[]}",
	    "{\"version\":01,\"jobs\":[]}",
	    "{\"version\":1,\"jobs\":[{\"id\":\"a\",}]}",
	    "{\"version\":1,\"x\":[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]],\"jobs\":[]}",
	    "{\"version\":1,\"x\":\"unterminated,\"jobs\":[]}",
	    "{\"version\":1,\"x\":\"bad \\q escape\",\"jobs\":[]}",
	    "{\"version\":1,\"x\":\"lone \\udc00\",\"jobs\":[]}",
	    "{\"version\":1,\"x\":tru,\"jobs\":[]}",
	};
	for (size_t i = 0; i < sizeof malformed / sizeof malformed[0]; i++) {
		raiju_store_status status = parse_document(malformed[i]);
		if (status != RAIJU_STORE_MALFORMED) {
			fprintf(stderr, "    accepted malformed document %zu: %s\n", i, raiju_store_status_name(status));
		}
		CHECK_INT(status, RAIJU_STORE_MALFORMED);
	}
	char raw[] = "{\"version\":1,\"jobs\":[]}";
	raw[2] = '\0';
	raiju_job_list list;
	CHECK_INT(raiju_jobs_from_json(raw, sizeof raw - 1, &list), RAIJU_STORE_MALFORMED);
	CHECK_INT(parse_document("{\"version\":2,\"jobs\":[]}"), RAIJU_STORE_INVALID_JOB);
}

static void invalid_jobs_are_rejected(void) {
	const char *const invalid[] = {
	    "{\"stage\":\"downloading\",\"installRoot\":\"/r\"}",
	    "{\"id\":\"a\",\"installRoot\":\"/r\"}",
	    "{\"id\":\"a\",\"stage\":\"downloading\"}",
	    "{\"id\":\"../a\",\"stage\":\"downloading\",\"installRoot\":\"/r\"}",
	    "{\"id\":\"\",\"stage\":\"downloading\",\"installRoot\":\"/r\"}",
	    "{\"id\":\"a\",\"stage\":\"exploding\",\"installRoot\":\"/r\"}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"r\"}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r/../etc\"}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r//x\"}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r/\"}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r\\u0000x\"}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r\",\"titleId\":\"PPSA2400\"}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r\",\"fileName\":\"a/b\"}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r\",\"expectedSize\":-1}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r\",\"expectedSize\":18446744073709551616}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r\",\"keepArchive\":1}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r\",\"kind\":\"tarball\"}",
	    "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r\",\"conflictPolicy\":\"cancel\"}",
	    "{\"id\":\"a\",\"stage\":\"failed\",\"failedStage\":\"ready\",\"installRoot\":\"/r\"}",
	    "{\"id\":\"a\",\"stage\":\"verifying\",\"installRoot\":\"/r\"}",
	    "{\"id\":\"a\",\"stage\":\"installing\",\"sourcePath\":\"/d/x\",\"fileName\":\"x\",\"installRoot\":\"/r\"}",
	    "{\"id\":\"a\",\"stage\":\"rescan\",\"sourcePath\":\"/d/x\",\"fileName\":\"x\",\"installRoot\":\"/r\","
	    "\"payloadType\":\"file\",\"payloadPath\":\"/d/x\"}",
	    "{\"id\":\"a\",\"stage\":\"rescan\",\"sourcePath\":\"/d/x\",\"fileName\":\"x\",\"installRoot\":\"/r\","
	    "\"payloadType\":\"file\",\"payloadPath\":\"/d/x\",\"installPath\":\"/etc/x\"}",
	    "{\"id\":\"a\",\"stage\":\"rescan\",\"sourcePath\":\"/d/x\",\"fileName\":\"x\",\"installRoot\":\"/r\","
	    "\"payloadType\":\"file\",\"payloadPath\":\"/d/x\",\"installPath\":\"/r/a/b\"}",
	    "{\"id\":\"a\",\"stage\":\"installing\",\"sourcePath\":\"/d/x\",\"fileName\":\"x\",\"installRoot\":\"/r\","
	    "\"payloadType\":\"folder\",\"payloadPath\":\"/s/a\"}",
	};
	for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
		raiju_store_status status = parse_single(invalid[i]);
		if (status == RAIJU_STORE_OK) {
			fprintf(stderr, "    accepted invalid job %zu\n", i);
		}
		CHECK(status == RAIJU_STORE_INVALID_JOB || status == RAIJU_STORE_MALFORMED);
	}
	CHECK_INT(parse_single("{\"id\":\"a\",\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"/r\"}"),
	          RAIJU_STORE_MALFORMED);
	CHECK_INT(parse_single("{\"id\":\"a\",\"stage\":\"rescan\",\"sourcePath\":\"/d/x\",\"fileName\":\"x\","
	                       "\"installRoot\":\"/\",\"payloadType\":\"file\",\"payloadPath\":\"/d/x\","
	                       "\"installPath\":\"/x\"}"),
	          RAIJU_STORE_OK);
	CHECK_INT(parse_single(VALID_JOB "," VALID_JOB), RAIJU_STORE_INVALID_JOB);
}

static void oversized_inputs_are_rejected(void) {
	size_t length = RAIJU_JOBS_FILE_MAX_BYTES + 1;
	char *big = malloc(length);
	CHECK(big != NULL);
	if (big != NULL) {
		memset(big, ' ', length);
		raiju_job_list list;
		CHECK_INT(raiju_jobs_from_json(big, length, &list), RAIJU_STORE_TOO_LARGE);
		free(big);
	}
	char long_path[RAIJU_JOB_PATH_MAX + 64];
	memset(long_path, 'a', sizeof long_path);
	long_path[0] = '/';
	long_path[sizeof long_path - 1] = '\0';
	char job[sizeof long_path + 128];
	snprintf(job, sizeof job, "{\"id\":\"a\",\"stage\":\"downloading\",\"installRoot\":\"%s\"}", long_path);
	char *document = malloc(sizeof job + 64);
	snprintf(document, sizeof job + 64, "{\"version\":1,\"jobs\":[%s]}", job);
	CHECK_INT(parse_document(document), RAIJU_STORE_INVALID_JOB);
	free(document);
}

static void save_is_atomic_and_load_reads_it_back(void) {
	char *dir = fixture_temp_dir();
	char *path = fixture_path(dir, "jobs.json");
	char *temp = fixture_path(dir, "jobs.json.tmp");
	raiju_job_list list = {NULL, 0, 0};
	raiju_job_list loaded;
	CHECK_INT(raiju_jobs_load(path, &loaded), RAIJU_STORE_MISSING);
	CHECK_INT(loaded.count, 0);
	raiju_job job = sample_job("one");
	CHECK(raiju_job_list_add(&list, &job) != NULL);
	CHECK_INT(raiju_jobs_save(&list, path), RAIJU_STORE_OK);
	struct stat info;
	CHECK(lstat(temp, &info) != 0);
	CHECK_INT(raiju_jobs_load(path, &loaded), RAIJU_STORE_OK);
	CHECK_INT(loaded.count, 1);
	raiju_job_list_free(&loaded);

	CHECK(fixture_write(temp, "stale half write", 16) == 0);
	job = sample_job("two");
	CHECK(raiju_job_list_add(&list, &job) != NULL);
	CHECK_INT(raiju_jobs_save(&list, path), RAIJU_STORE_OK);
	CHECK(lstat(temp, &info) != 0);
	CHECK_INT(raiju_jobs_load(path, &loaded), RAIJU_STORE_OK);
	CHECK_INT(loaded.count, 2);
	raiju_job_list_free(&loaded);

	char *missing_dir = fixture_path(dir, "missing/jobs.json");
	CHECK_INT(raiju_jobs_save(&list, missing_dir), RAIJU_STORE_IO_ERROR);
	CHECK_INT(raiju_jobs_load(path, &loaded), RAIJU_STORE_OK);
	CHECK_INT(loaded.count, 2);
	raiju_job_list_free(&loaded);
	free(missing_dir);

	raiju_job_list_free(&list);
	unlink(path);
	free(path);
	free(temp);
	rmdir(dir);
	free(dir);
}

static void a_corrupt_jobs_file_is_moved_aside(void) {
	char *dir = fixture_temp_dir();
	char *path = fixture_path(dir, "jobs.json");
	char *aside = fixture_path(dir, "jobs.json.bad");
	CHECK(fixture_write(path, "{\"version\":1,\"jobs\":[{\"id\":", 27) == 0);
	raiju_pipeline pipeline = {dir, dir, NULL, NULL, NULL, NULL, NULL};
	raiju_processor processor;
	CHECK_INT(raiju_processor_open(&processor, &pipeline, path), RAIJU_PROCESSOR_OK);
	CHECK(processor.load_status != RAIJU_STORE_OK && processor.load_status != RAIJU_STORE_MISSING);
	CHECK_INT(processor.list.count, 0);
	struct stat info;
	CHECK(lstat(aside, &info) == 0);
	CHECK(lstat(path, &info) != 0);
	raiju_processor_close(&processor);
	CHECK_STR(raiju_store_status_name(RAIJU_STORE_INVALID_JOB), "invalid_job");
	unlink(aside);
	free(path);
	free(aside);
	rmdir(dir);
	free(dir);
}

void run_job_store_tests(void) {
	RUN_TEST(jobs_round_trip_through_json);
	RUN_TEST(list_operations_find_and_remove);
	RUN_TEST(well_formed_documents_load);
	RUN_TEST(malformed_documents_are_rejected);
	RUN_TEST(invalid_jobs_are_rejected);
	RUN_TEST(oversized_inputs_are_rejected);
	RUN_TEST(save_is_atomic_and_load_reads_it_back);
	RUN_TEST(a_corrupt_jobs_file_is_moved_aside);
}
