#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/pipeline.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "raiju/image_fs.h"
#include "raiju/inspect.h"

#define TREE_MAX_DEPTH 64
#define RENAME_SUFFIX_MAX 999

typedef struct {
	const raiju_pipeline *pipeline;
	raiju_job *job;
} progress_bridge;

/**
 * Writes dir/name into out. Returns -1 when it does not fit.
 */
static int join_path(char *out, size_t out_size, const char *dir, const char *name) {
	size_t dir_len = strlen(dir);
	const char *separator = (dir_len > 0 && dir[dir_len - 1] == '/') ? "" : "/";
	int written = snprintf(out, out_size, "%s%s%s", dir, separator, name);
	return (written < 0 || (size_t)written >= out_size) ? -1 : 0;
}

/**
 * Copies text into a fixed buffer, failing instead of truncating.
 */
static int copy_bounded(char *out, size_t out_size, const char *text) {
	size_t length = strlen(text);
	if (length >= out_size) {
		return -1;
	}
	memcpy(out, text, length + 1);
	return 0;
}

/**
 * Returns the final path component.
 */
static const char *base_name(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/**
 * Returns whether something exists at path, without following symlinks.
 */
static bool path_exists(const char *path) {
	struct stat info;
	return lstat(path, &info) == 0;
}

/**
 * Returns whether path is root itself or lies inside it, comparing whole
 * components.
 */
static bool path_within(const char *path, const char *root) {
	size_t root_len = strlen(root);
	while (root_len > 1 && root[root_len - 1] == '/') {
		root_len--;
	}
	return strncmp(path, root, root_len) == 0 && (path[root_len] == '\0' || path[root_len] == '/');
}

/**
 * Creates a directory unless a real directory is already there.
 */
static int ensure_dir(const char *path) {
	if (mkdir(path, 0755) == 0) {
		return 0;
	}
	struct stat info;
	return (errno == EEXIST && lstat(path, &info) == 0 && S_ISDIR(info.st_mode)) ? 0 : -1;
}

/**
 * Removes a file or a whole directory tree without following symlinks. A
 * missing path counts as removed.
 */
static int remove_tree(const char *path) {
	struct stat info;
	if (lstat(path, &info) != 0) {
		return errno == ENOENT ? 0 : -1;
	}
	if (!S_ISDIR(info.st_mode)) {
		return unlink(path);
	}
	if (raiju_staging_clear(path) != 0) {
		return -1;
	}
	return rmdir(path);
}

/**
 * Adds up the sizes of the regular files under path without following
 * symlinks.
 */
static int tree_size(const char *path, unsigned depth, uint64_t *total) {
	struct stat info;
	if (depth > TREE_MAX_DEPTH || lstat(path, &info) != 0) {
		return -1;
	}
	if (S_ISREG(info.st_mode)) {
		*total += (uint64_t)info.st_size;
		return 0;
	}
	if (!S_ISDIR(info.st_mode)) {
		return 0;
	}
	DIR *dir = opendir(path);
	if (dir == NULL) {
		return -1;
	}
	int status = 0;
	struct dirent *item;
	char child[RAIJU_JOB_PATH_MAX];
	while (status == 0 && (item = readdir(dir)) != NULL) {
		if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
			continue;
		}
		status = (join_path(child, sizeof child, path, item->d_name) == 0) ? tree_size(child, depth + 1, total) : -1;
	}
	closedir(dir);
	return status;
}

/**
 * Fsyncs one file or directory.
 */
static int sync_path(const char *path) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	int status = fsync(fd);
	close(fd);
	return status;
}

/**
 * Fsyncs every regular file and directory under path, the single flush pass
 * that replaces per-file fsync during extraction.
 */
static int sync_tree(const char *path, unsigned depth) {
	struct stat info;
	if (depth > TREE_MAX_DEPTH || lstat(path, &info) != 0) {
		return -1;
	}
	if (S_ISREG(info.st_mode)) {
		return sync_path(path);
	}
	if (!S_ISDIR(info.st_mode)) {
		return 0;
	}
	DIR *dir = opendir(path);
	if (dir == NULL) {
		return -1;
	}
	int status = 0;
	struct dirent *item;
	char child[RAIJU_JOB_PATH_MAX];
	while (status == 0 && (item = readdir(dir)) != NULL) {
		if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
			continue;
		}
		status = (join_path(child, sizeof child, path, item->d_name) == 0) ? sync_tree(child, depth + 1) : -1;
	}
	closedir(dir);
	return status == 0 ? sync_path(path) : status;
}

/**
 * Fails the job with a formatted error text.
 */
static bool fail_with(raiju_job *job, const char *prefix, const char *detail) {
	char error[RAIJU_JOB_ERROR_MAX];
	if (detail != NULL && detail[0] != '\0') {
		snprintf(error, sizeof error, "%s: %s", prefix, detail);
	} else {
		snprintf(error, sizeof error, "%s", prefix);
	}
	raiju_job_fail(job, error);
	return true;
}

int raiju_job_dir(const char *root, const raiju_job *job, char *out, size_t out_size) {
	if (root == NULL || job == NULL || !raiju_job_id_valid(job->id)) {
		return -1;
	}
	return join_path(out, out_size, root, job->id);
}

/**
 * Picks the install name from param.json, then the download's name, then
 * the name of the payload inside it, and moves the job on to INSTALLING, or
 * to AWAITING_TITLE_ID when none of them has a title ID.
 */
static void resolve_title(raiju_job *job, const char *param_title_id, const char *inner_name) {
	if (job->title_source == RAIJU_TITLE_SOURCE_USER && job->title_id[0] != '\0') {
		job->install_path[0] = '\0';
		job->conflict_choice = RAIJU_CONFLICT_ASK;
		job->stage = RAIJU_STAGE_INSTALLING;
		return;
	}
	raiju_title_resolution resolution = raiju_resolve_title_id(param_title_id, job->file_name);
	if (resolution.source == RAIJU_TITLE_SOURCE_NONE && inner_name != NULL) {
		resolution = raiju_resolve_title_id(NULL, inner_name);
	}
	job->title_source = resolution.source;
	job->names_disagree = resolution.names_disagree;
	job->install_path[0] = '\0';
	job->conflict_choice = RAIJU_CONFLICT_ASK;
	if (resolution.source == RAIJU_TITLE_SOURCE_NONE) {
		job->title_id[0] = '\0';
		job->stage = RAIJU_STAGE_AWAITING_TITLE_ID;
	} else {
		memcpy(job->title_id, resolution.title_id, sizeof job->title_id);
		job->stage = RAIJU_STAGE_INSTALLING;
	}
}

/**
 * Reads the title ID from param.json inside an image into out and returns
 * out, or returns NULL when the image has none.
 */
static const char *image_title_id(const char *path, char out[RAIJU_TITLE_ID_LEN + 1]) {
	return raiju_title_id_from_image(path, out, NULL) == RAIJU_PARAM_OK ? out : NULL;
}

/**
 * Records the payload found by layout normalization and resolves its title.
 */
static bool accept_layout(raiju_job *job, const raiju_extract_result *result) {
	if (copy_bounded(job->payload_path, sizeof job->payload_path, result->payload_path) != 0) {
		return fail_with(job, "payload_path_too_long", NULL);
	}
	if (result->layout == RAIJU_LAYOUT_FOLDER_DUMP) {
		job->payload_type = RAIJU_PAYLOAD_FOLDER;
		if (result->param_json != NULL) {
			raiju_display_name_from_param_json(result->param_json, result->param_json_len, job->display_name,
			                                   sizeof job->display_name);
		}
		resolve_title(job, result->param_status == RAIJU_PARAM_OK ? result->title_id : NULL, NULL);
	} else {
		char title_id[RAIJU_TITLE_ID_LEN + 1];
		job->payload_type = RAIJU_PAYLOAD_IMAGE;
		raiju_display_name_from_image(job->payload_path, job->display_name, sizeof job->display_name);
		resolve_title(job, image_title_id(job->payload_path, title_id), base_name(job->payload_path));
	}
	return true;
}

/**
 * VERIFYING: the download's size must match what the server announced.
 */
static bool step_verifying(raiju_job *job) {
	struct stat info;
	if (lstat(job->source_path, &info) != 0) {
		return fail_with(job, "source_missing", NULL);
	}
	uint64_t size = 0;
	if (S_ISREG(info.st_mode)) {
		size = (uint64_t)info.st_size;
	} else if (!S_ISDIR(info.st_mode) || tree_size(job->source_path, 0, &size) != 0) {
		return fail_with(job, "source_unreadable", NULL);
	}
	if (job->expected_size != 0 && size != job->expected_size) {
		char detail[64];
		snprintf(detail, sizeof detail, "%llu of %llu bytes", (unsigned long long)size,
		         (unsigned long long)job->expected_size);
		return fail_with(job, "size_mismatch", detail);
	}
	job->stage = RAIJU_STAGE_CLASSIFYING;
	return true;
}

/**
 * CLASSIFYING: magic bytes decide between archive, image and anything else;
 * images with a bad header or structure fail. A downloaded folder is
 * normalized in place like extracted staging.
 */
static bool step_classifying(raiju_job *job) {
	struct stat info;
	if (lstat(job->source_path, &info) != 0) {
		return fail_with(job, "source_missing", NULL);
	}
	if (S_ISDIR(info.st_mode)) {
		raiju_extract_result result;
		memset(&result, 0, sizeof result);
		raiju_extract_status status = raiju_normalize_layout(job->source_path, &result);
		job->kind = result.layout == RAIJU_LAYOUT_SINGLE_IMAGE ? result.image.classification.kind : RAIJU_KIND_UNKNOWN;
		bool changed = status == RAIJU_EXTRACT_OK ? accept_layout(job, &result)
		                                          : fail_with(job, "layout", raiju_extract_status_name(status));
		raiju_extract_result_free(&result);
		return changed;
	}
	raiju_inspection inspection;
	if (raiju_inspect_file(job->source_path, &inspection) != 0) {
		return fail_with(job, "source_unreadable", NULL);
	}
	job->kind = inspection.classification.kind;
	if (!raiju_inspection_accepted(&inspection)) {
		const char *detail = inspection.classification.header_mismatch ? "header_mismatch"
		                                                               : raiju_image_status_name(inspection.image.status);
		return fail_with(job, "bad_image", detail);
	}
	switch (inspection.classification.category) {
	case RAIJU_CATEGORY_ARCHIVE:
		job->stage = RAIJU_STAGE_EXTRACTING;
		return true;
	case RAIJU_CATEGORY_IMAGE: {
		char title_id[RAIJU_TITLE_ID_LEN + 1];
		job->payload_type = RAIJU_PAYLOAD_IMAGE;
		memcpy(job->payload_path, job->source_path, sizeof job->payload_path);
		raiju_display_name_from_image(job->source_path, job->display_name, sizeof job->display_name);
		resolve_title(job, image_title_id(job->source_path, title_id), NULL);
		return true;
	}
	case RAIJU_CATEGORY_OTHER:
	default:
		job->payload_type = RAIJU_PAYLOAD_FILE;
		memcpy(job->payload_path, job->source_path, sizeof job->payload_path);
		job->install_path[0] = '\0';
		job->stage = RAIJU_STAGE_INSTALLING;
		return true;
	}
}

/**
 * Forwards libarchive progress to the job and the pipeline's progress hook.
 */
static raiju_extract_control forward_progress(uint64_t written, uint64_t total, void *context) {
	progress_bridge *bridge = context;
	bridge->job->progress_done = written;
	bridge->job->progress_total = total;
	if (bridge->pipeline->progress == NULL) {
		return RAIJU_EXTRACT_CONTINUE;
	}
	return bridge->pipeline->progress(bridge->job, written, total, bridge->pipeline->context);
}

/**
 * EXTRACTING: wipes the job's staging folder, then extracts the archive
 * into it from the beginning.
 */
static bool step_extracting(const raiju_pipeline *pipeline, raiju_job *job) {
	char staging[RAIJU_JOB_PATH_MAX];
	if (raiju_job_dir(pipeline->staging_root, job, staging, sizeof staging) != 0 ||
	    ensure_dir(pipeline->staging_root) != 0) {
		return fail_with(job, "staging_unavailable", NULL);
	}
	if (path_exists(staging) && raiju_staging_clear(staging) != 0) {
		return fail_with(job, "staging_clear_failed", NULL);
	}
	progress_bridge bridge = {pipeline, job};
	raiju_extract_request request = {job->source_path, staging, job->uncompressed_size, forward_progress, &bridge};
	raiju_extract_result result;
	memset(&result, 0, sizeof result);
	job->progress_done = 0;
	job->progress_total = job->uncompressed_size;
	raiju_extract_status status = raiju_extract_archive(&request, &result);
	bool changed = true;
	switch (status) {
	case RAIJU_EXTRACT_OK:
		changed = accept_layout(job, &result);
		break;
	case RAIJU_EXTRACT_PAUSED:
		changed = false;
		break;
	case RAIJU_EXTRACT_CANCELLED:
		remove_tree(staging);
		job->stage = RAIJU_STAGE_CANCELLED;
		break;
	default:
		changed = fail_with(job, raiju_extract_status_name(status), result.failed_entry);
		break;
	}
	raiju_extract_result_free(&result);
	return changed;
}

/**
 * Returns the lowercase extension an image is installed with: the image
 * extension of its name when it has one, otherwise the one its kind implies.
 */
static const char *image_extension(const raiju_job *job) {
	raiju_kind kind = raiju_image_kind_from_name(base_name(job->payload_path));
	if (kind == RAIJU_KIND_UNKNOWN) {
		kind = job->kind;
	}
	switch (kind) {
	case RAIJU_KIND_EXFAT:
		return "exfat";
	case RAIJU_KIND_UFS2:
		return "ffpkg";
	case RAIJU_KIND_PFS:
		return "ffpfs";
	case RAIJU_KIND_PFSC:
		return "ffpfsc";
	default:
		return NULL;
	}
}

/**
 * Writes the install name with an optional -<n> suffix: <TITLE_ID>.<ext>
 * for images, <TITLE_ID> for folders, the original name for other files
 * (with the suffix before its last extension).
 */
static int install_name(const raiju_job *job, unsigned suffix, char *out, size_t out_size) {
	char tag[16] = "";
	if (suffix > 0) {
		snprintf(tag, sizeof tag, "-%u", suffix);
	}
	int written;
	if (job->payload_type == RAIJU_PAYLOAD_IMAGE) {
		const char *extension = image_extension(job);
		if (extension == NULL) {
			return -1;
		}
		written = snprintf(out, out_size, "%s%s.%s", job->title_id, tag, extension);
	} else if (job->payload_type == RAIJU_PAYLOAD_FOLDER) {
		written = snprintf(out, out_size, "%s%s", job->title_id, tag);
	} else {
		const char *dot = strrchr(job->file_name, '.');
		size_t stem = (dot != NULL && dot != job->file_name) ? (size_t)(dot - job->file_name) : strlen(job->file_name);
		written = snprintf(out, out_size, "%.*s%s%s", (int)stem, job->file_name, tag, job->file_name + stem);
	}
	return (written < 0 || (size_t)written >= out_size || !raiju_job_file_name_valid(out)) ? -1 : 0;
}

/**
 * Writes the install path for a suffix into out.
 */
static int install_path_for(const raiju_job *job, unsigned suffix, char *out, size_t out_size) {
	char name[RAIJU_JOB_NAME_MAX];
	return install_name(job, suffix, name, sizeof name) == 0 ? join_path(out, out_size, job->install_root, name) : -1;
}

/**
 * Finds the first free <name>-<n> path, n from 2, and stores it as the
 * install path.
 */
static int pick_free_install_path(raiju_job *job) {
	char path[RAIJU_JOB_PATH_MAX];
	for (unsigned suffix = 2; suffix <= RENAME_SUFFIX_MAX; suffix++) {
		if (install_path_for(job, suffix, path, sizeof path) != 0) {
			return -1;
		}
		if (!path_exists(path)) {
			memcpy(job->install_path, path, sizeof job->install_path);
			return 0;
		}
	}
	return -1;
}

/**
 * Returns whether the payload is one the job itself produced: the download,
 * something inside a downloaded folder, or something inside its staging
 * folder. Guards against an edited jobs.json moving arbitrary paths.
 */
static bool payload_belongs_to_job(const raiju_pipeline *pipeline, const raiju_job *job) {
	char staging[RAIJU_JOB_PATH_MAX];
	if (path_within(job->payload_path, job->source_path)) {
		return true;
	}
	if (raiju_job_dir(pipeline->staging_root, job, staging, sizeof staging) == 0 &&
	    path_within(job->payload_path, staging)) {
		return true;
	}
	char *canonical_root = realpath(pipeline->staging_root, NULL);
	bool within = canonical_root != NULL && raiju_job_dir(canonical_root, job, staging, sizeof staging) == 0 &&
	              path_within(job->payload_path, staging);
	free(canonical_root);
	return within;
}

/**
 * First half of INSTALLING: decides the destination and persists it before
 * anything moves. An existing destination pauses the job in CONFLICT, or is
 * resolved right away by a preset rename or replace policy.
 */
static bool prepare_install(raiju_job *job) {
	char path[RAIJU_JOB_PATH_MAX];
	if (install_path_for(job, 0, path, sizeof path) != 0) {
		return fail_with(job, "install_name_invalid", NULL);
	}
	memcpy(job->install_path, path, sizeof job->install_path);
	job->conflict_choice = RAIJU_CONFLICT_ASK;
	if (!path_exists(path)) {
		return true;
	}
	switch (job->conflict_policy) {
	case RAIJU_CONFLICT_RENAME:
		job->conflict_choice = RAIJU_CONFLICT_RENAME;
		if (pick_free_install_path(job) != 0) {
			return fail_with(job, "no_free_name", NULL);
		}
		return true;
	case RAIJU_CONFLICT_REPLACE:
		job->conflict_choice = RAIJU_CONFLICT_REPLACE;
		return true;
	default:
		job->stage = RAIJU_STAGE_CONFLICT;
		return true;
	}
}

/**
 * Moves the payload to the install path with one rename, after flushing it
 * to disk, then flushes the install root.
 */
static int move_into_place(const raiju_job *job) {
	if (sync_tree(job->payload_path, 0) != 0 || rename(job->payload_path, job->install_path) != 0) {
		return -1;
	}
	sync_path(job->install_root);
	return 0;
}

/**
 * Second half of INSTALLING: performs the committed move. A rerun after a
 * crash finds the payload already gone and the destination present, and
 * treats the move as done. Replace parks the old copy in
 * <trash_root>/<id>/ first and deletes it after the swap, so the install
 * root always holds either the old or the new copy.
 */
static bool commit_install(const raiju_pipeline *pipeline, raiju_job *job) {
	bool source_exists = path_exists(job->payload_path);
	bool destination_exists = path_exists(job->install_path);
	if (job->conflict_choice == RAIJU_CONFLICT_REPLACE) {
		char trash[RAIJU_JOB_PATH_MAX];
		char parked[RAIJU_JOB_PATH_MAX];
		if (raiju_job_dir(pipeline->trash_root, job, trash, sizeof trash) != 0 ||
		    join_path(parked, sizeof parked, trash, base_name(job->install_path)) != 0) {
			return fail_with(job, "trash_unavailable", NULL);
		}
		if (source_exists && destination_exists) {
			if (ensure_dir(pipeline->trash_root) != 0 || ensure_dir(trash) != 0 || remove_tree(parked) != 0 ||
			    rename(job->install_path, parked) != 0) {
				return fail_with(job, "trash_move_failed", strerror(errno));
			}
			destination_exists = false;
		}
		if (source_exists && move_into_place(job) != 0) {
			return fail_with(job, "install_failed", strerror(errno));
		}
		if (!source_exists && !destination_exists) {
			return fail_with(job, "payload_missing", NULL);
		}
		if (remove_tree(trash) != 0) {
			return fail_with(job, "trash_cleanup_failed", strerror(errno));
		}
	} else {
		if (source_exists && destination_exists) {
			job->install_path[0] = '\0';
			return prepare_install(job);
		}
		if (!source_exists && !destination_exists) {
			return fail_with(job, "payload_missing", NULL);
		}
		if (source_exists && move_into_place(job) != 0) {
			return fail_with(job, "install_failed", strerror(errno));
		}
	}
	job->stage = RAIJU_STAGE_RESCAN;
	return true;
}

/**
 * INSTALLING: commits a destination on the first run, moves the payload on
 * the second.
 */
static bool step_installing(const raiju_pipeline *pipeline, raiju_job *job) {
	if (!payload_belongs_to_job(pipeline, job)) {
		return fail_with(job, "payload_outside_job", NULL);
	}
	if (ensure_dir(job->install_root) != 0) {
		return fail_with(job, "install_root_unavailable", NULL);
	}
	if (job->install_path[0] == '\0') {
		return prepare_install(job);
	}
	return commit_install(pipeline, job);
}

/**
 * Runs a rescan or notify hook and moves the job to next on success.
 */
static bool run_hook(raiju_job *job, raiju_rescan_fn hook, void *context, const char *name, raiju_stage next) {
	char error[RAIJU_JOB_ERROR_MAX] = "";
	if (hook != NULL && hook(job, error, sizeof error, context) != 0) {
		return fail_with(job, name, error);
	}
	job->stage = next;
	return true;
}

/**
 * Returns whether cleanup should delete the download: archives and
 * downloaded folders whose payload was taken out of them, unless the user
 * asked to keep the archive. Images and other files were moved, not copied.
 */
static bool should_delete_download(const raiju_job *job) {
	if (job->keep_archive || strcmp(job->payload_path, job->source_path) == 0) {
		return false;
	}
	return raiju_kind_category(job->kind) == RAIJU_CATEGORY_ARCHIVE || path_within(job->payload_path, job->source_path);
}

/**
 * NOTIFYING: removes the staging folder and, by default, the archive, then
 * tells the user.
 */
static bool step_notifying(const raiju_pipeline *pipeline, raiju_job *job) {
	char staging[RAIJU_JOB_PATH_MAX];
	if (raiju_job_dir(pipeline->staging_root, job, staging, sizeof staging) != 0 || remove_tree(staging) != 0) {
		return fail_with(job, "cleanup_failed", "staging");
	}
	if (should_delete_download(job) && remove_tree(job->source_path) != 0) {
		return fail_with(job, "cleanup_failed", "download");
	}
	return run_hook(job, pipeline->notify, pipeline->context, "notify_failed", RAIJU_STAGE_READY);
}

bool raiju_job_step(const raiju_pipeline *pipeline, raiju_job *job) {
	if (pipeline == NULL || job == NULL || !raiju_job_runnable(job)) {
		return false;
	}
	switch (job->stage) {
	case RAIJU_STAGE_VERIFYING:
		return step_verifying(job);
	case RAIJU_STAGE_CLASSIFYING:
		return step_classifying(job);
	case RAIJU_STAGE_EXTRACTING:
		return step_extracting(pipeline, job);
	case RAIJU_STAGE_INSTALLING:
		return step_installing(pipeline, job);
	case RAIJU_STAGE_RESCAN:
		return run_hook(job, pipeline->rescan, pipeline->context, "rescan_failed", RAIJU_STAGE_NOTIFYING);
	case RAIJU_STAGE_NOTIFYING:
		return step_notifying(pipeline, job);
	default:
		return false;
	}
}

int raiju_job_set_title_id(raiju_job *job, const char *title_id) {
	if (job == NULL || job->stage != RAIJU_STAGE_AWAITING_TITLE_ID || !raiju_title_id_valid(title_id)) {
		return -1;
	}
	memcpy(job->title_id, title_id, RAIJU_TITLE_ID_LEN + 1);
	job->title_source = RAIJU_TITLE_SOURCE_USER;
	job->names_disagree = false;
	job->install_path[0] = '\0';
	job->stage = RAIJU_STAGE_INSTALLING;
	return 0;
}

int raiju_job_resolve_conflict(const raiju_pipeline *pipeline, raiju_job *job, raiju_conflict_choice choice) {
	if (pipeline == NULL || job == NULL || job->stage != RAIJU_STAGE_CONFLICT) {
		return -1;
	}
	char staging[RAIJU_JOB_PATH_MAX];
	switch (choice) {
	case RAIJU_CONFLICT_RENAME:
		if (pick_free_install_path(job) != 0) {
			return -1;
		}
		break;
	case RAIJU_CONFLICT_REPLACE:
		break;
	case RAIJU_CONFLICT_CANCEL:
		if (raiju_job_dir(pipeline->staging_root, job, staging, sizeof staging) == 0) {
			remove_tree(staging);
		}
		job->conflict_choice = choice;
		job->stage = RAIJU_STAGE_CANCELLED;
		return 0;
	default:
		return -1;
	}
	job->conflict_choice = choice;
	job->stage = RAIJU_STAGE_INSTALLING;
	return 0;
}

int raiju_job_cancel(const raiju_pipeline *pipeline, raiju_job *job) {
	if (pipeline == NULL || job == NULL || job->stage == RAIJU_STAGE_READY || job->stage == RAIJU_STAGE_CANCELLED) {
		return -1;
	}
	char staging[RAIJU_JOB_PATH_MAX];
	if (raiju_job_dir(pipeline->staging_root, job, staging, sizeof staging) != 0 || remove_tree(staging) != 0) {
		return -1;
	}
	job->stage = RAIJU_STAGE_CANCELLED;
	return 0;
}

int raiju_job_retry(raiju_job *job) {
	if (job == NULL || job->stage != RAIJU_STAGE_FAILED) {
		return -1;
	}
	job->stage = job->failed_stage <= RAIJU_STAGE_VERIFYING ? RAIJU_STAGE_DOWNLOADING : job->failed_stage;
	job->error[0] = '\0';
	return 0;
}

int raiju_job_discard_files(const raiju_pipeline *pipeline, const raiju_job *job) {
	if (pipeline == NULL || job == NULL) {
		return -1;
	}
	int status = 0;
	char dir[RAIJU_JOB_PATH_MAX];
	if (raiju_job_dir(pipeline->staging_root, job, dir, sizeof dir) != 0 || remove_tree(dir) != 0) {
		status = -1;
	}
	if (raiju_job_dir(pipeline->trash_root, job, dir, sizeof dir) != 0 || remove_tree(dir) != 0) {
		status = -1;
	}
	bool installed = job->stage == RAIJU_STAGE_READY || job->stage == RAIJU_STAGE_RESCAN ||
	                 job->stage == RAIJU_STAGE_NOTIFYING ||
	                 (job->stage == RAIJU_STAGE_FAILED && job->failed_stage > RAIJU_STAGE_INSTALLING);
	if (!installed && job->source_path[0] != '\0' && !path_within(job->source_path, job->install_root) &&
	    remove_tree(job->source_path) != 0) {
		status = -1;
	}
	return status;
}
