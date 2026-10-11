#define _POSIX_C_SOURCE 200809L

#include "raiju/processor.h"

#include <stdio.h>
#include <string.h>

/**
 * Writes the queue to jobs.json.
 */
static raiju_processor_status persist(raiju_processor *processor) {
	return raiju_jobs_save(&processor->list, processor->jobs_path) == RAIJU_STORE_OK ? RAIJU_PROCESSOR_OK
	                                                                                 : RAIJU_PROCESSOR_STORE_ERROR;
}

raiju_processor_status raiju_processor_open(raiju_processor *processor, const raiju_pipeline *pipeline,
                                            const char *jobs_path) {
	if (processor == NULL || pipeline == NULL || jobs_path == NULL || pipeline->staging_root == NULL ||
	    pipeline->trash_root == NULL) {
		return RAIJU_PROCESSOR_INVALID;
	}
	memset(processor, 0, sizeof *processor);
	if (strlen(jobs_path) >= sizeof processor->jobs_path - 4) {
		return RAIJU_PROCESSOR_INVALID;
	}
	processor->pipeline = *pipeline;
	memcpy(processor->jobs_path, jobs_path, strlen(jobs_path) + 1);
	processor->load_status = raiju_jobs_load(jobs_path, &processor->list);
	if (processor->load_status != RAIJU_STORE_OK && processor->load_status != RAIJU_STORE_MISSING) {
		char aside[RAIJU_JOB_PATH_MAX];
		snprintf(aside, sizeof aside, "%s.bad", jobs_path);
		rename(jobs_path, aside);
	}
	return RAIJU_PROCESSOR_OK;
}

void raiju_processor_close(raiju_processor *processor) {
	if (processor != NULL) {
		raiju_job_list_free(&processor->list);
	}
}

raiju_processor_status raiju_processor_add(raiju_processor *processor, const raiju_job_options *options,
                                           raiju_category category, raiju_space_report *space) {
	raiju_job job;
	if (processor == NULL || raiju_job_init(&job, options) != 0) {
		return RAIJU_PROCESSOR_INVALID;
	}
	if (raiju_job_list_find(&processor->list, job.id) != NULL) {
		return RAIJU_PROCESSOR_DUPLICATE;
	}
	if (job.expected_size != 0) {
		raiju_space_need need = raiju_space_needed(category, job.expected_size, job.uncompressed_size);
		raiju_space_report report =
		    raiju_space_check(need, job.install_root, processor->pipeline.free_space, processor->pipeline.context);
		if (space != NULL) {
			*space = report;
		}
		if (report.status == RAIJU_SPACE_NOT_ENOUGH) {
			return RAIJU_PROCESSOR_NO_SPACE;
		}
	}
	if (raiju_job_list_add(&processor->list, &job) == NULL) {
		return RAIJU_PROCESSOR_FULL;
	}
	return persist(processor);
}

raiju_processor_status raiju_processor_download_complete(raiju_processor *processor, const char *id,
                                                         const char *path, uint64_t expected_size) {
	raiju_job *job = raiju_processor_find(processor, id);
	if (job == NULL) {
		return RAIJU_PROCESSOR_NOT_FOUND;
	}
	if (job->stage != RAIJU_STAGE_DOWNLOADING) {
		return RAIJU_PROCESSOR_WRONG_STAGE;
	}
	if (!raiju_job_path_valid(path)) {
		return RAIJU_PROCESSOR_INVALID;
	}
	const char *slash = strrchr(path, '/');
	const char *name = job->file_name[0] != '\0' ? job->file_name : slash + 1;
	if (!raiju_job_file_name_valid(name)) {
		return RAIJU_PROCESSOR_INVALID;
	}
	memmove(job->file_name, name, strlen(name) + 1);
	memcpy(job->source_path, path, strlen(path) + 1);
	if (expected_size != 0) {
		job->expected_size = expected_size;
	}
	job->stage = RAIJU_STAGE_VERIFYING;
	return persist(processor);
}

raiju_processor_status raiju_processor_download_failed(raiju_processor *processor, const char *id,
                                                       const char *error) {
	raiju_job *job = raiju_processor_find(processor, id);
	if (job == NULL) {
		return RAIJU_PROCESSOR_NOT_FOUND;
	}
	if (job->stage != RAIJU_STAGE_DOWNLOADING) {
		return RAIJU_PROCESSOR_WRONG_STAGE;
	}
	raiju_job_fail(job, error != NULL ? error : "download_failed");
	return persist(processor);
}

raiju_processor_status raiju_processor_step(raiju_processor *processor, const char *id) {
	raiju_job *job = raiju_processor_find(processor, id);
	if (job == NULL) {
		return RAIJU_PROCESSOR_NOT_FOUND;
	}
	if (!raiju_job_runnable(job)) {
		return RAIJU_PROCESSOR_WRONG_STAGE;
	}
	return raiju_job_step(&processor->pipeline, job) ? persist(processor) : RAIJU_PROCESSOR_OK;
}

raiju_processor_status raiju_processor_run(raiju_processor *processor) {
	if (processor == NULL) {
		return RAIJU_PROCESSOR_INVALID;
	}
	for (size_t i = 0; i < processor->list.count; i++) {
		raiju_job *job = &processor->list.jobs[i];
		while (raiju_job_runnable(job) && raiju_job_step(&processor->pipeline, job)) {
			raiju_processor_status status = persist(processor);
			if (status != RAIJU_PROCESSOR_OK) {
				return status;
			}
		}
	}
	return RAIJU_PROCESSOR_OK;
}

raiju_processor_status raiju_processor_set_title_id(raiju_processor *processor, const char *id,
                                                    const char *title_id) {
	raiju_job *job = raiju_processor_find(processor, id);
	if (job == NULL) {
		return RAIJU_PROCESSOR_NOT_FOUND;
	}
	if (job->stage != RAIJU_STAGE_AWAITING_TITLE_ID) {
		return RAIJU_PROCESSOR_WRONG_STAGE;
	}
	if (raiju_job_set_title_id(job, title_id) != 0) {
		return RAIJU_PROCESSOR_INVALID;
	}
	return persist(processor);
}

raiju_processor_status raiju_processor_resolve(raiju_processor *processor, const char *id,
                                               raiju_conflict_choice choice) {
	raiju_job *job = raiju_processor_find(processor, id);
	if (job == NULL) {
		return RAIJU_PROCESSOR_NOT_FOUND;
	}
	if (job->stage != RAIJU_STAGE_CONFLICT) {
		return RAIJU_PROCESSOR_WRONG_STAGE;
	}
	if (raiju_job_resolve_conflict(&processor->pipeline, job, choice) != 0) {
		return RAIJU_PROCESSOR_INVALID;
	}
	return persist(processor);
}

raiju_processor_status raiju_processor_retry(raiju_processor *processor, const char *id) {
	raiju_job *job = raiju_processor_find(processor, id);
	if (job == NULL) {
		return RAIJU_PROCESSOR_NOT_FOUND;
	}
	if (raiju_job_retry(job) != 0) {
		return RAIJU_PROCESSOR_WRONG_STAGE;
	}
	return persist(processor);
}

raiju_processor_status raiju_processor_discard(raiju_processor *processor, const char *id) {
	raiju_job *job = raiju_processor_find(processor, id);
	if (job == NULL) {
		return RAIJU_PROCESSOR_NOT_FOUND;
	}
	raiju_job_discard_files(&processor->pipeline, job);
	raiju_job_list_remove(&processor->list, id);
	return persist(processor);
}

raiju_job *raiju_processor_find(raiju_processor *processor, const char *id) {
	return processor != NULL ? raiju_job_list_find(&processor->list, id) : NULL;
}

const char *raiju_processor_status_name(raiju_processor_status status) {
	switch (status) {
	case RAIJU_PROCESSOR_OK:
		return "ok";
	case RAIJU_PROCESSOR_INVALID:
		return "invalid";
	case RAIJU_PROCESSOR_NOT_FOUND:
		return "not_found";
	case RAIJU_PROCESSOR_DUPLICATE:
		return "duplicate";
	case RAIJU_PROCESSOR_WRONG_STAGE:
		return "wrong_stage";
	case RAIJU_PROCESSOR_NO_SPACE:
		return "no_space";
	case RAIJU_PROCESSOR_FULL:
		return "full";
	case RAIJU_PROCESSOR_STORE_ERROR:
		return "store_error";
	}
	return "unknown";
}
