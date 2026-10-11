#ifndef RAIJU_JOB_H
#define RAIJU_JOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raiju/classify.h"
#include "raiju/title_id.h"

#define RAIJU_JOB_ID_MAX 64
#define RAIJU_JOB_PATH_MAX 1024
#define RAIJU_JOB_NAME_MAX 256
#define RAIJU_JOB_ERROR_MAX 256

typedef enum {
	RAIJU_STAGE_DOWNLOADING = 0,
	RAIJU_STAGE_VERIFYING,
	RAIJU_STAGE_CLASSIFYING,
	RAIJU_STAGE_EXTRACTING,
	RAIJU_STAGE_INSTALLING,
	RAIJU_STAGE_RESCAN,
	RAIJU_STAGE_NOTIFYING,
	RAIJU_STAGE_READY,
	RAIJU_STAGE_AWAITING_TITLE_ID,
	RAIJU_STAGE_CONFLICT,
	RAIJU_STAGE_FAILED,
	RAIJU_STAGE_CANCELLED
} raiju_stage;

typedef enum {
	RAIJU_PAYLOAD_NONE = 0,
	RAIJU_PAYLOAD_IMAGE,
	RAIJU_PAYLOAD_FOLDER,
	RAIJU_PAYLOAD_FILE
} raiju_payload_type;

typedef enum {
	RAIJU_CONFLICT_ASK = 0,
	RAIJU_CONFLICT_RENAME,
	RAIJU_CONFLICT_REPLACE,
	RAIJU_CONFLICT_CANCEL
} raiju_conflict_choice;

/**
 * One download and its post-download pipeline, as persisted in jobs.json.
 *
 * stage is the next stage to run; a stage only advances once its work is
 * done, so after a restart the job reruns the stage it was in. failed_stage
 * records where a FAILED job stopped. source_path is the finished download
 * (a file, or a directory for multi-file downloads) and file_name the
 * original name used for the title ID fallback. payload_path is what gets
 * installed: the download itself, or the dump folder or single image inside
 * staging. install_path is set once the destination is committed, which is
 * also when a conflict is detected; conflict_choice records how a conflict
 * was or will be resolved.
 */
typedef struct {
	char id[RAIJU_JOB_ID_MAX + 1];
	raiju_stage stage;
	raiju_stage failed_stage;
	char error[RAIJU_JOB_ERROR_MAX];
	char source_path[RAIJU_JOB_PATH_MAX];
	char file_name[RAIJU_JOB_NAME_MAX];
	uint64_t expected_size;
	uint64_t uncompressed_size;
	char install_root[RAIJU_JOB_PATH_MAX];
	bool keep_archive;
	raiju_conflict_choice conflict_policy;
	raiju_kind kind;
	raiju_payload_type payload_type;
	char payload_path[RAIJU_JOB_PATH_MAX];
	char title_id[RAIJU_TITLE_ID_LEN + 1];
	char display_name[RAIJU_JOB_NAME_MAX];
	raiju_title_source title_source;
	bool names_disagree;
	char install_path[RAIJU_JOB_PATH_MAX];
	raiju_conflict_choice conflict_choice;
	uint64_t progress_done;
	uint64_t progress_total;
} raiju_job;

/**
 * What a client asks for when it adds a download. file_name may be empty
 * until the download completes. uncompressed_size is 0 when unknown.
 * title_id pins the install name from the start (NULL or empty to let the
 * pipeline read it from param.json or the file name); when set it survives
 * classification.
 */
typedef struct {
	const char *id;
	const char *file_name;
	const char *install_root;
	uint64_t expected_size;
	uint64_t uncompressed_size;
	bool keep_archive;
	raiju_conflict_choice conflict_policy;
	const char *title_id;
} raiju_job_options;

/**
 * Returns whether text is usable as a job ID: 1 to RAIJU_JOB_ID_MAX
 * characters from [A-Za-z0-9_-]. Job IDs name staging and trash folders, so
 * nothing else is allowed.
 */
bool raiju_job_id_valid(const char *text);

/**
 * Returns whether text is an absolute path shorter than RAIJU_JOB_PATH_MAX
 * with no empty, '.' or '..' components.
 */
bool raiju_job_path_valid(const char *text);

/**
 * Returns whether text is a single file name: non-empty, shorter than
 * RAIJU_JOB_NAME_MAX, not '.' or '..', and without '/' or control
 * characters.
 */
bool raiju_job_file_name_valid(const char *text);

/**
 * Initialises a job in the DOWNLOADING stage from client options. Returns 0
 * on success and -1 when an option is invalid.
 */
int raiju_job_init(raiju_job *job, const raiju_job_options *options);

/**
 * Returns whether the processor can run the job's current stage without
 * waiting for a download, a user answer or a retry.
 */
bool raiju_job_runnable(const raiju_job *job);

/**
 * Returns whether the job has reached READY, FAILED or CANCELLED.
 */
bool raiju_job_finished(const raiju_job *job);

/**
 * Moves a job to FAILED, remembering the stage it failed in and a short
 * error text (truncated to fit).
 */
void raiju_job_fail(raiju_job *job, const char *error);

/**
 * Returns a stable lowercase name for a stage, as stored in jobs.json.
 */
const char *raiju_stage_name(raiju_stage stage);

/**
 * Parses a stage name written by raiju_stage_name. Returns false for an
 * unknown name.
 */
bool raiju_stage_from_name(const char *name, raiju_stage *out);

/**
 * Returns a stable lowercase name for a payload type.
 */
const char *raiju_payload_type_name(raiju_payload_type type);

/**
 * Parses a payload type name. Returns false for an unknown name.
 */
bool raiju_payload_type_from_name(const char *name, raiju_payload_type *out);

/**
 * Returns a stable lowercase name for a conflict choice.
 */
const char *raiju_conflict_choice_name(raiju_conflict_choice choice);

/**
 * Parses a conflict choice name. Returns false for an unknown name.
 */
bool raiju_conflict_choice_from_name(const char *name, raiju_conflict_choice *out);

/**
 * Parses a kind name written by raiju_kind_name. Returns false for an
 * unknown name.
 */
bool raiju_kind_from_name(const char *name, raiju_kind *out);

/**
 * Returns a stable lowercase name for a title source.
 */
const char *raiju_title_source_name(raiju_title_source source);

/**
 * Parses a title source name. Returns false for an unknown name.
 */
bool raiju_title_source_from_name(const char *name, raiju_title_source *out);

#endif
