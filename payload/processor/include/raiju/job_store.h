#ifndef RAIJU_JOB_STORE_H
#define RAIJU_JOB_STORE_H

#include <stddef.h>

#include "raiju/job.h"

#define RAIJU_JOBS_FILE_VERSION 1
#define RAIJU_JOBS_FILE_MAX_BYTES (4u * 1024u * 1024u)
#define RAIJU_JOBS_MAX 512

typedef enum {
	RAIJU_STORE_OK = 0,
	RAIJU_STORE_MISSING,
	RAIJU_STORE_IO_ERROR,
	RAIJU_STORE_TOO_LARGE,
	RAIJU_STORE_MALFORMED,
	RAIJU_STORE_INVALID_JOB,
	RAIJU_STORE_NO_MEMORY
} raiju_store_status;

/**
 * A growable array of jobs. Release with raiju_job_list_free.
 */
typedef struct {
	raiju_job *jobs;
	size_t count;
	size_t capacity;
} raiju_job_list;

/**
 * Appends a copy of job and returns a pointer to the stored copy, or NULL
 * when the list is full (RAIJU_JOBS_MAX) or out of memory. The pointer is
 * valid until the list changes size.
 */
raiju_job *raiju_job_list_add(raiju_job_list *list, const raiju_job *job);

/**
 * Returns the job with the given ID, or NULL.
 */
raiju_job *raiju_job_list_find(raiju_job_list *list, const char *id);

/**
 * Removes the job with the given ID. Returns false when there is none.
 */
bool raiju_job_list_remove(raiju_job_list *list, const char *id);

/**
 * Releases the list's storage and empties it.
 */
void raiju_job_list_free(raiju_job_list *list);

/**
 * Serialises a list as jobs.json into a newly allocated NUL-terminated
 * buffer the caller frees. Returns NULL when out of memory.
 */
char *raiju_jobs_to_json(const raiju_job_list *list, size_t *out_len);

/**
 * Parses jobs.json bytes into an empty list. The input is untrusted: the
 * whole document must be valid JSON, every job must carry valid IDs, paths,
 * names and enum values, IDs must be unique, and unknown keys are skipped.
 * On failure the list is left empty.
 */
raiju_store_status raiju_jobs_from_json(const char *json, size_t json_len, raiju_job_list *out);

/**
 * Writes the list to path atomically: the JSON goes to path.tmp, which is
 * fsynced and renamed over path, then the directory is fsynced. A crash
 * leaves either the old or the new file, never a mix.
 */
raiju_store_status raiju_jobs_save(const raiju_job_list *list, const char *path);

/**
 * Loads the list saved at path into an empty list. A missing file gives
 * RAIJU_STORE_MISSING and an empty list.
 */
raiju_store_status raiju_jobs_load(const char *path, raiju_job_list *out);

/**
 * Returns a stable lowercase name for a store status.
 */
const char *raiju_store_status_name(raiju_store_status status);

#endif
