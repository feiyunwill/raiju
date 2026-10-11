#ifndef RAIJU_PROCESSOR_H
#define RAIJU_PROCESSOR_H

#include <stdint.h>

#include "raiju/job_store.h"
#include "raiju/pipeline.h"

typedef enum {
	RAIJU_PROCESSOR_OK = 0,
	RAIJU_PROCESSOR_INVALID,
	RAIJU_PROCESSOR_NOT_FOUND,
	RAIJU_PROCESSOR_DUPLICATE,
	RAIJU_PROCESSOR_WRONG_STAGE,
	RAIJU_PROCESSOR_NO_SPACE,
	RAIJU_PROCESSOR_FULL,
	RAIJU_PROCESSOR_STORE_ERROR
} raiju_processor_status;

/**
 * The job queue: every job, the pipeline they run through and the jobs.json
 * they are persisted to after each change. Not thread safe; the payload's
 * worker thread owns it and serialises client requests onto it.
 */
typedef struct {
	raiju_job_list list;
	raiju_pipeline pipeline;
	char jobs_path[RAIJU_JOB_PATH_MAX];
	raiju_store_status load_status;
} raiju_processor;

/**
 * Loads jobs_path and prepares the queue. Each loaded job resumes from the
 * stage it was persisted in. A missing file starts an empty queue; an
 * unreadable or invalid one is moved aside to jobs_path.bad and the queue
 * starts empty, with the reason kept in load_status.
 */
raiju_processor_status raiju_processor_open(raiju_processor *processor, const raiju_pipeline *pipeline,
                                            const char *jobs_path);

/**
 * Releases the queue's memory. Nothing is written.
 */
void raiju_processor_close(raiju_processor *processor);

/**
 * Adds a download in the DOWNLOADING stage and persists it. When
 * expected_size is known the space check runs against the install root and
 * NO_SPACE refuses the job; space, when not NULL, receives the verdict.
 * category says whether uncompressed_size counts.
 */
raiju_processor_status raiju_processor_add(raiju_processor *processor, const raiju_job_options *options,
                                           raiju_category category, raiju_space_report *space);

/**
 * The download engine's completion event: the job's file (or folder) is at
 * path and the server announced expected_size bytes (0 when unknown, which
 * keeps the size given at add time). Moves the job to VERIFYING and
 * persists it; raiju_processor_run then takes it through the pipeline.
 */
raiju_processor_status raiju_processor_download_complete(raiju_processor *processor, const char *id,
                                                         const char *path, uint64_t expected_size);

/**
 * The download engine's failure event. Moves the job to FAILED.
 */
raiju_processor_status raiju_processor_download_failed(raiju_processor *processor, const char *id,
                                                       const char *error);

/**
 * Runs one stage of a job and persists the result.
 */
raiju_processor_status raiju_processor_step(raiju_processor *processor, const char *id);

/**
 * Runs every runnable job until it finishes or waits, persisting after each
 * stage.
 */
raiju_processor_status raiju_processor_run(raiju_processor *processor);

/**
 * Supplies the title ID for a job waiting for one, then persists.
 */
raiju_processor_status raiju_processor_set_title_id(raiju_processor *processor, const char *id,
                                                    const char *title_id);

/**
 * Answers a name conflict (rename, replace or cancel), then persists.
 */
raiju_processor_status raiju_processor_resolve(raiju_processor *processor, const char *id,
                                               raiju_conflict_choice choice);

/**
 * Sends a failed job back to its failed stage, then persists.
 */
raiju_processor_status raiju_processor_retry(raiju_processor *processor, const char *id);

/**
 * Cancels a job: its staging folder is removed, its download is kept until
 * the job is discarded, and nothing under an install root is touched. A job
 * that already reached READY or CANCELLED is refused with WRONG_STAGE.
 */
raiju_processor_status raiju_processor_cancel(raiju_processor *processor, const char *id);

/**
 * Deletes a job's working files (see raiju_job_discard_files) and removes it
 * from the queue, then persists.
 */
raiju_processor_status raiju_processor_discard(raiju_processor *processor, const char *id);

/**
 * Returns the job with the given ID, or NULL. The pointer is valid until the
 * queue changes size.
 */
raiju_job *raiju_processor_find(raiju_processor *processor, const char *id);

/**
 * Returns a stable lowercase name for a processor status.
 */
const char *raiju_processor_status_name(raiju_processor_status status);

#endif
