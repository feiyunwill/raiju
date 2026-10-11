#ifndef RAIJU_PIPELINE_H
#define RAIJU_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raiju/extract.h"
#include "raiju/job.h"
#include "raiju/space.h"

/**
 * Asks ShadowMountPlus (or a test fake) to rescan and confirm the installed
 * job. Returns 0 when the install is confirmed or the scanner is not
 * running; otherwise writes a reason into error and returns -1.
 */
typedef int (*raiju_rescan_fn)(const raiju_job *job, char *error, size_t error_size, void *context);

/**
 * Tells the user a job is ready. Returns 0 on success; otherwise writes a
 * reason into error and returns -1.
 */
typedef int (*raiju_notify_fn)(const raiju_job *job, char *error, size_t error_size, void *context);

/**
 * Receives extraction progress for a job. Returning PAUSE leaves the job in
 * EXTRACTING so it restarts later; CANCEL cancels the job.
 */
typedef raiju_extract_control (*raiju_job_progress_fn)(const raiju_job *job, uint64_t done, uint64_t total,
                                                        void *context);

/**
 * Fixed settings and hooks shared by every job. staging_root and trash_root
 * must be on the same filesystem as the install roots so the final moves are
 * atomic renames; each job uses <staging_root>/<id> and <trash_root>/<id>.
 * Missing roots are created (their parents must exist). Hooks may be NULL,
 * in which case the stage succeeds without doing anything. free_space
 * defaults to statvfs when NULL.
 */
typedef struct {
	const char *staging_root;
	const char *trash_root;
	raiju_rescan_fn rescan;
	raiju_notify_fn notify;
	raiju_job_progress_fn progress;
	raiju_free_space_fn free_space;
	void *context;
} raiju_pipeline;

/**
 * Runs the job's current stage once and moves it to the next stage, to
 * FAILED, or to a waiting stage (AWAITING_TITLE_ID, CONFLICT). Every stage
 * is safe to rerun after a crash: extraction clears staging first, and the
 * install step recognises a rename or trash swap that already happened.
 * INSTALLING takes two steps: the first commits the destination (or detects
 * a conflict), the second moves the payload. Returns true when the job
 * changed and should be persisted, false when it was not runnable or a
 * progress hook paused extraction.
 */
bool raiju_job_step(const raiju_pipeline *pipeline, raiju_job *job);

/**
 * Records a user-supplied title ID for a job in AWAITING_TITLE_ID and moves
 * it to INSTALLING. Returns -1 when the job is not waiting or the ID is not
 * well formed.
 */
int raiju_job_set_title_id(raiju_job *job, const char *title_id);

/**
 * Answers a name conflict for a job in CONFLICT. RENAME installs under the
 * first free <name>-<n> suffix, REPLACE moves the existing copy to the trash
 * folder, swaps the new one in and deletes the old one, CANCEL ends the job
 * without touching the install root (its staging folder is removed, the
 * download is kept). Returns -1 when the job is not in conflict or the
 * choice is ASK.
 */
int raiju_job_resolve_conflict(const raiju_pipeline *pipeline, raiju_job *job, raiju_conflict_choice choice);

/**
 * Sends a FAILED job back to the stage it failed in. A job that failed while
 * downloading or verifying goes back to DOWNLOADING, because its bytes on
 * disk have to be fetched again. Returns -1 when the job has not failed.
 */
int raiju_job_retry(raiju_job *job);

/**
 * Deletes a job's staging and trash folders and, for unfinished jobs, its
 * download. The install root is never touched. Returns 0 when everything
 * was removed.
 */
int raiju_job_discard_files(const raiju_pipeline *pipeline, const raiju_job *job);

/**
 * Writes <root>/<id> into out. Returns -1 when it does not fit.
 */
int raiju_job_dir(const char *root, const raiju_job *job, char *out, size_t out_size);

#endif
