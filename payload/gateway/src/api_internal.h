#ifndef RAIJU_API_INTERNAL_H
#define RAIJU_API_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raiju/fetch.h"
#include "raiju/gateway.h"
#include "raiju/http.h"
#include "raiju/processor.h"
#include "raiju/strbuf.h"

#define RAIJU_API_MAX_CANCELS 64
#define RAIJU_API_MAX_EVENTS 64
#define RAIJU_API_TICK_MS 400
#define RAIJU_STORAGE_TTL_SECONDS 30

/**
 * One engine event waiting for the worker thread: the aria2 GID and, for a
 * failure, the message. Completion events carry no path; the worker asks
 * aria2 for it when it runs.
 */
typedef struct {
	char gid[RAIJU_JOB_ID_MAX + 1];
	bool failed;
	char message[RAIJU_JOB_ERROR_MAX];
} raiju_api_event;

/**
 * The /api state: the gateway it belongs to, the processor queue, and the
 * single worker thread that owns it. queue_lock serialises every call that
 * touches the queue (handlers and the worker). event_lock guards the pending
 * engine events and wake_lock the worker's sleep, so the websocket thread
 * that receives aria2 notifications never waits for a stage that is running.
 * cancel_lock guards the set of cancel-requested job IDs the progress hook
 * consults during extraction, so a cancel can stop a running job without
 * waiting on queue_lock; an empty slot is free to reuse. storage_lock guards
 * the games and staging byte counts, which are walked at most every
 * RAIJU_STORAGE_TTL_SECONDS so /api/storage stays cheap.
 */
typedef struct raiju_api {
	raiju_gateway *gateway;
	raiju_processor processor;
	bool worker_started;
	pthread_t worker;
	pthread_mutex_t queue_lock;
	pthread_mutex_t event_lock;
	pthread_mutex_t wake_lock;
	pthread_cond_t wake_signal;
	pthread_mutex_t cancel_lock;
	char cancel_ids[RAIJU_API_MAX_CANCELS][RAIJU_JOB_ID_MAX + 1];
	pthread_mutex_t storage_lock;
	uint64_t storage_at;
	uint64_t storage_games;
	uint64_t storage_staging;
	raiju_api_event events[RAIJU_API_MAX_EVENTS];
	size_t event_count;
	bool stopping;
	char jobs_path[RAIJU_JOB_PATH_MAX];
	char staging_root[RAIJU_PATH_MAX];
	char trash_root[RAIJU_PATH_MAX];
} raiju_api;

/**
 * The outbound fetch implementation and its context from the gateway config.
 */
raiju_fetch_fn raiju_api_fetch_fn(raiju_gateway *gateway);
void *raiju_api_fetch_context(raiju_gateway *gateway);

/**
 * Serialises one job, and all jobs, in the API shape described in
 * ARCHITECTURE.md: the raw processor stage, the UI-vocabulary stage and the
 * UI status.
 */
void raiju_api_job_json(raiju_strbuf *out, const raiju_job *job);
void raiju_api_jobs_json(raiju_api *api, raiju_strbuf *out);

/**
 * Appends the allowlist a client path is checked against.
 */
const raiju_allowlist *raiju_api_allowlist(raiju_gateway *gateway);

bool raiju_api_add_core_routes(raiju_gateway *gateway, raiju_api *api);
bool raiju_api_add_job_routes(raiju_gateway *gateway, raiju_api *api);
bool raiju_api_add_probe_route(raiju_gateway *gateway, raiju_api *api);

/**
 * Extracts a top-level JSON string value from a request body.
 */
bool raiju_api_json_string(const char *body, size_t length, const char *key, char *out, size_t capacity);

#endif
