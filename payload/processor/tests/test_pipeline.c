#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <archive_entry.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "archive_builder.h"
#include "fixtures.h"
#include "image_builder.h"
#include "raiju/processor.h"
#include "test.h"

#define PARAM_JSON "{\"titleId\":\"PPSA24003\",\"localizedParameters\":{\"en-US\":{\"titleName\":\"Raiju\"}}}"
#define EBOOT "\177ELF eboot"
#define OTHER_FILE "\177ELF homebrew"
#define OLD_CONTENT "old copy"
#define JOB_ID "job-1"
#define MAX_STEPS 32
#define STAGE_COUNT (RAIJU_STAGE_CANCELLED + 1)

typedef struct {
	char *root;
	char *downloads;
	char *staging;
	char *trash;
	char *install;
	char *jobs;
	unsigned rescans;
	unsigned notifies;
	unsigned rescan_failures;
	unsigned notify_failures;
	bool rescan_saw_install;
	uint64_t free_bytes;
	bool free_fails;
	raiju_extract_control progress_answer;
	raiju_pipeline pipeline;
} world;

typedef struct {
	const char *name;
	const char *download_name;
	int (*make)(const char *path);
	const char *install_name;
	bool (*verify)(const char *path);
	bool deletes_download;
	bool extracts;
} scenario;

typedef struct {
	const char *name;
	bool (*verify)(const char *path);
} allowed_entry;

/**
 * Returns whether a path exists, without following symlinks.
 */
static bool exists(const char *path) {
	struct stat info;
	return lstat(path, &info) == 0;
}

/**
 * Returns whether the file at path holds exactly length bytes equal to data.
 */
static bool file_equals(const char *path, const void *data, size_t length) {
	FILE *stream = fopen(path, "rb");
	if (stream == NULL) {
		return false;
	}
	unsigned char *buffer = malloc(length + 1);
	size_t got = buffer ? fread(buffer, 1, length + 1, stream) : 0;
	fclose(stream);
	bool equal = buffer != NULL && got == length && memcmp(buffer, data, length) == 0;
	free(buffer);
	return equal;
}

/**
 * Returns whether dir/name holds exactly the given text.
 */
static bool file_in_equals(const char *dir, const char *name, const char *text) {
	char *path = fixture_path(dir, name);
	bool equal = path != NULL && file_equals(path, text, strlen(text));
	free(path);
	return equal;
}

/**
 * Returns whether dir/name exists.
 */
static bool exists_in(const char *dir, const char *name) {
	char *path = fixture_path(dir, name);
	bool found = path != NULL && exists(path);
	free(path);
	return found;
}

/**
 * Adds up the sizes of the regular files under path.
 */
static uint64_t tree_bytes(const char *path) {
	struct stat info;
	if (lstat(path, &info) != 0) {
		return 0;
	}
	if (S_ISREG(info.st_mode)) {
		return (uint64_t)info.st_size;
	}
	uint64_t total = 0;
	DIR *dir = S_ISDIR(info.st_mode) ? opendir(path) : NULL;
	struct dirent *item;
	while (dir != NULL && (item = readdir(dir)) != NULL) {
		if (strcmp(item->d_name, ".") != 0 && strcmp(item->d_name, "..") != 0) {
			char *child = fixture_path(path, item->d_name);
			total += tree_bytes(child);
			free(child);
		}
	}
	if (dir != NULL) {
		closedir(dir);
	}
	return total;
}

/**
 * Builds the exFAT game image every image scenario uses.
 */
static builder_exfat *exfat_game(void) {
	return builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
}

/**
 * Writes the exFAT game image to path.
 */
static int make_exfat(const char *path) {
	builder_exfat *volume = exfat_game();
	int status = volume ? fixture_write(path, volume->data, volume->size) : -1;
	builder_exfat_free(volume);
	return status;
}

/**
 * Returns whether path holds the exFAT game image byte for byte.
 */
static bool verify_exfat(const char *path) {
	builder_exfat *volume = exfat_game();
	bool equal = volume != NULL && file_equals(path, volume->data, volume->size);
	builder_exfat_free(volume);
	return equal;
}

/**
 * Writes a UFS2 game image without a title ID in its name to path.
 */
static int make_ufs2(const char *path) {
	builder_ufs2 *fs = builder_ufs2_game(65536, 65536, PARAM_JSON);
	int status = fs ? fixture_write(path, fs->data, fs->size) : -1;
	builder_ufs2_free(fs);
	return status;
}

/**
 * Returns whether path holds the UFS2 game image byte for byte.
 */
static bool verify_ufs2(const char *path) {
	builder_ufs2 *fs = builder_ufs2_game(65536, 65536, PARAM_JSON);
	bool equal = fs != NULL && file_equals(path, fs->data, fs->size);
	builder_ufs2_free(fs);
	return equal;
}

/**
 * Writes a zip holding a folder dump wrapped in two folders.
 */
static int make_dump_zip(const char *path) {
	const fixture_entry entries[] = {
	    {"Game/Game/sce_sys/param.json", AE_IFREG, PARAM_JSON, sizeof PARAM_JSON - 1, NULL},
	    {"Game/Game/eboot.bin", AE_IFREG, EBOOT, sizeof EBOOT - 1, NULL},
	    {"__MACOSX/Game/._eboot.bin", AE_IFREG, "x", 1, NULL},
	};
	return archive_builder_write(path, FORMAT_ZIP, entries, 3);
}

/**
 * Returns whether path is a complete folder dump with sce_sys/ at its root.
 */
static bool verify_dump(const char *path) {
	return file_in_equals(path, "eboot.bin", EBOOT) && file_in_equals(path, "sce_sys/param.json", PARAM_JSON) &&
	       !exists_in(path, "Game");
}

/**
 * Writes a zip holding a single exFAT image with an uppercase extension.
 */
static int make_image_zip(const char *path) {
	builder_exfat *volume = exfat_game();
	if (volume == NULL) {
		return -1;
	}
	const fixture_entry entries[] = {
	    {"Wrapper/Game.EXFAT", AE_IFREG, volume->data, volume->size, NULL},
	};
	int status = archive_builder_write(path, FORMAT_ZIP, entries, 1);
	builder_exfat_free(volume);
	return status;
}

/**
 * Writes a file that is neither an archive nor an image.
 */
static int make_other(const char *path) {
	return fixture_write(path, OTHER_FILE, sizeof OTHER_FILE - 1);
}

/**
 * Returns whether path holds the other file's bytes.
 */
static bool verify_other(const char *path) {
	return file_equals(path, OTHER_FILE, sizeof OTHER_FILE - 1);
}

/**
 * Creates a downloaded folder (as a multi-file download would) holding a
 * dump under one wrapper folder.
 */
static int make_dump_folder(const char *path) {
	char *wrap = fixture_path(path, "Wrap");
	char *sce_sys = wrap ? fixture_path(wrap, "sce_sys") : NULL;
	char *param = sce_sys ? fixture_path(sce_sys, "param.json") : NULL;
	char *eboot = wrap ? fixture_path(wrap, "eboot.bin") : NULL;
	int status = (param && eboot && fixture_mkdir(path) == 0 && fixture_mkdir(wrap) == 0 &&
	              fixture_mkdir(sce_sys) == 0 && fixture_write(param, PARAM_JSON, sizeof PARAM_JSON - 1) == 0 &&
	              fixture_write(eboot, EBOOT, sizeof EBOOT - 1) == 0)
	                 ? 0
	                 : -1;
	free(wrap);
	free(sce_sys);
	free(param);
	free(eboot);
	return status;
}

static const scenario dump_zip = {
    "dump zip", "CUSA00001-game.zip", make_dump_zip, "PPSA24003", verify_dump, true, true};
static const scenario image_zip = {
    "image zip", "PPSA99999-pack.zip", make_image_zip, "PPSA24003.exfat", verify_exfat, true, true};
static const scenario exfat_image = {
    "exfat image", "some-game.exfat", make_exfat, "PPSA24003.exfat", verify_exfat, false, false};
static const scenario ufs2_image = {
    "ufs2 image", "Some Game.FFPKG", make_ufs2, "PPSA24003.ffpkg", verify_ufs2, false, false};
static const scenario other_file = {
    "other file", "homebrew.elf", make_other, "homebrew.elf", verify_other, false, false};
static const scenario dump_folder = {
    "dump folder", "dump folder", make_dump_folder, "PPSA24003", verify_dump, true, false};

static const scenario *const all_scenarios[] = {&dump_zip, &image_zip, &exfat_image, &ufs2_image, &other_file,
                                                &dump_folder};

/**
 * Fake ShadowMountPlus rescan: confirms the job's install path exists, and
 * fails while rescan_failures is positive.
 */
static int fake_rescan(const raiju_job *job, char *error, size_t error_size, void *context) {
	world *w = context;
	w->rescans++;
	w->rescan_saw_install = exists(job->install_path);
	if (w->rescan_failures > 0) {
		w->rescan_failures--;
		snprintf(error, error_size, "shadowmount down");
		return -1;
	}
	return 0;
}

/**
 * Fake PS5 notification, failing while notify_failures is positive.
 */
static int fake_notify(const raiju_job *job, char *error, size_t error_size, void *context) {
	(void)job;
	world *w = context;
	w->notifies++;
	if (w->notify_failures > 0) {
		w->notify_failures--;
		snprintf(error, error_size, "notification refused");
		return -1;
	}
	return 0;
}

/**
 * Fake progress hook answering with the world's configured control.
 */
static raiju_extract_control fake_progress(const raiju_job *job, uint64_t done, uint64_t total, void *context) {
	(void)job;
	(void)done;
	(void)total;
	return ((world *)context)->progress_answer;
}

/**
 * Fake statfs reporting the world's configured free space.
 */
static int fake_free_space(const char *path, uint64_t *free_bytes, void *context) {
	(void)path;
	world *w = context;
	*free_bytes = w->free_bytes;
	return w->free_fails ? -1 : 0;
}

/**
 * Creates a temporary world: downloads and install folders, paths for
 * staging, trash and jobs.json, and a pipeline wired to the fakes.
 */
static bool world_open(world *w) {
	memset(w, 0, sizeof *w);
	w->root = fixture_temp_dir();
	if (w->root == NULL) {
		return false;
	}
	w->downloads = fixture_path(w->root, "downloads");
	w->staging = fixture_path(w->root, "staging");
	w->trash = fixture_path(w->root, "trash");
	w->install = fixture_path(w->root, "homebrew");
	w->jobs = fixture_path(w->root, "jobs.json");
	w->free_bytes = UINT64_MAX;
	w->progress_answer = RAIJU_EXTRACT_CONTINUE;
	w->pipeline = (raiju_pipeline){w->staging, w->trash, fake_rescan, fake_notify, fake_progress, fake_free_space, w};
	return w->downloads && w->staging && w->trash && w->install && w->jobs && fixture_mkdir(w->downloads) == 0 &&
	       fixture_mkdir(w->install) == 0;
}

/**
 * Deletes everything the world created.
 */
static void world_close(world *w) {
	if (w->root != NULL) {
		raiju_staging_clear(w->root);
		rmdir(w->root);
	}
	free(w->root);
	free(w->downloads);
	free(w->staging);
	free(w->trash);
	free(w->install);
	free(w->jobs);
}

/**
 * Returns the download path of a scenario in the world. The caller frees it.
 */
static char *download_path(const world *w, const scenario *s) {
	return fixture_path(w->downloads, s->download_name);
}

/**
 * Builds the scenario's download, adds a job for it, and reports the
 * download complete. Returns the job, or NULL.
 */
static raiju_job *start_job(world *w, raiju_processor *processor, const scenario *s, bool keep_archive,
                            raiju_conflict_choice policy) {
	char *path = download_path(w, s);
	CHECK(path != NULL && s->make(path) == 0);
	CHECK_INT(raiju_processor_open(processor, &w->pipeline, w->jobs), RAIJU_PROCESSOR_OK);
	raiju_job_options options = {JOB_ID, NULL, w->install, 0, 0, keep_archive, policy, NULL};
	CHECK_INT(raiju_processor_add(processor, &options, RAIJU_CATEGORY_OTHER, NULL), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_DOWNLOADING);
	CHECK_INT(raiju_processor_download_complete(processor, JOB_ID, path, tree_bytes(path)), RAIJU_PROCESSOR_OK);
	free(path);
	return raiju_processor_find(processor, JOB_ID);
}

/**
 * Checks that every entry under the install root is one of the allowed
 * names and passes its verifier, so a partial copy can never be seen.
 */
static void check_root(const world *w, const allowed_entry *allowed, size_t count) {
	DIR *dir = opendir(w->install);
	CHECK(dir != NULL);
	struct dirent *item;
	while (dir != NULL && (item = readdir(dir)) != NULL) {
		if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
			continue;
		}
		char *path = fixture_path(w->install, item->d_name);
		bool valid = false;
		for (size_t i = 0; i < count; i++) {
			if (strcmp(allowed[i].name, item->d_name) == 0) {
				valid = allowed[i].verify(path);
			}
		}
		if (!valid) {
			fprintf(stderr, "    unexpected install root entry: %s\n", item->d_name);
		}
		CHECK(valid);
		free(path);
	}
	if (dir != NULL) {
		closedir(dir);
	}
}

/**
 * Checks the install root against a scenario's single expected entry.
 */
static void check_scenario_root(const world *w, const scenario *s) {
	allowed_entry allowed = {s->install_name, s->verify};
	check_root(w, &allowed, 1);
}

/**
 * Returns whether the job's staging and trash folders are gone.
 */
static bool job_dirs_gone(const world *w) {
	return !exists_in(w->staging, JOB_ID) && !exists_in(w->trash, JOB_ID);
}

/**
 * Checks everything a READY job must leave behind.
 */
static void check_ready(const world *w, raiju_processor *processor, const scenario *s, bool keep_archive) {
	raiju_job *job = raiju_processor_find(processor, JOB_ID);
	CHECK(job != NULL);
	if (job == NULL) {
		return;
	}
	if (job->stage != RAIJU_STAGE_READY) {
		fprintf(stderr, "    %s: stage %s (%s)\n", s->name, raiju_stage_name(job->stage), job->error);
	}
	CHECK_INT(job->stage, RAIJU_STAGE_READY);
	char *installed = fixture_path(w->install, s->install_name);
	CHECK(s->verify(installed));
	CHECK_STR(job->install_path, installed);
	free(installed);
	check_scenario_root(w, s);
	CHECK(job_dirs_gone(w));
	char *download = download_path(w, s);
	bool download_remains = s->deletes_download ? keep_archive : false;
	CHECK_INT(exists(download), download_remains);
	free(download);
	CHECK(w->rescans >= 1 && w->rescan_saw_install);
	CHECK(w->notifies >= 1);
}

/**
 * Drives one scenario to READY with persistence after every stage, checking
 * the install root after each one.
 */
static void run_scenario(const scenario *s, bool keep_archive, const raiju_stage *expected_path, size_t path_len) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	raiju_job *job = start_job(&w, &processor, s, keep_archive, RAIJU_CONFLICT_ASK);
	size_t visited = 0;
	CHECK(job != NULL);
	while (job != NULL && raiju_job_runnable(job) && visited < MAX_STEPS) {
		if (expected_path != NULL && visited < path_len) {
			CHECK_INT(job->stage, expected_path[visited]);
		}
		CHECK_INT(raiju_processor_step(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
		check_scenario_root(&w, s);
		job = raiju_processor_find(&processor, JOB_ID);
		visited++;
	}
	if (expected_path != NULL) {
		CHECK_INT(visited, path_len);
	}
	check_ready(&w, &processor, s, keep_archive);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void archive_goes_through_every_stage(void) {
	const raiju_stage path[] = {RAIJU_STAGE_VERIFYING,  RAIJU_STAGE_CLASSIFYING, RAIJU_STAGE_EXTRACTING,
	                            RAIJU_STAGE_INSTALLING, RAIJU_STAGE_INSTALLING,  RAIJU_STAGE_RESCAN,
	                            RAIJU_STAGE_NOTIFYING};
	run_scenario(&dump_zip, false, path, sizeof path / sizeof path[0]);
	run_scenario(&image_zip, false, path, sizeof path / sizeof path[0]);
}

static void images_and_other_files_skip_extraction(void) {
	const raiju_stage path[] = {RAIJU_STAGE_VERIFYING,  RAIJU_STAGE_CLASSIFYING, RAIJU_STAGE_INSTALLING,
	                            RAIJU_STAGE_INSTALLING, RAIJU_STAGE_RESCAN,      RAIJU_STAGE_NOTIFYING};
	run_scenario(&exfat_image, false, path, sizeof path / sizeof path[0]);
	run_scenario(&ufs2_image, false, path, sizeof path / sizeof path[0]);
	run_scenario(&other_file, false, path, sizeof path / sizeof path[0]);
	run_scenario(&dump_folder, false, path, sizeof path / sizeof path[0]);
}

static void title_ids_come_from_param_json_first(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	start_job(&w, &processor, &image_zip, false, RAIJU_CONFLICT_ASK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_READY);
	if (job != NULL) {
		CHECK_STR(job->title_id, "PPSA24003");
		CHECK_STR(job->display_name, "Raiju");
		CHECK_INT(job->title_source, RAIJU_TITLE_SOURCE_PARAM_JSON);
		CHECK(job->names_disagree);
		CHECK_STR(raiju_kind_name(job->kind), "zip");
		CHECK_INT(job->payload_type, RAIJU_PAYLOAD_IMAGE);
	}
	raiju_processor_close(&processor);
	world_close(&w);
}

static void keep_archive_leaves_the_download(void) {
	run_scenario(&dump_zip, true, NULL, 0);
	run_scenario(&dump_folder, true, NULL, 0);
	run_scenario(&exfat_image, true, NULL, 0);
}

/**
 * Runs a scenario, killing the process at step kill_at: the in-memory state
 * is dropped and the queue reloaded from jobs.json. With unsaved_step the
 * stage's side effects happen first but are never persisted, as when the
 * process dies right before writing jobs.json. Returns false once the job
 * finished before reaching kill_at.
 */
static bool kill_and_resume(const scenario *s, unsigned kill_at, bool unsaved_step, bool *stages_hit) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	raiju_job *job = start_job(&w, &processor, s, false, RAIJU_CONFLICT_ASK);
	bool killed = false;
	raiju_stage killed_in = RAIJU_STAGE_DOWNLOADING;
	for (unsigned steps = 0; job != NULL && raiju_job_runnable(job) && steps < MAX_STEPS; steps++) {
		if (steps == kill_at) {
			killed_in = job->stage;
			stages_hit[job->stage] = true;
			if (unsaved_step) {
				raiju_job_step(&w.pipeline, job);
				check_scenario_root(&w, s);
			}
			killed = true;
			break;
		}
		CHECK_INT(raiju_processor_step(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
		job = raiju_processor_find(&processor, JOB_ID);
	}
	raiju_processor_close(&processor);
	if (killed) {
		CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
		CHECK_INT(processor.load_status, RAIJU_STORE_OK);
		job = raiju_processor_find(&processor, JOB_ID);
		CHECK(job != NULL && job->stage == killed_in);
		check_scenario_root(&w, s);
		CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
		check_ready(&w, &processor, s, false);
		raiju_processor_close(&processor);
	}
	world_close(&w);
	return killed;
}

static void a_kill_at_every_stage_resumes(void) {
	for (size_t i = 0; i < sizeof all_scenarios / sizeof all_scenarios[0]; i++) {
		const scenario *s = all_scenarios[i];
		bool stages_hit[STAGE_COUNT] = {false};
		for (int unsaved = 0; unsaved < 2; unsaved++) {
			for (unsigned kill_at = 0; kill_and_resume(s, kill_at, unsaved == 1, stages_hit); kill_at++) {
			}
		}
		CHECK(stages_hit[RAIJU_STAGE_VERIFYING]);
		CHECK(stages_hit[RAIJU_STAGE_CLASSIFYING]);
		CHECK_INT(stages_hit[RAIJU_STAGE_EXTRACTING], s->extracts);
		CHECK(stages_hit[RAIJU_STAGE_INSTALLING]);
		CHECK(stages_hit[RAIJU_STAGE_RESCAN]);
		CHECK(stages_hit[RAIJU_STAGE_NOTIFYING]);
	}
}

static void a_kill_while_downloading_keeps_the_job(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	CHECK_INT(processor.load_status, RAIJU_STORE_MISSING);
	raiju_job_options options = {JOB_ID, "CUSA00001-game.zip", w.install, 0, 0, false, RAIJU_CONFLICT_ASK, NULL};
	CHECK_INT(raiju_processor_add(&processor, &options, RAIJU_CATEGORY_ARCHIVE, NULL), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	raiju_processor_close(&processor);

	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_DOWNLOADING);
	char *path = fixture_path(w.downloads, "aria2-name.zip");
	CHECK(make_dump_zip(path) == 0);
	CHECK_INT(raiju_processor_download_complete(&processor, JOB_ID, path, 0), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_download_complete(&processor, JOB_ID, path, 0), RAIJU_PROCESSOR_WRONG_STAGE);
	job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && strcmp(job->file_name, "CUSA00001-game.zip") == 0);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_READY);
	CHECK(!exists(path));
	free(path);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void a_resumed_extraction_starts_from_a_clean_staging_folder(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	start_job(&w, &processor, &dump_zip, false, RAIJU_CONFLICT_ASK);
	CHECK_INT(raiju_processor_step(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_step(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_find(&processor, JOB_ID)->stage, RAIJU_STAGE_EXTRACTING);
	raiju_processor_close(&processor);

	char *staging = fixture_path(w.staging, JOB_ID);
	char *half = staging ? fixture_path(staging, "half-written.bin") : NULL;
	CHECK(fixture_mkdir(w.staging) == 0 && fixture_mkdir(staging) == 0 && fixture_write(half, "xx", 2) == 0);
	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_step(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_INSTALLING);
	CHECK(!exists(half));
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	check_ready(&w, &processor, &dump_zip, false);
	free(half);
	free(staging);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void pause_keeps_extracting_and_cancel_ends_the_job(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	start_job(&w, &processor, &dump_zip, false, RAIJU_CONFLICT_ASK);
	w.progress_answer = RAIJU_EXTRACT_PAUSE;
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_EXTRACTING);
	w.progress_answer = RAIJU_EXTRACT_CONTINUE;
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	check_ready(&w, &processor, &dump_zip, false);
	raiju_processor_close(&processor);
	world_close(&w);

	CHECK(world_open(&w));
	start_job(&w, &processor, &dump_zip, false, RAIJU_CONFLICT_ASK);
	w.progress_answer = RAIJU_EXTRACT_CANCEL;
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_CANCELLED);
	CHECK(job_dirs_gone(&w));
	check_root(&w, NULL, 0);
	raiju_processor_close(&processor);
	world_close(&w);
}

/**
 * Creates an old copy at <install>/<name>: a folder holding old.txt, or a
 * file holding OLD_CONTENT.
 */
static void make_old_copy(const world *w, const char *name, bool folder) {
	char *path = fixture_path(w->install, name);
	if (folder) {
		char *marker = fixture_path(path, "old.txt");
		CHECK(fixture_mkdir(path) == 0 && fixture_write(marker, OLD_CONTENT, strlen(OLD_CONTENT)) == 0);
		free(marker);
	} else {
		CHECK(fixture_write(path, OLD_CONTENT, strlen(OLD_CONTENT)) == 0);
	}
	free(path);
}

/**
 * Returns whether path is the old folder copy.
 */
static bool verify_old_folder(const char *path) {
	return file_in_equals(path, "old.txt", OLD_CONTENT) && !exists_in(path, "eboot.bin");
}

/**
 * Returns whether path is the old file copy.
 */
static bool verify_old_file(const char *path) {
	return file_equals(path, OLD_CONTENT, strlen(OLD_CONTENT));
}

/**
 * Starts a scenario whose install name is already taken and runs it until
 * it waits for a decision.
 */
static raiju_job *run_into_conflict(world *w, raiju_processor *processor, const scenario *s, bool old_is_folder) {
	CHECK(world_open(w));
	make_old_copy(w, s->install_name, old_is_folder);
	start_job(w, processor, s, false, RAIJU_CONFLICT_ASK);
	CHECK_INT(raiju_processor_run(processor), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_CONFLICT);
	return job;
}

static void a_conflict_waits_across_restarts(void) {
	world w;
	raiju_processor processor;
	raiju_job *job = run_into_conflict(&w, &processor, &dump_zip, true);
	char *taken = fixture_path(w.install, "PPSA24003");
	CHECK(job != NULL && strcmp(job->install_path, taken) == 0);
	free(taken);
	allowed_entry old = {"PPSA24003", verify_old_folder};
	check_root(&w, &old, 1);
	CHECK_INT(raiju_processor_resolve(&processor, JOB_ID, RAIJU_CONFLICT_ASK), RAIJU_PROCESSOR_INVALID);
	CHECK_INT(raiju_processor_retry(&processor, JOB_ID), RAIJU_PROCESSOR_WRONG_STAGE);
	raiju_processor_close(&processor);

	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_CONFLICT);
	check_root(&w, &old, 1);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void rename_installs_beside_the_existing_copy(void) {
	world w;
	raiju_processor processor;
	run_into_conflict(&w, &processor, &dump_zip, true);
	make_old_copy(&w, "PPSA24003-2", true);
	CHECK_INT(raiju_processor_resolve(&processor, JOB_ID, RAIJU_CONFLICT_RENAME), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_READY);
	char *third = fixture_path(w.install, "PPSA24003-3");
	CHECK(job != NULL && strcmp(job->install_path, third) == 0);
	allowed_entry entries[] = {
	    {"PPSA24003", verify_old_folder}, {"PPSA24003-2", verify_old_folder}, {"PPSA24003-3", verify_dump}};
	check_root(&w, entries, 3);
	CHECK(job_dirs_gone(&w));
	free(third);
	raiju_processor_close(&processor);
	world_close(&w);

	run_into_conflict(&w, &processor, &exfat_image, false);
	CHECK_INT(raiju_processor_resolve(&processor, JOB_ID, RAIJU_CONFLICT_RENAME), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	allowed_entry images[] = {{"PPSA24003.exfat", verify_old_file}, {"PPSA24003-2.exfat", verify_exfat}};
	check_root(&w, images, 2);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void replace_swaps_through_the_trash_folder(void) {
	const scenario *cases[] = {&dump_zip, &exfat_image};
	for (size_t i = 0; i < 2; i++) {
		const scenario *s = cases[i];
		bool folder = s == &dump_zip;
		world w;
		raiju_processor processor;
		run_into_conflict(&w, &processor, s, folder);
		CHECK_INT(raiju_processor_resolve(&processor, JOB_ID, RAIJU_CONFLICT_REPLACE), RAIJU_PROCESSOR_OK);
		allowed_entry either[] = {{s->install_name, folder ? verify_old_folder : verify_old_file},
		                          {s->install_name, s->verify}};
		raiju_job *job = raiju_processor_find(&processor, JOB_ID);
		for (unsigned steps = 0; job != NULL && raiju_job_runnable(job) && steps < MAX_STEPS; steps++) {
			CHECK_INT(raiju_processor_step(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
			char *path = fixture_path(w.install, s->install_name);
			CHECK(either[0].verify(path) || either[1].verify(path));
			free(path);
			job = raiju_processor_find(&processor, JOB_ID);
		}
		check_ready(&w, &processor, s, false);
		raiju_processor_close(&processor);
		world_close(&w);
	}
}

static void a_kill_in_the_middle_of_a_replace_finishes_it(void) {
	world w;
	raiju_processor processor;
	run_into_conflict(&w, &processor, &dump_zip, true);
	CHECK_INT(raiju_processor_resolve(&processor, JOB_ID, RAIJU_CONFLICT_REPLACE), RAIJU_PROCESSOR_OK);
	raiju_processor_close(&processor);

	char *old = fixture_path(w.install, "PPSA24003");
	char *trash_job = fixture_path(w.trash, JOB_ID);
	char *parked = trash_job ? fixture_path(trash_job, "PPSA24003") : NULL;
	CHECK(fixture_mkdir(w.trash) == 0 && fixture_mkdir(trash_job) == 0 && rename(old, parked) == 0);
	check_root(&w, NULL, 0);
	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	check_ready(&w, &processor, &dump_zip, false);
	CHECK(!exists(parked));
	raiju_processor_close(&processor);
	world_close(&w);
	free(old);
	free(trash_job);
	free(parked);
}

static void cancel_leaves_the_install_root_alone(void) {
	world w;
	raiju_processor processor;
	run_into_conflict(&w, &processor, &dump_zip, true);
	CHECK_INT(raiju_processor_resolve(&processor, JOB_ID, RAIJU_CONFLICT_CANCEL), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_CANCELLED);
	allowed_entry old = {"PPSA24003", verify_old_folder};
	check_root(&w, &old, 1);
	CHECK(job_dirs_gone(&w));
	char *download = download_path(&w, &dump_zip);
	CHECK(exists(download));
	CHECK_INT(raiju_processor_resolve(&processor, JOB_ID, RAIJU_CONFLICT_RENAME), RAIJU_PROCESSOR_WRONG_STAGE);
	CHECK_INT(raiju_processor_discard(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	CHECK(!exists(download));
	CHECK(raiju_processor_find(&processor, JOB_ID) == NULL);
	check_root(&w, &old, 1);
	free(download);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void preset_policies_resolve_without_asking(void) {
	world w;
	raiju_processor processor;
	CHECK(world_open(&w));
	make_old_copy(&w, "PPSA24003.exfat", false);
	start_job(&w, &processor, &exfat_image, false, RAIJU_CONFLICT_RENAME);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	allowed_entry renamed[] = {{"PPSA24003.exfat", verify_old_file}, {"PPSA24003-2.exfat", verify_exfat}};
	check_root(&w, renamed, 2);
	raiju_processor_close(&processor);
	world_close(&w);

	CHECK(world_open(&w));
	make_old_copy(&w, "PPSA24003.exfat", false);
	start_job(&w, &processor, &exfat_image, false, RAIJU_CONFLICT_REPLACE);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	check_ready(&w, &processor, &exfat_image, false);
	raiju_processor_close(&processor);
	world_close(&w);

	CHECK(world_open(&w));
	make_old_copy(&w, "homebrew.elf", false);
	start_job(&w, &processor, &other_file, false, RAIJU_CONFLICT_RENAME);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	allowed_entry others[] = {{"homebrew.elf", verify_old_file}, {"homebrew-2.elf", verify_other}};
	check_root(&w, others, 2);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void a_failed_rescan_keeps_files_and_retries(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	start_job(&w, &processor, &dump_zip, false, RAIJU_CONFLICT_ASK);
	w.rescan_failures = 1;
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_FAILED);
	if (job != NULL) {
		CHECK_INT(job->failed_stage, RAIJU_STAGE_RESCAN);
		CHECK_STR(job->error, "rescan_failed: shadowmount down");
	}
	char *download = download_path(&w, &dump_zip);
	CHECK(exists(download));
	check_scenario_root(&w, &dump_zip);
	raiju_processor_close(&processor);

	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_find(&processor, JOB_ID)->stage, RAIJU_STAGE_FAILED);
	CHECK_INT(raiju_processor_retry(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_RESCAN && job->error[0] == '\0');
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	check_ready(&w, &processor, &dump_zip, false);
	free(download);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void a_failed_notification_retries_from_notifying(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	start_job(&w, &processor, &exfat_image, false, RAIJU_CONFLICT_ASK);
	w.notify_failures = 1;
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_FAILED && job->failed_stage == RAIJU_STAGE_NOTIFYING);
	CHECK_INT(raiju_processor_retry(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	check_ready(&w, &processor, &exfat_image, false);
	CHECK_INT(w.notifies, 2);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void a_size_mismatch_fails_and_retry_downloads_again(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	char *path = download_path(&w, &exfat_image);
	CHECK(make_exfat(path) == 0);
	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	raiju_job_options options = {JOB_ID, NULL, w.install, 0, 0, false, RAIJU_CONFLICT_ASK, NULL};
	CHECK_INT(raiju_processor_add(&processor, &options, RAIJU_CATEGORY_IMAGE, NULL), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_download_complete(&processor, JOB_ID, path, tree_bytes(path) + 1), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_FAILED && job->failed_stage == RAIJU_STAGE_VERIFYING);
	CHECK(job != NULL && strncmp(job->error, "size_mismatch", 13) == 0);
	CHECK(exists(path));
	check_root(&w, NULL, 0);
	CHECK_INT(raiju_processor_retry(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_find(&processor, JOB_ID)->stage, RAIJU_STAGE_DOWNLOADING);
	CHECK_INT(raiju_processor_download_complete(&processor, JOB_ID, path, tree_bytes(path)), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	check_ready(&w, &processor, &exfat_image, false);
	free(path);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void a_download_failure_fails_the_job(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	raiju_job_options options = {JOB_ID, NULL, w.install, 0, 0, false, RAIJU_CONFLICT_ASK, NULL};
	CHECK_INT(raiju_processor_add(&processor, &options, RAIJU_CATEGORY_OTHER, NULL), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_add(&processor, &options, RAIJU_CATEGORY_OTHER, NULL), RAIJU_PROCESSOR_DUPLICATE);
	CHECK_INT(raiju_processor_download_failed(&processor, JOB_ID, "http 404\n"), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_FAILED && job->failed_stage == RAIJU_STAGE_DOWNLOADING);
	CHECK(job != NULL && strcmp(job->error, "http 404 ") == 0);
	CHECK_INT(raiju_processor_retry(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_find(&processor, JOB_ID)->stage, RAIJU_STAGE_DOWNLOADING);
	CHECK_INT(raiju_processor_download_failed(&processor, "nope", NULL), RAIJU_PROCESSOR_NOT_FOUND);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void a_bad_image_fails_classification_and_discard_removes_it(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	char *path = fixture_path(w.downloads, "PPSA24003.ffpkg");
	static const char html[] = "<!doctype html><title>404</title>";
	CHECK(fixture_write(path, html, sizeof html - 1) == 0);
	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	raiju_job_options options = {JOB_ID, NULL, w.install, 0, 0, false, RAIJU_CONFLICT_ASK, NULL};
	CHECK_INT(raiju_processor_add(&processor, &options, RAIJU_CATEGORY_IMAGE, NULL), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_download_complete(&processor, JOB_ID, path, 0), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_FAILED && job->failed_stage == RAIJU_STAGE_CLASSIFYING);
	CHECK(job != NULL && strcmp(job->error, "bad_image: header_mismatch") == 0);
	CHECK(exists(path));
	check_root(&w, NULL, 0);
	CHECK_INT(raiju_processor_discard(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	CHECK(!exists(path));
	CHECK_INT(processor.list.count, 0);
	free(path);
	raiju_processor_close(&processor);
	raiju_processor reopened;
	CHECK_INT(raiju_processor_open(&reopened, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	CHECK_INT(reopened.list.count, 0);
	CHECK(raiju_processor_find(&reopened, JOB_ID) == NULL);
	raiju_processor_close(&reopened);
	world_close(&w);
}

static void a_broken_archive_fails_extraction_and_retries(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	start_job(&w, &processor, &dump_zip, false, RAIJU_CONFLICT_ASK);
	char *path = download_path(&w, &dump_zip);
	FILE *stream = fopen(path, "r+b");
	CHECK(stream != NULL);
	if (stream != NULL) {
		fseek(stream, 40, SEEK_SET);
		fwrite("garbagegarbagegarbage", 1, 21, stream);
		fclose(stream);
	}
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_FAILED && job->failed_stage == RAIJU_STAGE_EXTRACTING);
	CHECK(exists(path));
	check_root(&w, NULL, 0);
	CHECK(make_dump_zip(path) == 0);
	CHECK_INT(raiju_processor_retry(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_find(&processor, JOB_ID)->stage, RAIJU_STAGE_EXTRACTING);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	check_ready(&w, &processor, &dump_zip, false);
	free(path);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void a_missing_title_id_asks_the_user(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	static const scenario unnamed = {
	    "unnamed", "game.ffpfs", make_other, "CUSA12345.ffpfs", verify_other, false, false};
	start_job(&w, &processor, &unnamed, false, RAIJU_CONFLICT_ASK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_AWAITING_TITLE_ID);
	raiju_processor_close(&processor);

	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_set_title_id(&processor, JOB_ID, "ppsa12345"), RAIJU_PROCESSOR_INVALID);
	CHECK_INT(raiju_processor_set_title_id(&processor, JOB_ID, "CUSA12345"), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_set_title_id(&processor, JOB_ID, "CUSA12345"), RAIJU_PROCESSOR_WRONG_STAGE);
	job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->title_source == RAIJU_TITLE_SOURCE_USER);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	check_ready(&w, &processor, &unnamed, false);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void an_edited_payload_path_outside_the_job_is_refused(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	start_job(&w, &processor, &dump_zip, false, RAIJU_CONFLICT_ASK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	while (job != NULL && job->stage != RAIJU_STAGE_INSTALLING) {
		CHECK_INT(raiju_processor_step(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
		job = raiju_processor_find(&processor, JOB_ID);
	}
	char *elsewhere = fixture_path(w.root, "elsewhere");
	CHECK(job != NULL && fixture_mkdir(elsewhere) == 0);
	if (job != NULL) {
		snprintf(job->payload_path, sizeof job->payload_path, "%s", elsewhere);
	}
	CHECK_INT(raiju_processor_step(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_FAILED && strcmp(job->error, "payload_outside_job") == 0);
	CHECK(exists(elsewhere));
	check_root(&w, NULL, 0);
	free(elsewhere);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void space_accounting_counts_archive_and_contents(void) {
	const uint64_t gib = RAIJU_SPACE_MARGIN_BYTES;
	raiju_space_need need = raiju_space_needed(RAIJU_CATEGORY_ARCHIVE, 10 * gib, 30 * gib);
	CHECK(need.required_bytes == 41 * gib && !need.estimated);
	need = raiju_space_needed(RAIJU_CATEGORY_ARCHIVE, 10 * gib, 0);
	CHECK(need.required_bytes == 21 * gib && need.estimated);
	need = raiju_space_needed(RAIJU_CATEGORY_IMAGE, 10 * gib, 99 * gib);
	CHECK(need.required_bytes == 11 * gib && !need.estimated);
	need = raiju_space_needed(RAIJU_CATEGORY_ARCHIVE, UINT64_MAX - 1, UINT64_MAX - 1);
	CHECK(need.required_bytes == UINT64_MAX);

	world w;
	CHECK(world_open(&w));
	need = raiju_space_needed(RAIJU_CATEGORY_ARCHIVE, 10 * gib, 30 * gib);
	w.free_bytes = 41 * gib;
	raiju_space_report report = raiju_space_check(need, w.install, fake_free_space, &w);
	CHECK_INT(report.status, RAIJU_SPACE_OK);
	w.free_bytes = 41 * gib - 1;
	report = raiju_space_check(need, w.install, fake_free_space, &w);
	CHECK_INT(report.status, RAIJU_SPACE_NOT_ENOUGH);
	CHECK(report.free_bytes == 41 * gib - 1 && report.required_bytes == 41 * gib);
	w.free_fails = true;
	CHECK_INT(raiju_space_check(need, w.install, fake_free_space, &w).status, RAIJU_SPACE_UNKNOWN);
	w.free_fails = false;
	uint64_t real_free = 0;
	CHECK_INT(raiju_statvfs_free_space(w.install, &real_free, NULL), 0);
	CHECK(real_free > 0);
	CHECK_INT(raiju_space_check(raiju_space_needed(RAIJU_CATEGORY_IMAGE, 1, 0), w.install, NULL, NULL).status,
	          real_free > gib ? RAIJU_SPACE_OK : RAIJU_SPACE_NOT_ENOUGH);

	raiju_processor processor;
	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	w.free_bytes = 20 * gib;
	raiju_job_options options = {JOB_ID, "game.7z", w.install, 10 * gib, 30 * gib, false, RAIJU_CONFLICT_ASK, NULL};
	CHECK_INT(raiju_processor_add(&processor, &options, RAIJU_CATEGORY_ARCHIVE, &report), RAIJU_PROCESSOR_NO_SPACE);
	CHECK_INT(report.status, RAIJU_SPACE_NOT_ENOUGH);
	CHECK(raiju_processor_find(&processor, JOB_ID) == NULL);
	w.free_bytes = 50 * gib;
	CHECK_INT(raiju_processor_add(&processor, &options, RAIJU_CATEGORY_ARCHIVE, &report), RAIJU_PROCESSOR_OK);
	CHECK_INT(report.status, RAIJU_SPACE_OK);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void stage_rules_and_names_are_stable(void) {
	for (int stage = 0; stage < STAGE_COUNT; stage++) {
		raiju_stage parsed;
		CHECK(raiju_stage_from_name(raiju_stage_name((raiju_stage)stage), &parsed));
		CHECK_INT(parsed, stage);
		raiju_job job;
		memset(&job, 0, sizeof job);
		job.stage = (raiju_stage)stage;
		bool runnable = stage >= RAIJU_STAGE_VERIFYING && stage <= RAIJU_STAGE_NOTIFYING;
		CHECK_INT(raiju_job_runnable(&job), runnable);
		bool finished = stage == RAIJU_STAGE_READY || stage == RAIJU_STAGE_FAILED || stage == RAIJU_STAGE_CANCELLED;
		CHECK_INT(raiju_job_finished(&job), finished);
	}
	raiju_stage parsed;
	CHECK(!raiju_stage_from_name("Ready", &parsed));
	CHECK_STR(raiju_stage_name(RAIJU_STAGE_AWAITING_TITLE_ID), "awaiting_title_id");
	CHECK_STR(raiju_processor_status_name(RAIJU_PROCESSOR_NO_SPACE), "no_space");
	CHECK_STR(raiju_space_status_name(RAIJU_SPACE_NOT_ENOUGH), "not_enough");

	raiju_pipeline pipeline = {"/tmp/staging", "/tmp/trash", NULL, NULL, NULL, NULL, NULL};
	raiju_job job;
	raiju_job_options options = {JOB_ID, NULL, "/data/homebrew", 0, 0, false, RAIJU_CONFLICT_ASK, NULL};
	CHECK_INT(raiju_job_init(&job, &options), 0);
	CHECK(!raiju_job_step(&pipeline, &job));
	CHECK_INT(raiju_job_retry(&job), -1);
	CHECK_INT(raiju_job_set_title_id(&job, "PPSA24003"), -1);
	CHECK_INT(raiju_job_resolve_conflict(&pipeline, &job, RAIJU_CONFLICT_RENAME), -1);
	options.id = "../escape";
	CHECK_INT(raiju_job_init(&job, &options), -1);
	options.id = JOB_ID;
	options.install_root = "relative/root";
	CHECK_INT(raiju_job_init(&job, &options), -1);
	options.install_root = "/data/../etc";
	CHECK_INT(raiju_job_init(&job, &options), -1);
	options.install_root = "/data/homebrew";
	options.conflict_policy = RAIJU_CONFLICT_CANCEL;
	CHECK_INT(raiju_job_init(&job, &options), -1);
	options.conflict_policy = RAIJU_CONFLICT_ASK;
	options.file_name = "a/b.zip";
	CHECK_INT(raiju_job_init(&job, &options), -1);
}

static void cancel_removes_staging_and_keeps_the_download(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	start_job(&w, &processor, &dump_zip, false, RAIJU_CONFLICT_ASK);
	CHECK_INT(raiju_processor_step(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_step(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_find(&processor, JOB_ID)->stage, RAIJU_STAGE_EXTRACTING);
	char *staging = fixture_path(w.staging, JOB_ID);
	CHECK(staging != NULL && fixture_mkdir(w.staging) == 0 && fixture_mkdir(staging) == 0);
	CHECK_INT(raiju_processor_cancel(&processor, JOB_ID), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_CANCELLED);
	CHECK(!exists(staging));
	char *download = download_path(&w, &dump_zip);
	CHECK(exists(download));
	check_root(&w, NULL, 0);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_find(&processor, JOB_ID)->stage, RAIJU_STAGE_CANCELLED);
	CHECK_INT(raiju_processor_cancel(&processor, JOB_ID), RAIJU_PROCESSOR_WRONG_STAGE);
	CHECK_INT(raiju_processor_cancel(&processor, "missing"), RAIJU_PROCESSOR_NOT_FOUND);
	free(download);
	free(staging);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void cancel_is_refused_once_ready(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	start_job(&w, &processor, &exfat_image, false, RAIJU_CONFLICT_ASK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_find(&processor, JOB_ID)->stage, RAIJU_STAGE_READY);
	CHECK_INT(raiju_processor_cancel(&processor, JOB_ID), RAIJU_PROCESSOR_WRONG_STAGE);
	raiju_processor_close(&processor);
	world_close(&w);
}

static void a_title_override_survives_classification(void) {
	world w;
	CHECK(world_open(&w));
	raiju_processor processor;
	char *path = download_path(&w, &exfat_image);
	CHECK(make_exfat(path) == 0);
	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	raiju_job_options options = {JOB_ID, NULL, w.install, 0, 0, false, RAIJU_CONFLICT_ASK, "CUSA99999"};
	CHECK_INT(raiju_processor_add(&processor, &options, RAIJU_CATEGORY_IMAGE, NULL), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_download_complete(&processor, JOB_ID, path, tree_bytes(path)), RAIJU_PROCESSOR_OK);
	CHECK_INT(raiju_processor_run(&processor), RAIJU_PROCESSOR_OK);
	raiju_job *job = raiju_processor_find(&processor, JOB_ID);
	CHECK(job != NULL && job->stage == RAIJU_STAGE_READY);
	if (job != NULL) {
		CHECK_STR(job->title_id, "CUSA99999");
		CHECK_INT(job->title_source, RAIJU_TITLE_SOURCE_USER);
		char *installed = fixture_path(w.install, "CUSA99999.exfat");
		CHECK(verify_exfat(installed));
		CHECK_STR(job->install_path, installed);
		free(installed);
	}
	raiju_processor_close(&processor);
	world_close(&w);

	CHECK(world_open(&w));
	CHECK_INT(raiju_processor_open(&processor, &w.pipeline, w.jobs), RAIJU_PROCESSOR_OK);
	raiju_job_options bad = {JOB_ID, NULL, w.install, 0, 0, false, RAIJU_CONFLICT_ASK, "not-a-title"};
	CHECK_INT(raiju_processor_add(&processor, &bad, RAIJU_CATEGORY_IMAGE, NULL), RAIJU_PROCESSOR_INVALID);
	raiju_processor_close(&processor);
	world_close(&w);
	free(path);
}

void run_pipeline_tests(void) {
	RUN_TEST(archive_goes_through_every_stage);
	RUN_TEST(images_and_other_files_skip_extraction);
	RUN_TEST(title_ids_come_from_param_json_first);
	RUN_TEST(keep_archive_leaves_the_download);
	RUN_TEST(a_kill_at_every_stage_resumes);
	RUN_TEST(a_kill_while_downloading_keeps_the_job);
	RUN_TEST(a_resumed_extraction_starts_from_a_clean_staging_folder);
	RUN_TEST(pause_keeps_extracting_and_cancel_ends_the_job);
	RUN_TEST(a_conflict_waits_across_restarts);
	RUN_TEST(rename_installs_beside_the_existing_copy);
	RUN_TEST(replace_swaps_through_the_trash_folder);
	RUN_TEST(a_kill_in_the_middle_of_a_replace_finishes_it);
	RUN_TEST(cancel_leaves_the_install_root_alone);
	RUN_TEST(cancel_removes_staging_and_keeps_the_download);
	RUN_TEST(cancel_is_refused_once_ready);
	RUN_TEST(a_title_override_survives_classification);
	RUN_TEST(preset_policies_resolve_without_asking);
	RUN_TEST(a_failed_rescan_keeps_files_and_retries);
	RUN_TEST(a_failed_notification_retries_from_notifying);
	RUN_TEST(a_size_mismatch_fails_and_retry_downloads_again);
	RUN_TEST(a_download_failure_fails_the_job);
	RUN_TEST(a_bad_image_fails_classification_and_discard_removes_it);
	RUN_TEST(a_broken_archive_fails_extraction_and_retries);
	RUN_TEST(a_missing_title_id_asks_the_user);
	RUN_TEST(an_edited_payload_path_outside_the_job_is_refused);
	RUN_TEST(space_accounting_counts_archive_and_contents);
	RUN_TEST(stage_rules_and_names_are_stable);
}
