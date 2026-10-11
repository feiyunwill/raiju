#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <archive.h>
#include <archive_entry.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fixtures.h"
#include "raiju/extract.h"
#include "test.h"

#define PARAM_JSON "{\"titleId\":\"PPSA24003\",\"contentVersion\":\"01.000.000\"}"
#define PAUSE_FILE_BYTES (512u * 1024u)

typedef enum {
	FORMAT_ZIP = 0,
	FORMAT_7Z,
	FORMAT_TAR_ZSTD,
	FORMAT_TAR_XZ,
	FORMAT_TAR_GZ,
	FORMAT_COUNT
} fixture_format;

typedef struct {
	const char *path;
	unsigned type;
	const void *data;
	size_t length;
	const char *link;
} fixture_entry;

typedef struct {
	char *root;
	char *archive;
	char *staging;
} sandbox;

typedef struct {
	unsigned calls;
	uint64_t last_written;
	uint64_t last_total;
	bool monotonic;
	unsigned answer_after;
	raiju_extract_control answer;
} progress_log;

static const char *const format_names[FORMAT_COUNT] = {"zip", "7z", "tar.zst", "tar.xz", "tar.gz"};

/**
 * Configures an archive writer for a fixture format. Returns false when the
 * host libarchive cannot write it natively.
 */
static bool configure_writer(struct archive *writer, fixture_format format) {
	switch (format) {
	case FORMAT_ZIP:
		return archive_write_set_format_zip(writer) == ARCHIVE_OK;
	case FORMAT_7Z:
		return archive_write_set_format_7zip(writer) == ARCHIVE_OK;
	case FORMAT_TAR_ZSTD:
		return archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK &&
		       archive_write_add_filter_zstd(writer) == ARCHIVE_OK;
	case FORMAT_TAR_XZ:
		return archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK &&
		       archive_write_add_filter_xz(writer) == ARCHIVE_OK;
	case FORMAT_TAR_GZ:
		return archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK &&
		       archive_write_add_filter_gzip(writer) == ARCHIVE_OK;
	case FORMAT_COUNT:
		break;
	}
	return false;
}

/**
 * Writes a fixture archive with libarchive's write API. Returns 0 on
 * success, 1 when the format is not writable on this host and -1 on error.
 */
static int write_archive(const char *path, fixture_format format, const fixture_entry *entries, size_t count) {
	struct archive *writer = archive_write_new();
	if (writer == NULL) {
		return -1;
	}
	if (!configure_writer(writer, format)) {
		archive_write_free(writer);
		return 1;
	}
	if (archive_write_open_filename(writer, path) != ARCHIVE_OK) {
		archive_write_free(writer);
		return -1;
	}
	int status = 0;
	for (size_t i = 0; i < count && status == 0; i++) {
		struct archive_entry *entry = archive_entry_new();
		archive_entry_set_pathname(entry, entries[i].path);
		archive_entry_set_filetype(entry, entries[i].type);
		archive_entry_set_perm(entry, entries[i].type == AE_IFDIR ? 0755 : 0644);
		archive_entry_set_mtime(entry, 1700000000, 0);
		if (entries[i].type == AE_IFLNK) {
			archive_entry_set_symlink(entry, entries[i].link);
		} else if (entries[i].link != NULL) {
			archive_entry_set_hardlink(entry, entries[i].link);
		}
		if (entries[i].type == AE_IFREG) {
			archive_entry_set_size(entry, (la_int64_t)entries[i].length);
		}
		if (archive_write_header(writer, entry) != ARCHIVE_OK) {
			status = -1;
		} else if (entries[i].length > 0 &&
		           archive_write_data(writer, entries[i].data, entries[i].length) != (la_ssize_t)entries[i].length) {
			status = -1;
		}
		archive_entry_free(entry);
	}
	if (archive_write_close(writer) != ARCHIVE_OK) {
		status = -1;
	}
	archive_write_free(writer);
	return status;
}

/**
 * Creates a temporary root holding an archive path and a staging path that
 * does not exist yet.
 */
static bool sandbox_open(sandbox *box, const char *archive_name) {
	box->root = fixture_temp_dir();
	box->archive = box->root ? fixture_path(box->root, archive_name) : NULL;
	box->staging = box->root ? fixture_path(box->root, "staging") : NULL;
	return box->root != NULL && box->archive != NULL && box->staging != NULL;
}

/**
 * Removes everything a test created under the sandbox root.
 */
static void sandbox_close(sandbox *box) {
	if (box->root != NULL) {
		raiju_staging_clear(box->root);
		rmdir(box->root);
	}
	free(box->root);
	free(box->archive);
	free(box->staging);
}

/**
 * Builds the archive and extracts it into the sandbox staging directory.
 * Returns false and leaves out untouched when the format is not writable.
 */
static bool extract_fixture(sandbox *box, fixture_format format, const fixture_entry *entries, size_t count,
                            raiju_extract_result *out) {
	int written = write_archive(box->archive, format, entries, count);
	CHECK(written >= 0);
	if (written != 0) {
		if (written > 0) {
			printf("    skipped %s: not writable by this libarchive\n", format_names[format]);
		}
		return false;
	}
	raiju_extract_request request = {box->archive, box->staging, 0, NULL, NULL};
	raiju_extract_archive(&request, out);
	return true;
}

/**
 * Returns whether a path exists, without following symlinks.
 */
static bool path_exists(const char *path) {
	struct stat info;
	return lstat(path, &info) == 0;
}

/**
 * Returns the size of the regular file at dir/name, or -1.
 */
static long long file_size_in(const char *dir, const char *name) {
	char *path = fixture_path(dir, name);
	struct stat info;
	long long size = (path != NULL && lstat(path, &info) == 0 && S_ISREG(info.st_mode)) ? (long long)info.st_size : -1;
	free(path);
	return size;
}

/**
 * Returns whether text ends with suffix.
 */
static bool ends_with(const char *text, const char *suffix) {
	size_t text_len = text ? strlen(text) : 0;
	size_t suffix_len = strlen(suffix);
	return text != NULL && text_len >= suffix_len && strcmp(text + text_len - suffix_len, suffix) == 0;
}

/**
 * Records progress calls and answers with a configured control once enough
 * calls have been made.
 */
static raiju_extract_control record_progress(uint64_t written, uint64_t total, void *context) {
	progress_log *log = context;
	if (written < log->last_written) {
		log->monotonic = false;
	}
	log->calls++;
	log->last_written = written;
	log->last_total = total;
	return log->calls > log->answer_after ? log->answer : RAIJU_EXTRACT_CONTINUE;
}

static void dump_at_root_is_extracted_with_its_title_id(void) {
	static const char eboot[] = "\x7f" "ELF fake eboot";
	const fixture_entry entries[] = {
	    {"sce_sys/", AE_IFDIR, NULL, 0, NULL},
	    {"sce_sys/param.json", AE_IFREG, PARAM_JSON, sizeof PARAM_JSON - 1, NULL},
	    {"eboot.bin", AE_IFREG, eboot, sizeof eboot - 1, NULL},
	};
	sandbox box;
	CHECK(sandbox_open(&box, "CUSA00001-game.zip"));
	raiju_extract_result result;
	if (extract_fixture(&box, FORMAT_ZIP, entries, 3, &result)) {
		CHECK_INT(result.status, RAIJU_EXTRACT_OK);
		CHECK_INT(result.layout, RAIJU_LAYOUT_FOLDER_DUMP);
		CHECK_INT(result.wrapper_depth, 0);
		CHECK_INT(result.entries, 3);
		CHECK_INT(result.bytes_written, sizeof PARAM_JSON - 1 + sizeof eboot - 1);
		char *canonical = realpath(box.staging, NULL);
		CHECK(canonical != NULL && result.payload_path != NULL && strcmp(canonical, result.payload_path) == 0);
		free(canonical);
		CHECK_INT(file_size_in(box.staging, "eboot.bin"), sizeof eboot - 1);
		CHECK_INT(result.param_status, RAIJU_PARAM_OK);
		CHECK_STR(result.title_id, "PPSA24003");
		CHECK_INT(result.param_json_len, sizeof PARAM_JSON - 1);
		CHECK(result.param_json != NULL && memcmp(result.param_json, PARAM_JSON, sizeof PARAM_JSON - 1) == 0);
		raiju_title_resolution resolution = raiju_resolve_title_id(result.title_id, "CUSA00001-game.zip");
		CHECK_INT(resolution.source, RAIJU_TITLE_SOURCE_PARAM_JSON);
		CHECK_STR(resolution.title_id, "PPSA24003");
		CHECK(resolution.names_disagree);
	}
	raiju_extract_result_free(&result);
	sandbox_close(&box);
}

static void wrapper_folders_are_stripped_in_every_format(void) {
	static const char eboot[] = "eboot";
	const fixture_entry entries[] = {
	    {"Game/", AE_IFDIR, NULL, 0, NULL},
	    {"Game/Game/", AE_IFDIR, NULL, 0, NULL},
	    {"Game/Game/sce_sys/param.json", AE_IFREG, PARAM_JSON, sizeof PARAM_JSON - 1, NULL},
	    {"Game/Game/eboot.bin", AE_IFREG, eboot, sizeof eboot - 1, NULL},
	};
	for (int format = 0; format < FORMAT_COUNT; format++) {
		sandbox box;
		CHECK(sandbox_open(&box, "dump.archive"));
		raiju_extract_result result;
		if (extract_fixture(&box, (fixture_format)format, entries, 4, &result)) {
			if (result.status != RAIJU_EXTRACT_OK) {
				fprintf(stderr, "    format %s: %s\n", format_names[format], raiju_extract_status_name(result.status));
			}
			CHECK_INT(result.status, RAIJU_EXTRACT_OK);
			CHECK_INT(result.layout, RAIJU_LAYOUT_FOLDER_DUMP);
			CHECK_INT(result.wrapper_depth, 2);
			CHECK(ends_with(result.payload_path, "/staging/Game/Game"));
			CHECK_STR(result.title_id, "PPSA24003");
			CHECK_INT(result.bytes_written, sizeof PARAM_JSON - 1 + sizeof eboot - 1);
		}
		raiju_extract_result_free(&result);
		sandbox_close(&box);
	}
}

static void implicit_directories_and_clutter_do_not_block_stripping(void) {
	const fixture_entry entries[] = {
	    {"./Wrapper/./Dump/sce_sys/param.json", AE_IFREG, PARAM_JSON, sizeof PARAM_JSON - 1, NULL},
	    {"__MACOSX/Wrapper/._Dump", AE_IFREG, "x", 1, NULL},
	    {"Wrapper/.DS_Store", AE_IFREG, "x", 1, NULL},
	};
	sandbox box;
	CHECK(sandbox_open(&box, "dump.zip"));
	raiju_extract_result result;
	if (extract_fixture(&box, FORMAT_ZIP, entries, 3, &result)) {
		CHECK_INT(result.status, RAIJU_EXTRACT_OK);
		CHECK_INT(result.wrapper_depth, 2);
		CHECK(ends_with(result.payload_path, "/Wrapper/Dump"));
	}
	raiju_extract_result_free(&result);
	sandbox_close(&box);
}

static void dump_without_param_json_reports_it_missing(void) {
	const fixture_entry entries[] = {
	    {"Game/sce_sys/icon0.png", AE_IFREG, "png", 3, NULL},
	};
	sandbox box;
	CHECK(sandbox_open(&box, "dump.tar.gz"));
	raiju_extract_result result;
	if (extract_fixture(&box, FORMAT_TAR_GZ, entries, 1, &result)) {
		CHECK_INT(result.status, RAIJU_EXTRACT_OK);
		CHECK_INT(result.layout, RAIJU_LAYOUT_FOLDER_DUMP);
		CHECK_INT(result.param_status, RAIJU_PARAM_IO_ERROR);
		CHECK(result.param_json == NULL);
		CHECK_STR(result.title_id, "");
	}
	raiju_extract_result_free(&result);
	sandbox_close(&box);
}

static void single_image_becomes_the_install_payload(void) {
	size_t length = 256u * 512u;
	uint8_t *image = fixture_exfat(length, 9, 7, 256);
	const fixture_entry entries[] = {
	    {"Release/PPSA24003.exfat", AE_IFREG, image, length, NULL},
	    {"Release/readme.nfo", AE_IFREG, "enjoy", 5, NULL},
	};
	for (int format = 0; format < FORMAT_COUNT; format++) {
		sandbox box;
		CHECK(sandbox_open(&box, "image.archive"));
		raiju_extract_result result;
		if (extract_fixture(&box, (fixture_format)format, entries, 2, &result)) {
			CHECK_INT(result.status, RAIJU_EXTRACT_OK);
			CHECK_INT(result.layout, RAIJU_LAYOUT_SINGLE_IMAGE);
			CHECK(ends_with(result.payload_path, "/staging/Release/PPSA24003.exfat"));
			CHECK_INT(result.image.classification.kind, RAIJU_KIND_EXFAT);
			CHECK(raiju_inspection_accepted(&result.image));
			CHECK(result.param_json == NULL);
		}
		raiju_extract_result_free(&result);
		sandbox_close(&box);
	}
	free(image);
}

static void corrupted_single_image_is_refused(void) {
	static const char page[] = "<html>quota exceeded</html>";
	const fixture_entry entries[] = {
	    {"PPSA24003.ffpkg", AE_IFREG, page, sizeof page - 1, NULL},
	};
	sandbox box;
	CHECK(sandbox_open(&box, "image.zip"));
	raiju_extract_result result;
	if (extract_fixture(&box, FORMAT_ZIP, entries, 1, &result)) {
		CHECK_INT(result.status, RAIJU_EXTRACT_BAD_IMAGE);
		CHECK(result.image.classification.header_mismatch);
	}
	raiju_extract_result_free(&result);
	sandbox_close(&box);
}

static void several_images_or_none_have_no_payload(void) {
	const fixture_entry two_images[] = {
	    {"a/PPSA24003.ffpfs", AE_IFREG, "a", 1, NULL},
	    {"b/PPSA24004.ffpfsc", AE_IFREG, "b", 1, NULL},
	};
	const fixture_entry nothing[] = {
	    {"notes.txt", AE_IFREG, "hello", 5, NULL},
	};
	sandbox box;
	CHECK(sandbox_open(&box, "two.zip"));
	raiju_extract_result result;
	if (extract_fixture(&box, FORMAT_ZIP, two_images, 2, &result)) {
		CHECK_INT(result.status, RAIJU_EXTRACT_MULTIPLE_IMAGES);
		CHECK_INT(result.layout, RAIJU_LAYOUT_NONE);
	}
	raiju_extract_result_free(&result);
	sandbox_close(&box);

	CHECK(sandbox_open(&box, "none.zip"));
	if (extract_fixture(&box, FORMAT_ZIP, nothing, 1, &result)) {
		CHECK_INT(result.status, RAIJU_EXTRACT_NO_PAYLOAD);
	}
	raiju_extract_result_free(&result);
	sandbox_close(&box);
}

/**
 * Extracts a malicious archive and checks the job failed with the expected
 * status on the expected entry, after the legitimate entry before it.
 */
static void expect_rejected(fixture_format format, const fixture_entry *bad, raiju_extract_status expected,
                            const char *escape_name) {
	const fixture_entry entries[] = {
	    {"Game/sce_sys/param.json", AE_IFREG, PARAM_JSON, sizeof PARAM_JSON - 1, NULL},
	    *bad,
	};
	sandbox box;
	CHECK(sandbox_open(&box, "evil.archive"));
	raiju_extract_result result;
	if (extract_fixture(&box, format, entries, 2, &result)) {
		if (result.status != expected) {
			fprintf(stderr, "    %s entry %s: %s\n", format_names[format], bad->path,
			        raiju_extract_status_name(result.status));
		}
		CHECK_INT(result.status, expected);
		CHECK_INT(result.layout, RAIJU_LAYOUT_NONE);
		CHECK(result.payload_path == NULL);
		CHECK(result.failed_entry != NULL && strcmp(result.failed_entry, bad->path) == 0);
		CHECK_INT(result.entries, 1);
		if (escape_name != NULL) {
			char *escaped = fixture_path(box.root, escape_name);
			CHECK(escaped != NULL && !path_exists(escaped));
			free(escaped);
		}
	}
	raiju_extract_result_free(&result);
	sandbox_close(&box);
}

static void parent_traversal_is_rejected(void) {
	const fixture_entry escape = {"../escape.txt", AE_IFREG, "pwned", 5, NULL};
	const fixture_entry nested = {"Game/../../escape.txt", AE_IFREG, "pwned", 5, NULL};
	const fixture_entry backslash = {"..\\escape.txt", AE_IFREG, "pwned", 5, NULL};
	expect_rejected(FORMAT_TAR_GZ, &escape, RAIJU_EXTRACT_UNSAFE_PATH, "escape.txt");
	expect_rejected(FORMAT_ZIP, &escape, RAIJU_EXTRACT_UNSAFE_PATH, "escape.txt");
	expect_rejected(FORMAT_TAR_XZ, &nested, RAIJU_EXTRACT_UNSAFE_PATH, "escape.txt");
	expect_rejected(FORMAT_TAR_GZ, &backslash, RAIJU_EXTRACT_UNSAFE_PATH, NULL);
}

static void absolute_path_is_rejected(void) {
	char *root = fixture_temp_dir();
	char *target = root ? fixture_path(root, "absolute.txt") : NULL;
	CHECK(target != NULL);
	if (target != NULL) {
		const fixture_entry absolute = {target, AE_IFREG, "pwned", 5, NULL};
		expect_rejected(FORMAT_TAR_GZ, &absolute, RAIJU_EXTRACT_UNSAFE_PATH, NULL);
		expect_rejected(FORMAT_ZIP, &absolute, RAIJU_EXTRACT_UNSAFE_PATH, NULL);
		CHECK(!path_exists(target));
	}
	free(target);
	if (root != NULL) {
		rmdir(root);
	}
	free(root);
}

static void symlinks_are_rejected(void) {
	const fixture_entry outside = {"Game/link", AE_IFLNK, NULL, 0, "/etc"};
	const fixture_entry inside = {"Game/link", AE_IFLNK, NULL, 0, "sce_sys/param.json"};
	expect_rejected(FORMAT_TAR_GZ, &outside, RAIJU_EXTRACT_LINK_ENTRY, NULL);
	expect_rejected(FORMAT_TAR_ZSTD, &inside, RAIJU_EXTRACT_LINK_ENTRY, NULL);
	expect_rejected(FORMAT_ZIP, &outside, RAIJU_EXTRACT_LINK_ENTRY, NULL);
}

static void hardlinks_are_rejected(void) {
	const fixture_entry inside = {"Game/copy.json", AE_IFREG, NULL, 0, "Game/sce_sys/param.json"};
	const fixture_entry outside = {"Game/passwd", AE_IFREG, NULL, 0, "/etc/passwd"};
	expect_rejected(FORMAT_TAR_GZ, &inside, RAIJU_EXTRACT_LINK_ENTRY, NULL);
	expect_rejected(FORMAT_TAR_XZ, &outside, RAIJU_EXTRACT_LINK_ENTRY, NULL);
}

static void special_files_are_rejected(void) {
	const fixture_entry fifo = {"Game/pipe", AE_IFIFO, NULL, 0, NULL};
	expect_rejected(FORMAT_TAR_GZ, &fifo, RAIJU_EXTRACT_UNSUPPORTED_ENTRY, NULL);
}

static void progress_reports_bytes_against_the_total(void) {
	uint8_t *blob = calloc(1, PAUSE_FILE_BYTES);
	const fixture_entry entries[] = {
	    {"Game/sce_sys/param.json", AE_IFREG, PARAM_JSON, sizeof PARAM_JSON - 1, NULL},
	    {"Game/data.bin", AE_IFREG, blob, PAUSE_FILE_BYTES, NULL},
	};
	uint64_t expected = sizeof PARAM_JSON - 1 + PAUSE_FILE_BYTES;
	sandbox box;
	CHECK(sandbox_open(&box, "dump.zip"));
	CHECK_INT(write_archive(box.archive, FORMAT_ZIP, entries, 2), 0);
	uint64_t total = 0;
	CHECK_INT(raiju_archive_total_bytes(box.archive, &total), 0);
	CHECK_INT(total, expected);
	progress_log log = {0, 0, 0, true, 1000000u, RAIJU_EXTRACT_CONTINUE};
	raiju_extract_request request = {box.archive, box.staging, total, record_progress, &log};
	raiju_extract_result result;
	CHECK_INT(raiju_extract_archive(&request, &result), RAIJU_EXTRACT_OK);
	CHECK(log.calls >= 3);
	CHECK(log.monotonic);
	CHECK_INT(log.last_written, expected);
	CHECK_INT(log.last_total, expected);
	CHECK_INT(result.bytes_written, expected);
	raiju_extract_result_free(&result);
	sandbox_close(&box);
	free(blob);
}

static void pause_waits_for_the_current_entry_then_resume_restarts(void) {
	uint8_t *blob = calloc(1, PAUSE_FILE_BYTES);
	const fixture_entry entries[] = {
	    {"Game/a.bin", AE_IFREG, blob, PAUSE_FILE_BYTES, NULL},
	    {"Game/b.bin", AE_IFREG, blob, PAUSE_FILE_BYTES, NULL},
	    {"Game/sce_sys/param.json", AE_IFREG, PARAM_JSON, sizeof PARAM_JSON - 1, NULL},
	};
	sandbox box;
	CHECK(sandbox_open(&box, "dump.tar.zst"));
	fixture_format format = write_archive(box.archive, FORMAT_TAR_ZSTD, entries, 3) == 0 ? FORMAT_TAR_ZSTD : FORMAT_ZIP;
	CHECK_INT(write_archive(box.archive, format, entries, 3), 0);

	progress_log log = {0, 0, 0, true, 2, RAIJU_EXTRACT_PAUSE};
	raiju_extract_request request = {box.archive, box.staging, 0, record_progress, &log};
	raiju_extract_result result;
	CHECK_INT(raiju_extract_archive(&request, &result), RAIJU_EXTRACT_PAUSED);
	CHECK_INT(result.entries, 1);
	CHECK_INT(result.layout, RAIJU_LAYOUT_NONE);
	CHECK_INT(log.last_total, 0);
	char *game = fixture_path(box.staging, "Game");
	CHECK_INT(file_size_in(game, "a.bin"), PAUSE_FILE_BYTES);
	CHECK_INT(file_size_in(game, "b.bin"), -1);
	raiju_extract_result_free(&result);

	log = (progress_log){0, 0, 0, true, 1000000u, RAIJU_EXTRACT_CONTINUE};
	CHECK_INT(raiju_extract_archive(&request, &result), RAIJU_EXTRACT_STAGING_NOT_EMPTY);
	CHECK_INT(raiju_staging_clear(box.staging), 0);
	CHECK_INT(raiju_extract_archive(&request, &result), RAIJU_EXTRACT_OK);
	CHECK_INT(result.entries, 3);
	CHECK_INT(file_size_in(game, "b.bin"), PAUSE_FILE_BYTES);
	CHECK_STR(result.title_id, "PPSA24003");
	raiju_extract_result_free(&result);
	free(game);
	sandbox_close(&box);
	free(blob);
}

static void cancel_stops_before_the_first_entry(void) {
	const fixture_entry entries[] = {
	    {"Game/sce_sys/param.json", AE_IFREG, PARAM_JSON, sizeof PARAM_JSON - 1, NULL},
	};
	sandbox box;
	CHECK(sandbox_open(&box, "dump.zip"));
	CHECK_INT(write_archive(box.archive, FORMAT_ZIP, entries, 1), 0);
	progress_log log = {0, 0, 0, true, 0, RAIJU_EXTRACT_CANCEL};
	raiju_extract_request request = {box.archive, box.staging, 0, record_progress, &log};
	raiju_extract_result result;
	CHECK_INT(raiju_extract_archive(&request, &result), RAIJU_EXTRACT_CANCELLED);
	CHECK_INT(result.entries, 0);
	CHECK_INT(log.calls, 1);
	raiju_extract_result_free(&result);
	sandbox_close(&box);
}

static void unreadable_archives_and_bad_arguments_fail(void) {
	sandbox box;
	CHECK(sandbox_open(&box, "garbage.zip"));
	CHECK_INT(fixture_write(box.archive, "not an archive at all", 21), 0);
	raiju_extract_request request = {box.archive, box.staging, 0, NULL, NULL};
	raiju_extract_result result;
	CHECK_INT(raiju_extract_archive(&request, &result), RAIJU_EXTRACT_ARCHIVE_ERROR);
	raiju_extract_result_free(&result);
	uint64_t total = 1;
	CHECK_INT(raiju_archive_total_bytes(box.archive, &total), -1);
	CHECK_INT(total, 0);
	char *missing = fixture_path(box.root, "missing.7z");
	request.archive_path = missing;
	CHECK_INT(raiju_extract_archive(&request, &result), RAIJU_EXTRACT_ARCHIVE_ERROR);
	raiju_extract_result_free(&result);
	free(missing);
	sandbox_close(&box);
	CHECK_INT(raiju_extract_archive(NULL, &result), RAIJU_EXTRACT_INVALID_ARGUMENT);
	CHECK_INT(raiju_extract_archive(&request, NULL), RAIJU_EXTRACT_INVALID_ARGUMENT);
	CHECK_INT(raiju_staging_clear(NULL), -1);
}

static void names_are_stable(void) {
	CHECK_STR(raiju_extract_status_name(RAIJU_EXTRACT_LINK_ENTRY), "link_entry");
	CHECK_STR(raiju_extract_status_name(RAIJU_EXTRACT_UNSAFE_PATH), "unsafe_path");
	CHECK_STR(raiju_layout_name(RAIJU_LAYOUT_FOLDER_DUMP), "folder_dump");
	CHECK_STR(raiju_layout_name(RAIJU_LAYOUT_SINGLE_IMAGE), "single_image");
}

void run_extract_tests(void) {
	RUN_TEST(dump_at_root_is_extracted_with_its_title_id);
	RUN_TEST(wrapper_folders_are_stripped_in_every_format);
	RUN_TEST(implicit_directories_and_clutter_do_not_block_stripping);
	RUN_TEST(dump_without_param_json_reports_it_missing);
	RUN_TEST(single_image_becomes_the_install_payload);
	RUN_TEST(corrupted_single_image_is_refused);
	RUN_TEST(several_images_or_none_have_no_payload);
	RUN_TEST(parent_traversal_is_rejected);
	RUN_TEST(absolute_path_is_rejected);
	RUN_TEST(symlinks_are_rejected);
	RUN_TEST(hardlinks_are_rejected);
	RUN_TEST(special_files_are_rejected);
	RUN_TEST(progress_reports_bytes_against_the_total);
	RUN_TEST(pause_waits_for_the_current_entry_then_resume_restarts);
	RUN_TEST(cancel_stops_before_the_first_entry);
	RUN_TEST(unreadable_archives_and_bad_arguments_fail);
	RUN_TEST(names_are_stable);
}
