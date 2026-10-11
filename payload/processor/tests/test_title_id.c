#include <stdlib.h>

#include "fixtures.h"
#include "raiju/title_id.h"
#include "test.h"

static const char realistic_param_json[] =
	"{\r\n"
	"  \"ageLevel\": {\"default\": 12, \"US\": 13},\r\n"
	"  \"applicationCategoryType\": 0,\r\n"
	"  \"contentId\": \"EP0000-PPSA24003_00-SOMEGAMENAME0000\",\r\n"
	"  \"contentVersion\": \"01.000.000\",\r\n"
	"  \"downloadDataSize\": 1.5e3,\r\n"
	"  \"kernel\": {\"cpuPageTableSize\": -0, \"flexibleMemorySize\": 0.25, \"gpuPageTableSize\": 0},\r\n"
	"  \"localizedParameters\": {\r\n"
	"    \"defaultLanguage\": \"en-US\",\r\n"
	"    \"en-US\": {\"titleName\": \"Some \\\"Game\\\" \\u00e9\\ud83c\\udfae\"},\r\n"
	"    \"fr-FR\": {\"titleName\": \"Un jeu\", \"titleId\": \"CUSA99999\"}\r\n"
	"  },\r\n"
	"  \"pubtools\": {\"toolVersion\": \"1.0\", \"submission\": true, \"debug\": false, \"extra\": null},\r\n"
	"  \"addcont\": {\"serviceIdForSharing\": [\"EP0000-PPSA24003_00\", [], [1, 2.0, -3E+2]]},\r\n"
	"  \"titleId\": \"PPSA24003\",\r\n"
	"  \"versionFileUri\": \"http:\\/\\/example.invalid\\/v.xml\"\r\n"
	"}\r\n";

/**
 * Parses a NUL-terminated param.json string and returns its status, writing
 * the title ID into out.
 */
static raiju_param_status parse(const char *json, char out[RAIJU_TITLE_ID_LEN + 1]) {
	return raiju_title_id_from_param_json(json, strlen(json), out);
}

static void title_id_is_read_from_realistic_param_json(void) {
	char id[RAIJU_TITLE_ID_LEN + 1] = "";
	CHECK_INT(parse(realistic_param_json, id), RAIJU_PARAM_OK);
	CHECK_STR(id, "PPSA24003");
}

static void minimal_and_spaced_documents_parse(void) {
	char id[RAIJU_TITLE_ID_LEN + 1] = "";
	CHECK_INT(parse("{\"titleId\":\"CUSA12345\"}", id), RAIJU_PARAM_OK);
	CHECK_STR(id, "CUSA12345");
	CHECK_INT(parse(" \t\n{ \"titleId\" \n : \t \"PPSA00001\" \r\n} \n", id), RAIJU_PARAM_OK);
	CHECK_STR(id, "PPSA00001");
}

static void byte_order_mark_is_skipped(void) {
	char id[RAIJU_TITLE_ID_LEN + 1] = "";
	CHECK_INT(parse("\xEF\xBB\xBF{\"titleId\": \"PPSA24003\"}", id), RAIJU_PARAM_OK);
	CHECK_STR(id, "PPSA24003");
}

static void escapes_in_key_and_value_are_decoded(void) {
	char id[RAIJU_TITLE_ID_LEN + 1] = "";
	CHECK_INT(parse("{\"title\\u0049d\": \"PPSA\\u00302003\"}", id), RAIJU_PARAM_OK);
	CHECK_STR(id, "PPSA02003");
	CHECK_INT(parse("{\"titleId\\u0000\": \"PPSA24003\"}", id), RAIJU_PARAM_NO_TITLE_ID);
}

static void nested_title_id_is_ignored(void) {
	char id[RAIJU_TITLE_ID_LEN + 1] = "";
	CHECK_INT(parse("{\"localizedParameters\": {\"titleId\": \"PPSA24003\"}}", id), RAIJU_PARAM_NO_TITLE_ID);
	CHECK_INT(parse("{\"list\": [{\"titleId\": \"PPSA24003\"}]}", id), RAIJU_PARAM_NO_TITLE_ID);
	CHECK_INT(parse("{}", id), RAIJU_PARAM_NO_TITLE_ID);
	CHECK_INT(parse("{\"titleid\": \"PPSA24003\"}", id), RAIJU_PARAM_NO_TITLE_ID);
}

static void malformed_documents_are_rejected(void) {
	static const char *const documents[] = {
		"",
		"   ",
		"[\"titleId\", \"PPSA24003\"]",
		"\"PPSA24003\"",
		"{\"titleId\": \"PPSA24003\",}",
		"{\"titleId\": \"PPSA24003\"",
		"{\"titleId\": \"PPSA24003}",
		"{\"titleId\": \"PPSA24003\"} trailing",
		"{\"titleId\": \"PPSA24003\"}{}",
		"{'titleId': 'PPSA24003'}",
		"{titleId: \"PPSA24003\"}",
		"{\"titleId\" \"PPSA24003\"}",
		"{\"a\": 01, \"titleId\": \"PPSA24003\"}",
		"{\"a\": 1., \"titleId\": \"PPSA24003\"}",
		"{\"a\": -, \"titleId\": \"PPSA24003\"}",
		"{\"a\": 1e, \"titleId\": \"PPSA24003\"}",
		"{\"a\": +1, \"titleId\": \"PPSA24003\"}",
		"{\"a\": tru, \"titleId\": \"PPSA24003\"}",
		"{\"a\": [1 2], \"titleId\": \"PPSA24003\"}",
		"{\"a\": \"tab\there\", \"titleId\": \"PPSA24003\"}",
		"{\"a\": \"\\x41\", \"titleId\": \"PPSA24003\"}",
		"{\"a\": \"\\u12\", \"titleId\": \"PPSA24003\"}",
		"{\"a\": \"\\ud83c\", \"titleId\": \"PPSA24003\"}",
		"{\"a\": \"\\udfae\", \"titleId\": \"PPSA24003\"}",
		"{\"a\": \"\\ud83c\\u0041\", \"titleId\": \"PPSA24003\"}",
		"{\"titleId\": \"PPSA24003\", \"titleId\": \"PPSA24003\"}",
		"{\"titleId\": \"PPSA24003\", \"title\\u0049d\": \"CUSA00001\"}",
		"{\"a\": \"unterminated\\",
	};
	for (size_t i = 0; i < sizeof documents / sizeof documents[0]; i++) {
		char id[RAIJU_TITLE_ID_LEN + 1] = "";
		raiju_param_status status = parse(documents[i], id);
		if (status != RAIJU_PARAM_MALFORMED) {
			fprintf(stderr, "document %zu was not rejected: %s\n", i, documents[i]);
		}
		CHECK_INT(status, RAIJU_PARAM_MALFORMED);
		CHECK_STR(id, "");
	}
}

static void embedded_nul_byte_is_rejected(void) {
	static const char document[] = "{\"titleId\": \"PPSA24003\"}\0{}";
	char id[RAIJU_TITLE_ID_LEN + 1] = "";
	CHECK_INT(raiju_title_id_from_param_json(document, sizeof document - 1, id), RAIJU_PARAM_MALFORMED);
	static const char in_string[] = "{\"titleId\": \"PPSA2\0003\"}";
	CHECK_INT(raiju_title_id_from_param_json(in_string, sizeof in_string - 1, id), RAIJU_PARAM_MALFORMED);
}

static void deep_nesting_is_rejected(void) {
	char document[512];
	size_t length = 0;
	document[length++] = '{';
	const char *key = "\"a\":";
	for (int i = 0; i < 100; i++) {
		memcpy(document + length, key, 4);
		length += 4;
		document[length++] = '[';
	}
	char id[RAIJU_TITLE_ID_LEN + 1] = "";
	CHECK_INT(raiju_title_id_from_param_json(document, length, id), RAIJU_PARAM_MALFORMED);
}

static void unusable_title_id_values_are_rejected(void) {
	static const char *const documents[] = {
		"{\"titleId\": 24003}",
		"{\"titleId\": null}",
		"{\"titleId\": [\"PPSA24003\"]}",
		"{\"titleId\": \"\"}",
		"{\"titleId\": \"ppsa24003\"}",
		"{\"titleId\": \"PPSA2400\"}",
		"{\"titleId\": \"PPSA240033\"}",
		"{\"titleId\": \"../../etc\"}",
		"{\"titleId\": \"PPSA/4003\"}",
		"{\"titleId\": \"PPSA24003PPSA24003PPSA24003\"}",
		"{\"titleId\": \"PPS\\u00c124003\"}",
	};
	for (size_t i = 0; i < sizeof documents / sizeof documents[0]; i++) {
		char id[RAIJU_TITLE_ID_LEN + 1] = "";
		CHECK_INT(parse(documents[i], id), RAIJU_PARAM_BAD_TITLE_ID);
		CHECK_STR(id, "");
	}
}

static void oversized_document_is_rejected(void) {
	size_t length = RAIJU_PARAM_JSON_MAX_BYTES + 1;
	char *document = malloc(length);
	memset(document, ' ', length);
	char id[RAIJU_TITLE_ID_LEN + 1] = "";
	CHECK_INT(raiju_title_id_from_param_json(document, length, id), RAIJU_PARAM_TOO_LARGE);
	free(document);
}

static void folder_dump_title_id_is_read_from_disk(void) {
	char *dir = fixture_temp_dir();
	CHECK(dir != NULL);
	if (dir == NULL) {
		return;
	}
	char *sce_sys = fixture_path(dir, "sce_sys");
	char *param = fixture_path(dir, "sce_sys/param.json");
	char id[RAIJU_TITLE_ID_LEN + 1] = "";

	CHECK_INT(raiju_title_id_from_folder(dir, id), RAIJU_PARAM_IO_ERROR);
	CHECK_INT(fixture_mkdir(sce_sys), 0);
	CHECK_INT(fixture_write(param, realistic_param_json, strlen(realistic_param_json)), 0);
	CHECK_INT(raiju_title_id_from_folder(dir, id), RAIJU_PARAM_OK);
	CHECK_STR(id, "PPSA24003");

	char *with_slash = fixture_path(dir, "");
	char slashed_id[RAIJU_TITLE_ID_LEN + 1] = "";
	CHECK_INT(raiju_title_id_from_folder(with_slash, slashed_id), RAIJU_PARAM_OK);
	CHECK_STR(slashed_id, "PPSA24003");

	static const char broken[] = "<html>not json</html>";
	CHECK_INT(fixture_write(param, broken, sizeof broken - 1), 0);
	CHECK_INT(raiju_title_id_from_folder(dir, id), RAIJU_PARAM_MALFORMED);

	fixture_remove(param);
	fixture_remove(sce_sys);
	fixture_remove(dir);
	free(with_slash);
	free(param);
	free(sce_sys);
	free(dir);
}

static void oversized_param_json_on_disk_is_rejected(void) {
	char *dir = fixture_temp_dir();
	CHECK(dir != NULL);
	if (dir == NULL) {
		return;
	}
	char *sce_sys = fixture_path(dir, "sce_sys");
	char *param = fixture_path(dir, "sce_sys/param.json");
	size_t length = RAIJU_PARAM_JSON_MAX_BYTES + 16;
	char *document = malloc(length);
	memset(document, ' ', length);
	CHECK_INT(fixture_mkdir(sce_sys), 0);
	CHECK_INT(fixture_write(param, document, length), 0);
	char id[RAIJU_TITLE_ID_LEN + 1] = "";
	CHECK_INT(raiju_title_id_from_folder(dir, id), RAIJU_PARAM_TOO_LARGE);
	fixture_remove(param);
	fixture_remove(sce_sys);
	fixture_remove(dir);
	free(document);
	free(param);
	free(sce_sys);
	free(dir);
}

static void title_id_is_found_in_file_names(void) {
	struct {
		const char *name;
		const char *expected;
	} matches[] = {
		{"PPSA24003-some-game-name.7z", "PPSA24003"},
		{"Some Game [CUSA12345] v1.00.rar", "CUSA12345"},
		{"PPSA24003.exfat", "PPSA24003"},
		{"xxCUSA123456.zip", "CUSA12345"},
		{"PPPSA12345.zip", "PPSA12345"},
		{"PPSAPPSA00007", "PPSA00007"},
		{"CUSA1234-PPSA99999-CUSA11111", "PPSA99999"},
	};
	for (size_t i = 0; i < sizeof matches / sizeof matches[0]; i++) {
		char id[RAIJU_TITLE_ID_LEN + 1] = "";
		CHECK(raiju_title_id_from_name(matches[i].name, id));
		CHECK_STR(id, matches[i].expected);
	}
	static const char *const misses[] = {
		"ppsa24003.7z", "PPSA2400.7z", "PPSB24003.7z", "PPSA 24003", "CUSA", "PPSA1234", "", "game.exfat",
	};
	for (size_t i = 0; i < sizeof misses / sizeof misses[0]; i++) {
		char id[RAIJU_TITLE_ID_LEN + 1] = "";
		CHECK(!raiju_title_id_from_name(misses[i], id));
		CHECK_STR(id, "");
	}
	char id[RAIJU_TITLE_ID_LEN + 1] = "";
	CHECK(!raiju_title_id_from_name(NULL, id));
}

static void title_id_validation(void) {
	CHECK(raiju_title_id_valid("PPSA24003"));
	CHECK(raiju_title_id_valid("NPXS00001"));
	CHECK(!raiju_title_id_valid("PPSA2400"));
	CHECK(!raiju_title_id_valid("PPSA240031"));
	CHECK(!raiju_title_id_valid("pPSA24003"));
	CHECK(!raiju_title_id_valid("PPS124003"));
	CHECK(!raiju_title_id_valid(""));
	CHECK(!raiju_title_id_valid(NULL));
}

static void param_json_wins_and_disagreement_is_reported(void) {
	raiju_title_resolution resolution = raiju_resolve_title_id("PPSA24003", "CUSA12345-some-game.7z");
	CHECK_INT(resolution.source, RAIJU_TITLE_SOURCE_PARAM_JSON);
	CHECK_STR(resolution.title_id, "PPSA24003");
	CHECK_STR(resolution.name_title_id, "CUSA12345");
	CHECK(resolution.names_disagree);
}

static void agreeing_sources_are_not_flagged(void) {
	raiju_title_resolution resolution = raiju_resolve_title_id("PPSA24003", "PPSA24003-some-game-name.7z");
	CHECK_INT(resolution.source, RAIJU_TITLE_SOURCE_PARAM_JSON);
	CHECK_STR(resolution.title_id, "PPSA24003");
	CHECK(!resolution.names_disagree);
	raiju_title_resolution param_only = raiju_resolve_title_id("PPSA24003", "game.7z");
	CHECK_INT(param_only.source, RAIJU_TITLE_SOURCE_PARAM_JSON);
	CHECK_STR(param_only.name_title_id, "");
	CHECK(!param_only.names_disagree);
}

static void file_name_is_the_fallback(void) {
	raiju_title_resolution missing = raiju_resolve_title_id(NULL, "PPSA24003-some-game-name.7z");
	CHECK_INT(missing.source, RAIJU_TITLE_SOURCE_FILE_NAME);
	CHECK_STR(missing.title_id, "PPSA24003");
	CHECK(!missing.names_disagree);
	raiju_title_resolution invalid = raiju_resolve_title_id("../../x", "CUSA12345.rar");
	CHECK_INT(invalid.source, RAIJU_TITLE_SOURCE_FILE_NAME);
	CHECK_STR(invalid.title_id, "CUSA12345");
}

static void no_source_asks_the_user(void) {
	raiju_title_resolution resolution = raiju_resolve_title_id(NULL, "some-game.7z");
	CHECK_INT(resolution.source, RAIJU_TITLE_SOURCE_NONE);
	CHECK_STR(resolution.title_id, "");
	CHECK(!resolution.names_disagree);
	CHECK_INT(raiju_resolve_title_id(NULL, NULL).source, RAIJU_TITLE_SOURCE_NONE);
}

static void param_status_names_are_stable(void) {
	CHECK_STR(raiju_param_status_name(RAIJU_PARAM_OK), "ok");
	CHECK_STR(raiju_param_status_name(RAIJU_PARAM_MALFORMED), "malformed");
	CHECK_STR(raiju_param_status_name(RAIJU_PARAM_IO_ERROR), "io_error");
}

void run_title_id_tests(void) {
	RUN_TEST(title_id_is_read_from_realistic_param_json);
	RUN_TEST(minimal_and_spaced_documents_parse);
	RUN_TEST(byte_order_mark_is_skipped);
	RUN_TEST(escapes_in_key_and_value_are_decoded);
	RUN_TEST(nested_title_id_is_ignored);
	RUN_TEST(malformed_documents_are_rejected);
	RUN_TEST(embedded_nul_byte_is_rejected);
	RUN_TEST(deep_nesting_is_rejected);
	RUN_TEST(unusable_title_id_values_are_rejected);
	RUN_TEST(oversized_document_is_rejected);
	RUN_TEST(folder_dump_title_id_is_read_from_disk);
	RUN_TEST(oversized_param_json_on_disk_is_rejected);
	RUN_TEST(title_id_is_found_in_file_names);
	RUN_TEST(title_id_validation);
	RUN_TEST(param_json_wins_and_disagreement_is_reported);
	RUN_TEST(agreeing_sources_are_not_flagged);
	RUN_TEST(file_name_is_the_fallback);
	RUN_TEST(no_source_asks_the_user);
	RUN_TEST(param_status_names_are_stable);
}
