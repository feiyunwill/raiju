#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/api.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "api_internal.h"
#include "raiju/art.h"
#include "raiju/auth.h"
#include "raiju/fsutil.h"
#include "raiju/proxy.h"

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "0.0.0"
#endif

#define RAIJU_SMP_SCAN_URL "http://127.0.0.1:10101/api/v1/scan"
#define RAIJU_STORAGE_MAX_DEPTH 64

typedef struct {
	const char *id;
	const char *label;
	const char *path;
} raiju_destination;

static const raiju_destination api_destinations[] = {
    {"homebrew", "homebrew", "/data/homebrew"},
    {"etahen", "etaHEN/games", "/data/etaHEN/games"},
    {"downloads", "Downloads", "/data/raiju/downloads"},
};

static const char *find_needle(const char *start, const char *limit, const char *needle, size_t needle_length) {
	for (const char *cursor = start; cursor + needle_length <= limit; cursor++) {
		if (memcmp(cursor, needle, needle_length) == 0) {
			return cursor;
		}
	}
	return NULL;
}

static const char *json_value(const char *body, size_t length, const char *key) {
	char needle[96];
	int written = snprintf(needle, sizeof needle, "\"%s\"", key);
	if (written < 0 || (size_t)written >= sizeof needle) {
		return NULL;
	}
	const char *cursor = body;
	const char *limit = body + length;
	size_t needle_length = strlen(needle);
	const char *found;
	while (cursor < limit && (found = find_needle(cursor, limit, needle, needle_length)) != NULL) {
		const char *after = found + needle_length;
		while (after < limit && (*after == ' ' || *after == ':')) {
			after++;
		}
		if (after < limit && (*after == '"' || *after == '{' || *after == '[' || (*after >= '0' && *after <= '9') ||
		                  strncmp(after, "true", 4) == 0 || strncmp(after, "false", 5) == 0)) {
			return after;
		}
		cursor = after;
	}
	return NULL;
}

static bool json_string(const char *body, size_t length, const char *key, char *out, size_t capacity) {
	const char *value = json_value(body, length, key);
	if (value == NULL || *value != '"') {
		return false;
	}
	value++;
	const char *limit = body + length;
	const char *end = value;
	while (end < limit && *end != '"') {
		if (*end == '\\' && end + 1 < limit) {
			end++;
		}
		end++;
	}
	if (end >= limit) {
		return false;
	}
	size_t written = 0;
	for (const char *cursor = value; cursor < end && written + 1 < capacity; cursor++) {
		if (*cursor != '\\' || cursor + 1 >= end) {
			out[written++] = *cursor;
			continue;
		}
		cursor++;
		switch (*cursor) {
		case 'n':
			out[written++] = '\n';
			break;
		case 't':
			out[written++] = '\t';
			break;
		case 'r':
			out[written++] = '\r';
			break;
		case 'b':
			out[written++] = '\b';
			break;
		case 'f':
			out[written++] = '\f';
			break;
		case 'u': {
			unsigned codepoint = 0;
			int digits = 0;
			while (digits < 4 && cursor + 1 < end) {
				char hex = cursor[1];
				int value_hex = hex >= '0' && hex <= '9'   ? hex - '0'
				                : hex >= 'a' && hex <= 'f' ? hex - 'a' + 10
				                : hex >= 'A' && hex <= 'F' ? hex - 'A' + 10
				                                           : -1;
				if (value_hex < 0) {
					break;
				}
				codepoint = codepoint * 16 + (unsigned)value_hex;
				cursor++;
				digits++;
			}
			if (digits == 4 && written + 4 < capacity) {
				if (codepoint < 0x80) {
					out[written++] = (char)codepoint;
				} else if (codepoint < 0x800) {
					out[written++] = (char)(0xC0 | (codepoint >> 6));
					out[written++] = (char)(0x80 | (codepoint & 0x3F));
				} else {
					out[written++] = (char)(0xE0 | (codepoint >> 12));
					out[written++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
					out[written++] = (char)(0x80 | (codepoint & 0x3F));
				}
			}
			break;
		}
		default:
			out[written++] = *cursor;
			break;
		}
	}
	out[written] = '\0';
	return true;
}

bool raiju_api_json_string(const char *body, size_t length, const char *key, char *out, size_t capacity) {
	return json_string(body, length, key, out, capacity);
}

static bool json_uint(const char *body, size_t length, const char *key, uint64_t *out) {
	const char *value = json_value(body, length, key);
	if (value == NULL) {
		return false;
	}
	char *end = NULL;
	unsigned long long parsed = strtoull(value, &end, 10);
	if (end == value) {
		return false;
	}
	*out = (uint64_t)parsed;
	return true;
}

static bool json_bool(const char *body, size_t length, const char *key, bool *out) {
	const char *value = json_value(body, length, key);
	if (value == NULL) {
		return false;
	}
	if (strncmp(value, "true", 4) == 0) {
		*out = true;
		return true;
	}
	if (strncmp(value, "false", 5) == 0) {
		*out = false;
		return true;
	}
	return false;
}

static const char *ui_stage_name(raiju_stage stage) {
	switch (stage) {
	case RAIJU_STAGE_DOWNLOADING:
		return "download";
	case RAIJU_STAGE_VERIFYING:
	case RAIJU_STAGE_CLASSIFYING:
	case RAIJU_STAGE_AWAITING_TITLE_ID:
		return "verify";
	case RAIJU_STAGE_EXTRACTING:
		return "extract";
	case RAIJU_STAGE_INSTALLING:
	case RAIJU_STAGE_RESCAN:
	case RAIJU_STAGE_NOTIFYING:
	case RAIJU_STAGE_CONFLICT:
		return "install";
	default:
		return "ready";
	}
}

static const char *ui_status_name(raiju_stage stage) {
	switch (stage) {
	case RAIJU_STAGE_DOWNLOADING:
	case RAIJU_STAGE_VERIFYING:
	case RAIJU_STAGE_CLASSIFYING:
	case RAIJU_STAGE_EXTRACTING:
	case RAIJU_STAGE_INSTALLING:
	case RAIJU_STAGE_RESCAN:
	case RAIJU_STAGE_NOTIFYING:
		return "active";
	case RAIJU_STAGE_AWAITING_TITLE_ID:
	case RAIJU_STAGE_CONFLICT:
		return "waiting";
	case RAIJU_STAGE_READY:
		return "done";
	default:
		return "failed";
	}
}

static const char *install_name_of(const raiju_job *job) {
	if (job->install_path[0] != '\0') {
		const char *slash = strrchr(job->install_path, '/');
		return slash != NULL ? slash + 1 : job->install_path;
	}
	return job->title_id;
}

/**
 * Milliseconds on the monotonic clock, for the snapshot publish throttle.
 */
static uint64_t monotonic_ms(void) {
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0;
	}
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

/**
 * Applies the one stage/status/install-name mapping to a job, filling the
 * snapshot record every read serves.
 */
/**
 * The total of the stage running now: the size a download expects, or the archive's
 * uncompressed size while extracting. 0 when that size is unknown, which is what a
 * tar.* download reports until it has been read, so a client falls back to bytes moved.
 */
static uint64_t stage_total_of(const raiju_job *job) {
	if (job->stage == RAIJU_STAGE_DOWNLOADING) {
		return job->expected_size;
	}
	if (job->stage == RAIJU_STAGE_EXTRACTING) {
		return job->uncompressed_size;
	}
	return 0;
}

static void fill_view(raiju_api_job_view *view, const raiju_job *job) {
	raiju_stage bucket = job->stage == RAIJU_STAGE_FAILED ? job->failed_stage : job->stage;
	snprintf(view->id, sizeof view->id, "%s", job->id);
	snprintf(view->stage, sizeof view->stage, "%s", raiju_stage_name(job->stage));
	snprintf(view->ui_stage, sizeof view->ui_stage, "%s", ui_stage_name(bucket));
	snprintf(view->status, sizeof view->status, "%s", ui_status_name(job->stage));
	snprintf(view->title_id, sizeof view->title_id, "%s", job->title_id);
	snprintf(view->source_name, sizeof view->source_name, "%s", job->file_name);
	snprintf(view->display_name, sizeof view->display_name, "%s", job->display_name);
	snprintf(view->install_name, sizeof view->install_name, "%s", install_name_of(job));
	snprintf(view->kind, sizeof view->kind, "%s", raiju_kind_name(job->kind));
	snprintf(view->error, sizeof view->error, "%s", job->error);
	view->bytes_total = job->expected_size;
	view->bytes_stage_total = stage_total_of(job);
	view->bytes_done = job->progress_done;
}

/**
 * Serialises a snapshot record in the API shape described in ARCHITECTURE.md.
 */
static void raiju_api_job_view_json(raiju_strbuf *out, const raiju_api_job_view *view) {
	raiju_strbuf_append(out, "{\"id\":");
	raiju_strbuf_append_json_string(out, view->id);
	raiju_strbuf_append(out, ",\"stage\":");
	raiju_strbuf_append_json_string(out, view->stage);
	raiju_strbuf_append(out, ",\"uiStage\":");
	raiju_strbuf_append_json_string(out, view->ui_stage);
	raiju_strbuf_append(out, ",\"status\":");
	raiju_strbuf_append_json_string(out, view->status);
	raiju_strbuf_append(out, ",\"titleId\":");
	raiju_strbuf_append_json_string(out, view->title_id);
	raiju_strbuf_append(out, ",\"sourceName\":");
	raiju_strbuf_append_json_string(out, view->source_name);
	raiju_strbuf_append(out, ",\"name\":");
	raiju_strbuf_append_json_string(out, view->source_name);
	raiju_strbuf_append(out, ",\"displayName\":");
	raiju_strbuf_append_json_string(out, view->display_name);
	raiju_strbuf_append(out, ",\"kind\":");
	raiju_strbuf_append_json_string(out, view->kind);
	raiju_strbuf_append(out, ",\"installName\":");
	raiju_strbuf_append_json_string(out, view->install_name);
	raiju_strbuf_append(out, ",\"bytesTotal\":");
	raiju_strbuf_append_uint(out, (unsigned long long)view->bytes_total);
	raiju_strbuf_append(out, ",\"bytesStageTotal\":");
	raiju_strbuf_append_uint(out, (unsigned long long)view->bytes_stage_total);
	raiju_strbuf_append(out, ",\"bytesDone\":");
	raiju_strbuf_append_uint(out, (unsigned long long)view->bytes_done);
	raiju_strbuf_append(out, ",\"error\":");
	raiju_strbuf_append_json_string(out, view->error);
	raiju_strbuf_append_char(out, '}');
}

/**
 * A cheap stamp of what the snapshot shows, so the queue is only copied into
 * it when a stage or a progress figure actually moved.
 */
static uint64_t queue_stamp(const raiju_api *api) {
	uint64_t stamp = 1469598103934665603ull ^ (uint64_t)api->processor.list.count;
	for (size_t i = 0; i < api->processor.list.count; i++) {
		const raiju_job *job = &api->processor.list.jobs[i];
		stamp = (stamp ^ (uint64_t)job->stage) * 1099511628211ull;
		stamp = (stamp ^ job->progress_done) * 1099511628211ull;
	}
	return stamp;
}

/**
 * Copies the queue into the published snapshot under view_lock, once its
 * visible state changed and at most every RAIJU_API_VIEW_PUBLISH_MS unless
 * force is set. Callers hold queue_lock, so the lock order is queue_lock then
 * view_lock.
 */
static void publish_views(raiju_api *api, bool force) {
	uint64_t now = monotonic_ms();
	uint64_t stamp = queue_stamp(api);
	pthread_mutex_lock(&api->view_lock);
	if (!force && (stamp == api->view_stamp || now - api->view_published_ms < RAIJU_API_VIEW_PUBLISH_MS)) {
		pthread_mutex_unlock(&api->view_lock);
		return;
	}
	size_t count = api->processor.list.count;
	if (count > RAIJU_JOBS_MAX) {
		count = RAIJU_JOBS_MAX;
	}
	for (size_t i = 0; i < count; i++) {
		fill_view(&api->views[i], &api->processor.list.jobs[i]);
	}
	api->view_count = count;
	api->view_published_ms = now;
	api->view_stamp = stamp;
	pthread_mutex_unlock(&api->view_lock);
}

void raiju_api_job_json(raiju_strbuf *out, const raiju_job *job) {
	raiju_api_job_view view;
	fill_view(&view, job);
	raiju_api_job_view_json(out, &view);
}

void raiju_api_jobs_json(raiju_api *api, raiju_strbuf *out) {
	raiju_strbuf_append(out, "{\"jobs\":[");
	pthread_mutex_lock(&api->view_lock);
	for (size_t i = 0; i < api->view_count; i++) {
		if (i > 0) {
			raiju_strbuf_append_char(out, ',');
		}
		raiju_api_job_view_json(out, &api->views[i]);
	}
	pthread_mutex_unlock(&api->view_lock);
	raiju_strbuf_append(out, "]}");
}

raiju_fetch_fn raiju_api_fetch_fn(raiju_gateway *gateway) {
	return gateway->config.fetch != NULL ? gateway->config.fetch : raiju_fetch_default();
}

void *raiju_api_fetch_context(raiju_gateway *gateway) {
	return gateway->config.fetch_context;
}

const raiju_allowlist *raiju_api_allowlist(raiju_gateway *gateway) {
	return &gateway->allowlist;
}

static bool root_covers_path(const char *root, const char *path) {
	size_t length = strlen(root);
	while (length > 1 && root[length - 1] == '/') {
		length--;
	}
	return strncmp(path, root, length) == 0 && (path[length] == '\0' || path[length] == '/');
}

static bool destination_allowed(raiju_gateway *gateway, const char *path) {
	if (!raiju_job_path_valid(path)) {
		return false;
	}
	for (size_t i = 0; i < gateway->allowlist.count; i++) {
		if (root_covers_path(gateway->allowlist.roots[i], path)) {
			return true;
		}
	}
	return false;
}

/**
 * Sums the sizes of regular files under path without following symlinks.
 */
static uint64_t directory_bytes(const char *path, unsigned depth) {
	if (depth > RAIJU_STORAGE_MAX_DEPTH) {
		return 0;
	}
	struct stat info;
	if (lstat(path, &info) != 0) {
		return 0;
	}
	if (S_ISREG(info.st_mode)) {
		return (uint64_t)info.st_size;
	}
	if (!S_ISDIR(info.st_mode)) {
		return 0;
	}
	DIR *directory = opendir(path);
	if (directory == NULL) {
		return 0;
	}
	uint64_t total = 0;
	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		char child[RAIJU_PATH_MAX];
		int written = snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
		if (written < 0 || (size_t)written >= sizeof child) {
			continue;
		}
		total += directory_bytes(child, depth + 1);
	}
	closedir(directory);
	return total;
}

static bool engine_json(raiju_gateway *gateway, const char *body, size_t length, char *out, size_t capacity,
                        int *out_status) {
	size_t injected_length = 0;
	char *injected = raiju_proxy_inject_secret(body, length, gateway->secret, &injected_length);
	if (injected == NULL) {
		return false;
	}
	raiju_upstream_response response;
	int result =
	    raiju_upstream_post(&gateway->upstream, (const unsigned char *)injected, injected_length, 10000, &response);
	free(injected);
	if (result != 0) {
		return false;
	}
	if (out_status != NULL) {
		*out_status = response.status;
	}
	if (out != NULL) {
		snprintf(out, capacity, "%s", raiju_strbuf_cstr(&response.body));
	}
	raiju_upstream_response_free(&response);
	return true;
}

static bool engine_health(raiju_gateway *gateway, char *version, size_t capacity) {
	char body[256];
	snprintf(body, sizeof body, "{\"jsonrpc\":\"2.0\",\"id\":\"raiju\",\"method\":\"aria2.getVersion\"}");
	char response[2048];
	int status = 0;
	if (!engine_json(gateway, body, strlen(body), response, sizeof response, &status) || status != 200) {
		return false;
	}
	char raw_version[64];
	if (!json_string(response, strlen(response), "version", raw_version, sizeof raw_version)) {
		return false;
	}
	snprintf(version, capacity, "%s", raw_version);
	return true;
}

static bool engine_status_path(raiju_gateway *gateway, const char *gid, char *path, size_t capacity) {
	char body[512];
	snprintf(body, sizeof body,
	         "{\"jsonrpc\":\"2.0\",\"id\":\"raiju\",\"method\":\"aria2.tellStatus\","
	         "\"params\":[\"%s\",[\"files\"]]}",
	         gid);
	char response[8192];
	int status = 0;
	if (!engine_json(gateway, body, strlen(body), response, sizeof response, &status) || status != 200) {
		return false;
	}
	return json_string(response, strlen(response), "path", path, capacity);
}

static bool engine_download_state(raiju_gateway *gateway, const char *gid, char *status, size_t status_capacity) {
	char body[512];
	snprintf(body, sizeof body,
	         "{\"jsonrpc\":\"2.0\",\"id\":\"raiju\",\"method\":\"aria2.tellStatus\","
	         "\"params\":[\"%s\",[\"status\"]]}",
	         gid);
	char response[4096];
	int code = 0;
	if (!engine_json(gateway, body, strlen(body), response, sizeof response, &code) || code != 200) {
		return false;
	}
	return json_string(response, strlen(response), "status", status, status_capacity);
}

static bool engine_add_uri(raiju_gateway *gateway, const char *gid, const char *uri, const char *dir) {
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	raiju_strbuf_append(&document, "{\"jsonrpc\":\"2.0\",\"id\":\"raiju\",\"method\":\"aria2.addUri\",\"params\":[[");
	raiju_strbuf_append_json_string(&document, uri);
	raiju_strbuf_append(&document, "],{\"gid\":");
	raiju_strbuf_append_json_string(&document, gid);
	raiju_strbuf_append(&document, ",\"dir\":");
	raiju_strbuf_append_json_string(&document, dir);
	raiju_strbuf_append(&document, "}]}");
	bool ok = false;
	if (!document.failed) {
		char response[2048];
		int status = 0;
		if (engine_json(gateway, raiju_strbuf_cstr(&document), document.length, response, sizeof response, &status) &&
		    status == 200 && strstr(response, "\"result\"") != NULL) {
			ok = true;
		}
	}
	raiju_strbuf_free(&document);
	return ok;
}

static int api_rescan(const raiju_job *job, char *error, size_t error_size, void *context) {
	(void)job;
	(void)error;
	(void)error_size;
	(void)context;
	raiju_upstream endpoint;
	if (!raiju_upstream_parse(RAIJU_SMP_SCAN_URL, &endpoint)) {
		return 0;
	}
	raiju_upstream_response response;
	if (raiju_upstream_post(&endpoint, (const unsigned char *)"{}", 2, 5000, &response) != 0) {
		return 0;
	}
	int status = response.status;
	raiju_upstream_response_free(&response);
	return status == 200 ? 0 : -1;
}

static int api_notify(const raiju_job *job, char *error, size_t error_size, void *context) {
	(void)error;
	(void)error_size;
	raiju_api *api = context;
	const raiju_platform *platform = api->gateway->config.platform;
	char message[RAIJU_JOB_NAME_MAX + 16];
	snprintf(message, sizeof message, "Raiju: %s ready", job->title_id[0] != '\0' ? job->title_id : job->file_name);
	platform->notify(platform->ctx, message);
	return 0;
}

static int api_free_space(const char *path, uint64_t *free_bytes, void *context) {
	raiju_api *api = context;
	const raiju_platform *platform = api->gateway->config.platform;
	raiju_disk_usage usage;
	if (platform->disk_usage(platform->ctx, path, &usage) != 0) {
		return -1;
	}
	*free_bytes = usage.free_bytes;
	return 0;
}

static raiju_extract_control api_progress(const raiju_job *job, uint64_t done, uint64_t total, void *context) {
	(void)done;
	(void)total;
	raiju_api *api = context;
	raiju_extract_control control = RAIJU_EXTRACT_CONTINUE;
	pthread_mutex_lock(&api->cancel_lock);
	for (size_t i = 0; i < RAIJU_API_MAX_CANCELS; i++) {
		if (api->cancel_ids[i][0] != '\0' && strcmp(api->cancel_ids[i], job->id) == 0) {
			control = RAIJU_EXTRACT_CANCEL;
			break;
		}
	}
	pthread_mutex_unlock(&api->cancel_lock);
	publish_views(api, false);
	return control;
}

/**
 * Takes the oldest pending engine event, if any, so handling one (which asks
 * aria2 for the download path) never happens while the event queue is locked.
 */
static bool take_event(raiju_api *api, raiju_api_event *out) {
	bool taken = false;
	pthread_mutex_lock(&api->event_lock);
	if (api->event_count > 0) {
		*out = api->events[0];
		for (size_t i = 1; i < api->event_count; i++) {
			api->events[i - 1] = api->events[i];
		}
		api->event_count--;
		taken = true;
	}
	pthread_mutex_unlock(&api->event_lock);
	return taken;
}

static bool apply_events(raiju_api *api) {
	bool changed = false;
	raiju_api_event event;
	while (take_event(api, &event)) {
		if (event.failed) {
			if (raiju_processor_download_failed(&api->processor, event.gid, event.message) == RAIJU_PROCESSOR_OK) {
				changed = true;
			}
			continue;
		}
		char path[RAIJU_JOB_PATH_MAX];
		if (engine_status_path(api->gateway, event.gid, path, sizeof path) &&
		    raiju_processor_download_complete(&api->processor, event.gid, path, 0) == RAIJU_PROCESSOR_OK) {
			changed = true;
		}
	}
	return changed;
}

static bool poll_downloads(raiju_api *api) {
	bool pending = false;
	for (size_t i = 0; i < api->processor.list.count; i++) {
		if (api->processor.list.jobs[i].stage == RAIJU_STAGE_DOWNLOADING) {
			pending = true;
			break;
		}
	}
	if (!pending) {
		return false;
	}
	bool changed = false;
	for (size_t i = 0; i < api->processor.list.count; i++) {
		raiju_job *job = &api->processor.list.jobs[i];
		if (job->stage != RAIJU_STAGE_DOWNLOADING) {
			continue;
		}
		char status[32];
		char path[RAIJU_JOB_PATH_MAX];
		if (!engine_download_state(api->gateway, job->id, status, sizeof status)) {
			continue;
		}
		if (strcmp(status, "complete") == 0) {
			if (engine_status_path(api->gateway, job->id, path, sizeof path) &&
			    raiju_processor_download_complete(&api->processor, job->id, path, 0) == RAIJU_PROCESSOR_OK) {
				changed = true;
			}
		} else if (strcmp(status, "error") == 0) {
			if (raiju_processor_download_failed(&api->processor, job->id, "download_error") == RAIJU_PROCESSOR_OK) {
				changed = true;
			}
		}
	}
	return changed;
}

static void *api_worker(void *argument) {
	raiju_api *api = argument;
	for (;;) {
		pthread_mutex_lock(&api->wake_lock);
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += RAIJU_API_TICK_MS / 1000;
		deadline.tv_nsec += (long)(RAIJU_API_TICK_MS % 1000) * 1000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec += 1;
			deadline.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&api->wake_signal, &api->wake_lock, &deadline);
		bool stopping = api->stopping;
		pthread_mutex_unlock(&api->wake_lock);
		if (stopping) {
			break;
		}
		pthread_mutex_lock(&api->queue_lock);
		bool changed = apply_events(api);
		if (poll_downloads(api)) {
			changed = true;
		}
		if (changed) {
			publish_views(api, true);
		}
		raiju_processor_run(&api->processor);
		publish_views(api, false);
		pthread_mutex_unlock(&api->queue_lock);
	}
	return NULL;
}

/**
 * Wakes the worker without holding the queue lock, so the websocket thread
 * that just received an aria2 notification never waits for a running stage.
 */
static void wake_worker(raiju_api *api) {
	pthread_mutex_lock(&api->wake_lock);
	pthread_cond_broadcast(&api->wake_signal);
	pthread_mutex_unlock(&api->wake_lock);
}

static void queue_event(raiju_api *api, const char *gid, bool failed, const char *message) {
	pthread_mutex_lock(&api->event_lock);
	if (api->event_count < RAIJU_API_MAX_EVENTS) {
		raiju_api_event *event = &api->events[api->event_count++];
		snprintf(event->gid, sizeof event->gid, "%s", gid);
		event->failed = failed;
		snprintf(event->message, sizeof event->message, "%s", failed ? message : "");
	}
	pthread_mutex_unlock(&api->event_lock);
	wake_worker(api);
}

void raiju_api_engine_complete(raiju_gateway *gateway, const char *gid) {
	raiju_api *api = gateway->api;
	if (api == NULL || gid == NULL || !raiju_job_id_valid(gid)) {
		return;
	}
	queue_event(api, gid, false, "");
}

void raiju_api_engine_error(raiju_gateway *gateway, const char *gid, const char *message) {
	raiju_api *api = gateway->api;
	if (api == NULL || gid == NULL || !raiju_job_id_valid(gid)) {
		return;
	}
	queue_event(api, gid, true, message != NULL ? message : "download_error");
}

static int handle_status(const raiju_http_request *request, raiju_http_response *response, void *context) {
	(void)request;
	raiju_api *api = context;
	raiju_gateway *gateway = api->gateway;
	const raiju_platform *platform = gateway->config.platform;
	char address[64] = "";
	platform->interface_ip(platform->ctx, address, sizeof address);
	char firmware[64] = "unknown";
	platform->firmware(platform->ctx, firmware, sizeof firmware);
	uint64_t uptime = (uint64_t)time(NULL) - gateway->started_at;
	char engine_version[64] = "";
	bool engine_up = engine_health(gateway, engine_version, sizeof engine_version);
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	raiju_strbuf_append(&document, "{\"name\":\"raiju\",\"version\":");
	raiju_strbuf_append_json_string(&document, RAIJU_VERSION);
	raiju_strbuf_append(&document, ",\"uptime\":");
	raiju_strbuf_append_uint(&document, (unsigned long long)uptime);
	raiju_strbuf_append(&document, ",\"firmware\":");
	raiju_strbuf_append_json_string(&document, firmware);
	raiju_strbuf_append(&document, ",\"platform\":");
	raiju_strbuf_append_json_string(&document, platform == raiju_platform_host() ? "host" : "ps5");
	raiju_strbuf_append(&document, ",\"ip\":");
	raiju_strbuf_append_json_string(&document, address);
	raiju_strbuf_append(&document, ",\"engine\":{");
	if (engine_up) {
		raiju_strbuf_append(&document, "\"up\":true,\"version\":");
		raiju_strbuf_append_json_string(&document, engine_version);
	} else {
		raiju_strbuf_append(&document, "\"up\":false");
	}
	raiju_strbuf_append(&document, "}}");
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

static int handle_storage(const raiju_http_request *request, raiju_http_response *response, void *context) {
	(void)request;
	raiju_api *api = context;
	raiju_gateway *gateway = api->gateway;
	raiju_disk_usage usage;
	memset(&usage, 0, sizeof usage);
	gateway->config.platform->disk_usage(gateway->config.platform->ctx, gateway->config.data_dir, &usage);
	uint64_t now = (uint64_t)time(NULL);
	uint64_t games = 0;
	uint64_t staging_bytes = 0;
	pthread_mutex_lock(&api->storage_lock);
	if (api->storage_at != 0 && now >= api->storage_at && now - api->storage_at < RAIJU_STORAGE_TTL_SECONDS) {
		games = api->storage_games;
		staging_bytes = api->storage_staging;
	} else {
		char staging[RAIJU_PATH_MAX];
		snprintf(staging, sizeof staging, "%s/staging", gateway->config.data_dir);
		games = directory_bytes(gateway->install_root, 0);
		staging_bytes = directory_bytes(staging, 0);
		api->storage_at = now;
		api->storage_games = games;
		api->storage_staging = staging_bytes;
	}
	pthread_mutex_unlock(&api->storage_lock);
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	raiju_strbuf_append(&document, "{\"totalBytes\":");
	raiju_strbuf_append_uint(&document, (unsigned long long)usage.total_bytes);
	raiju_strbuf_append(&document, ",\"freeBytes\":");
	raiju_strbuf_append_uint(&document, (unsigned long long)usage.free_bytes);
	raiju_strbuf_append(&document, ",\"gamesBytes\":");
	raiju_strbuf_append_uint(&document, (unsigned long long)games);
	raiju_strbuf_append(&document, ",\"stagingBytes\":");
	raiju_strbuf_append_uint(&document, (unsigned long long)staging_bytes);
	raiju_strbuf_append_char(&document, '}');
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

static int percent_decode(const char *text, size_t length, char *out, size_t capacity) {
	size_t written = 0;
	for (size_t i = 0; i < length; i++) {
		int value = 0;
		char c = text[i];
		if (c == '%' && i + 2 < length) {
			char hex[3] = {text[i + 1], text[i + 2], '\0'};
			char *end = NULL;
			value = (int)strtol(hex, &end, 16);
			if (end != hex + 2) {
				value = c;
			} else {
				i += 2;
			}
		} else if (c == '+') {
			value = ' ';
		} else {
			value = c;
		}
		if (written + 1 >= capacity) {
			return -1;
		}
		out[written++] = (char)value;
	}
	out[written] = '\0';
	return 0;
}

static bool query_param(const char *query, const char *name, char *out, size_t capacity) {
	if (query == NULL) {
		return false;
	}
	size_t name_length = strlen(name);
	const char *cursor = query;
	while (cursor != NULL && *cursor != '\0') {
		const char *next = strchr(cursor, '&');
		size_t length = next == NULL ? strlen(cursor) : (size_t)(next - cursor);
		const char *equals = memchr(cursor, '=', length);
		if (equals != NULL && (size_t)(equals - cursor) == name_length &&
		    strncmp(cursor, name, name_length) == 0) {
			return percent_decode(equals + 1, length - (size_t)(equals - cursor) - 1, out, capacity) == 0;
		}
		cursor = next == NULL ? NULL : next + 1;
	}
	return false;
}

static int handle_folders(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_api *api = context;
	raiju_gateway *gateway = api->gateway;
	char requested[RAIJU_PATH_MAX];
	if (!query_param(request->query, "path", requested, sizeof requested)) {
		snprintf(requested, sizeof requested, "%s", gateway->install_root);
	}
	char resolved[RAIJU_PATH_MAX];
	raiju_allow_status status = raiju_allowlist_check(&gateway->allowlist, requested, resolved, sizeof resolved);
	if (status == RAIJU_ALLOW_RESOLVE_FAILED) {
		raiju_response_error(response, 404, "not found");
		return 404;
	}
	if (status != RAIJU_ALLOW_OK) {
		raiju_response_error(response, 403, raiju_allow_status_name(status));
		return 403;
	}
	struct stat info;
	if (stat(resolved, &info) != 0 || !S_ISDIR(info.st_mode)) {
		raiju_response_error(response, 400, "not a directory");
		return 400;
	}
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	raiju_strbuf_append(&document, "{\"roots\":[");
	for (size_t i = 0; i < sizeof api_destinations / sizeof api_destinations[0]; i++) {
		if (i > 0) {
			raiju_strbuf_append_char(&document, ',');
		}
		raiju_strbuf_append(&document, "{\"id\":");
		raiju_strbuf_append_json_string(&document, api_destinations[i].id);
		raiju_strbuf_append(&document, ",\"label\":");
		raiju_strbuf_append_json_string(&document, api_destinations[i].label);
		raiju_strbuf_append(&document, ",\"path\":");
		raiju_strbuf_append_json_string(&document, api_destinations[i].path);
		raiju_strbuf_append_char(&document, '}');
	}
	raiju_strbuf_append(&document, "],\"path\":");
	raiju_strbuf_append_json_string(&document, resolved);
	raiju_strbuf_append(&document, ",\"entries\":[");
	DIR *directory = opendir(resolved);
	if (directory != NULL) {
		struct dirent *entry;
		bool first = true;
		while ((entry = readdir(directory)) != NULL) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}
			char child[RAIJU_PATH_MAX];
			int written = snprintf(child, sizeof child, "%s/%s", resolved, entry->d_name);
			if (written < 0 || (size_t)written >= sizeof child) {
				continue;
			}
			struct stat child_info;
			if (lstat(child, &child_info) != 0) {
				continue;
			}
			if (!first) {
				raiju_strbuf_append_char(&document, ',');
			}
			first = false;
			raiju_strbuf_append(&document, "{\"name\":");
			raiju_strbuf_append_json_string(&document, entry->d_name);
			raiju_strbuf_append(&document, ",\"path\":");
			raiju_strbuf_append_json_string(&document, child);
			raiju_strbuf_append(&document, ",\"directory\":");
			raiju_strbuf_append(&document, S_ISDIR(child_info.st_mode) ? "true" : "false");
			raiju_strbuf_append_char(&document, '}');
		}
		closedir(directory);
	}
	raiju_strbuf_append(&document, "]}");
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

static void art_headers(raiju_http_response *response) {
	raiju_response_set_header(response, "Content-Type", "application/json");
	raiju_response_set_header(response, "Cache-Control", "public, max-age=86400");
}

static int handle_art(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_api *api = context;
	raiju_gateway *gateway = api->gateway;
	if (request->param_count < 1) {
		raiju_response_error(response, 400, "missing title id");
		return 400;
	}
	const char *title_id = request->params[0];
	if (!raiju_art_title_id_valid(title_id)) {
		art_headers(response);
		raiju_response_set_status(response, 404);
		raiju_response_write_str(response, "{\"error\":\"not_found\"}");
		return 404;
	}
	char art_dir[RAIJU_PATH_MAX];
	snprintf(art_dir, sizeof art_dir, "%s/art", gateway->config.data_dir);
	uint64_t now = (uint64_t)time(NULL);
	size_t cached_length = 0;
	char *cached = raiju_art_cache_read(art_dir, title_id, now, &cached_length);
	if (cached != NULL) {
		art_headers(response);
		raiju_response_set_status(response, 200);
		raiju_response_write(response, cached, cached_length);
		free(cached);
		return 200;
	}
	char url[128];
	if (!raiju_art_page_url(title_id, url, sizeof url)) {
		art_headers(response);
		raiju_response_set_status(response, 404);
		raiju_response_write_str(response, "{\"error\":\"not_found\"}");
		return 404;
	}
	raiju_fetch_fn fetch = raiju_api_fetch_fn(gateway);
	raiju_fetch_response fetched;
	int result = fetch(raiju_api_fetch_context(gateway), RAIJU_FETCH_GET, url, NULL, 2u * 1024u * 1024u,
	                   RAIJU_FETCH_DEFAULT_TIMEOUT_MS, &fetched);
	if (result != 0 || fetched.status != 200) {
		raiju_fetch_response_free(&fetched);
		art_headers(response);
		raiju_response_set_status(response, 404);
		raiju_response_write_str(response, "{\"error\":\"not_found\"}");
		return 404;
	}
	raiju_art *art = raiju_art_parse(title_id, fetched.body != NULL ? fetched.body : "", fetched.body_length);
	raiju_fetch_response_free(&fetched);
	if (art == NULL) {
		art_headers(response);
		raiju_response_set_status(response, 404);
		raiju_response_write_str(response, "{\"error\":\"not_found\"}");
		return 404;
	}
	size_t json_length = 0;
	char *json = raiju_art_to_json(art, &json_length);
	raiju_art_free(art);
	if (json == NULL) {
		raiju_response_error(response, 500, "could not serialise");
		return 500;
	}
	raiju_art_cache_write(art_dir, title_id, json, json_length);
	art_headers(response);
	raiju_response_set_status(response, 200);
	raiju_response_write(response, json, json_length);
	free(json);
	return 200;
}

static raiju_category category_for_kind(const char *kind) {
	raiju_kind parsed;
	if (kind != NULL && raiju_kind_from_name(kind, &parsed)) {
		return raiju_kind_category(parsed);
	}
	return RAIJU_CATEGORY_OTHER;
}

static int handle_jobs_list(const raiju_http_request *request, raiju_http_response *response, void *context) {
	(void)request;
	raiju_api *api = context;
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	raiju_api_jobs_json(api, &document);
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

static int handle_jobs_get(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_api *api = context;
	const char *id = request->param_count >= 1 ? request->params[0] : "";
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	bool found = false;
	pthread_mutex_lock(&api->view_lock);
	for (size_t i = 0; i < api->view_count; i++) {
		if (strcmp(api->views[i].id, id) == 0) {
			raiju_api_job_view_json(&document, &api->views[i]);
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&api->view_lock);
	if (!found) {
		raiju_strbuf_free(&document);
		raiju_response_error(response, 404, "not found");
		return 404;
	}
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

static int handle_jobs_create(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_api *api = context;
	raiju_gateway *gateway = api->gateway;
	const char *body = (const char *)request->body;
	size_t length = request->body_length;
	char id[RAIJU_JOB_ID_MAX + 1];
	char uri[2048];
	char destination[RAIJU_PATH_MAX];
	char title_id[RAIJU_TITLE_ID_LEN + 1];
	char file_name[RAIJU_JOB_NAME_MAX];
	char conflict[16];
	char kind[16];
	if (!json_string(body, length, "id", id, sizeof id) || !json_string(body, length, "uri", uri, sizeof uri)) {
		raiju_response_error(response, 400, "id and uri are required");
		return 400;
	}
	if (!raiju_job_id_valid(id)) {
		raiju_response_error(response, 400, "invalid job id");
		return 400;
	}
	if (!json_string(body, length, "destination", destination, sizeof destination)) {
		snprintf(destination, sizeof destination, "%s", gateway->install_root);
	}
	if (!destination_allowed(gateway, destination)) {
		raiju_response_error(response, 403, "destination outside allowlisted roots");
		return 403;
	}
	title_id[0] = '\0';
	json_string(body, length, "titleId", title_id, sizeof title_id);
	file_name[0] = '\0';
	json_string(body, length, "fileName", file_name, sizeof file_name);
	conflict[0] = '\0';
	json_string(body, length, "conflict", conflict, sizeof conflict);
	kind[0] = '\0';
	json_string(body, length, "kind", kind, sizeof kind);
	raiju_conflict_choice policy = RAIJU_CONFLICT_ASK;
	if (strcmp(conflict, "rename") == 0) {
		policy = RAIJU_CONFLICT_RENAME;
	} else if (strcmp(conflict, "replace") == 0) {
		policy = RAIJU_CONFLICT_REPLACE;
	}
	uint64_t size = 0;
	json_uint(body, length, "size", &size);
	uint64_t uncompressed = 0;
	json_uint(body, length, "uncompressedSize", &uncompressed);
	bool keep_archive = false;
	json_bool(body, length, "keepArchive", &keep_archive);
	raiju_job_options options;
	memset(&options, 0, sizeof options);
	options.id = id;
	options.file_name = file_name[0] != '\0' ? file_name : NULL;
	options.install_root = destination;
	options.expected_size = size;
	options.uncompressed_size = uncompressed;
	options.keep_archive = keep_archive;
	options.conflict_policy = policy;
	options.title_id = title_id[0] != '\0' ? title_id : NULL;
	raiju_category category = kind[0] != '\0' ? category_for_kind(kind)
	                                          : (uncompressed > 0 ? RAIJU_CATEGORY_ARCHIVE : RAIJU_CATEGORY_OTHER);
	pthread_mutex_lock(&api->queue_lock);
	raiju_space_report space;
	raiju_processor_status status = raiju_processor_add(&api->processor, &options, category, &space);
	if (status == RAIJU_PROCESSOR_NO_SPACE) {
		pthread_mutex_unlock(&api->queue_lock);
		raiju_strbuf document;
		raiju_strbuf_init(&document);
		raiju_strbuf_append(&document, "{\"error\":\"no_space\",\"requiredBytes\":");
		raiju_strbuf_append_uint(&document, (unsigned long long)space.required_bytes);
		raiju_strbuf_append(&document, ",\"freeBytes\":");
		raiju_strbuf_append_uint(&document, (unsigned long long)space.free_bytes);
		raiju_strbuf_append(&document, ",\"estimated\":");
		raiju_strbuf_append(&document, space.estimated ? "true" : "false");
		raiju_strbuf_append_char(&document, '}');
		raiju_response_json(response, 409, raiju_strbuf_cstr(&document));
		raiju_strbuf_free(&document);
		return 409;
	}
	if (status == RAIJU_PROCESSOR_DUPLICATE) {
		pthread_mutex_unlock(&api->queue_lock);
		raiju_response_error(response, 409, "duplicate job");
		return 409;
	}
	if (status != RAIJU_PROCESSOR_OK) {
		pthread_mutex_unlock(&api->queue_lock);
		raiju_response_error(response, 400, raiju_processor_status_name(status));
		return 400;
	}
	char downloads[RAIJU_PATH_MAX];
	snprintf(downloads, sizeof downloads, "%s/downloads", gateway->config.data_dir);
	if (!engine_add_uri(gateway, id, uri, downloads)) {
		raiju_processor_discard(&api->processor, id);
		pthread_mutex_unlock(&api->queue_lock);
		raiju_response_error(response, 502, "engine unreachable");
		return 502;
	}
	raiju_job *job = raiju_processor_find(&api->processor, id);
	publish_views(api, true);
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	if (job != NULL) {
		raiju_api_job_json(&document, job);
	}
	pthread_mutex_unlock(&api->queue_lock);
	raiju_response_json(response, 201, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 201;
}

static int handle_jobs_retry(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_api *api = context;
	const char *id = request->param_count >= 1 ? request->params[0] : "";
	pthread_mutex_lock(&api->queue_lock);
	raiju_processor_status status = raiju_processor_retry(&api->processor, id);
	raiju_job *job = raiju_processor_find(&api->processor, id);
	publish_views(api, true);
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	if (job != NULL) {
		raiju_api_job_json(&document, job);
	}
	pthread_mutex_unlock(&api->queue_lock);
	wake_worker(api);
	int code = status == RAIJU_PROCESSOR_NOT_FOUND ? 404 : status == RAIJU_PROCESSOR_WRONG_STAGE ? 409 : 200;
	if (code != 200) {
		raiju_strbuf_free(&document);
		raiju_response_error(response, code, raiju_processor_status_name(status));
		return code;
	}
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

static int handle_jobs_cancel(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_api *api = context;
	const char *id = request->param_count >= 1 ? request->params[0] : "";
	pthread_mutex_lock(&api->cancel_lock);
	bool listed = false;
	size_t free_slot = RAIJU_API_MAX_CANCELS;
	for (size_t i = 0; i < RAIJU_API_MAX_CANCELS; i++) {
		if (api->cancel_ids[i][0] == '\0') {
			if (free_slot == RAIJU_API_MAX_CANCELS) {
				free_slot = i;
			}
			continue;
		}
		if (strcmp(api->cancel_ids[i], id) == 0) {
			listed = true;
			break;
		}
	}
	if (!listed && free_slot < RAIJU_API_MAX_CANCELS) {
		snprintf(api->cancel_ids[free_slot], RAIJU_JOB_ID_MAX + 1, "%s", id);
	}
	pthread_mutex_unlock(&api->cancel_lock);
	pthread_mutex_lock(&api->queue_lock);
	raiju_processor_status status = raiju_processor_cancel(&api->processor, id);
	raiju_job *job = raiju_processor_find(&api->processor, id);
	publish_views(api, true);
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	if (job != NULL) {
		raiju_api_job_json(&document, job);
	}
	pthread_mutex_unlock(&api->queue_lock);
	pthread_mutex_lock(&api->cancel_lock);
	for (size_t i = 0; i < RAIJU_API_MAX_CANCELS; i++) {
		if (api->cancel_ids[i][0] != '\0' && strcmp(api->cancel_ids[i], id) == 0) {
			api->cancel_ids[i][0] = '\0';
		}
	}
	pthread_mutex_unlock(&api->cancel_lock);
	if (status == RAIJU_PROCESSOR_NOT_FOUND) {
		raiju_strbuf_free(&document);
		raiju_response_error(response, 404, "not found");
		return 404;
	}
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

static int handle_jobs_resolve(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_api *api = context;
	const char *id = request->param_count >= 1 ? request->params[0] : "";
	char choice[16];
	if (!json_string((const char *)request->body, request->body_length, "choice", choice, sizeof choice)) {
		raiju_response_error(response, 400, "choice is required");
		return 400;
	}
	raiju_conflict_choice resolved;
	if (!raiju_conflict_choice_from_name(choice, &resolved) || resolved == RAIJU_CONFLICT_ASK) {
		raiju_response_error(response, 400, "invalid choice");
		return 400;
	}
	pthread_mutex_lock(&api->queue_lock);
	raiju_processor_status status = raiju_processor_resolve(&api->processor, id, resolved);
	raiju_job *job = raiju_processor_find(&api->processor, id);
	publish_views(api, true);
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	if (job != NULL) {
		raiju_api_job_json(&document, job);
	}
	pthread_mutex_unlock(&api->queue_lock);
	wake_worker(api);
	if (status == RAIJU_PROCESSOR_NOT_FOUND) {
		raiju_strbuf_free(&document);
		raiju_response_error(response, 404, "not found");
		return 404;
	}
	if (status == RAIJU_PROCESSOR_WRONG_STAGE || status == RAIJU_PROCESSOR_INVALID) {
		raiju_strbuf_free(&document);
		raiju_response_error(response, 409, raiju_processor_status_name(status));
		return 409;
	}
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

bool raiju_api_add_core_routes(raiju_gateway *gateway, raiju_api *api) {
	const raiju_route routes[] = {
	    {"GET", "/api/status", handle_status, api, false},
	    {"GET", "/api/storage", handle_storage, api, true},
	    {"GET", "/api/folders", handle_folders, api, true},
	    {"GET", "/api/art/:id", handle_art, api, true},
	};
	for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
		if (!raiju_gateway_add_route(gateway, &routes[i])) {
			return false;
		}
	}
	return true;
}

bool raiju_api_add_job_routes(raiju_gateway *gateway, raiju_api *api) {
	const raiju_route routes[] = {
	    {"GET", "/api/jobs", handle_jobs_list, api, true},
	    {"POST", "/api/jobs", handle_jobs_create, api, true},
	    {"GET", "/api/jobs/:id", handle_jobs_get, api, true},
	    {"POST", "/api/jobs/:id/retry", handle_jobs_retry, api, true},
	    {"POST", "/api/jobs/:id/cancel", handle_jobs_cancel, api, true},
	    {"POST", "/api/jobs/:id/resolve", handle_jobs_resolve, api, true},
	};
	for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
		if (!raiju_gateway_add_route(gateway, &routes[i])) {
			return false;
		}
	}
	return true;
}

struct raiju_api *raiju_api_register(raiju_gateway *gateway) {
	raiju_api *api = calloc(1, sizeof *api);
	if (api == NULL) {
		return NULL;
	}
	api->gateway = gateway;
	raiju_pipeline pipeline;
	memset(&pipeline, 0, sizeof pipeline);
	snprintf(api->staging_root, sizeof api->staging_root, "%s/staging", gateway->config.data_dir);
	snprintf(api->trash_root, sizeof api->trash_root, "%s/trash", gateway->config.data_dir);
	snprintf(api->jobs_path, sizeof api->jobs_path, "%s/session/jobs.json", gateway->config.data_dir);
	pipeline.staging_root = api->staging_root;
	pipeline.trash_root = api->trash_root;
	pipeline.rescan = api_rescan;
	pipeline.notify = api_notify;
	pipeline.progress = api_progress;
	pipeline.free_space = api_free_space;
	pipeline.context = api;
	pthread_mutex_init(&api->queue_lock, NULL);
	pthread_mutex_init(&api->view_lock, NULL);
	pthread_mutex_init(&api->probe_lock, NULL);
	pthread_mutex_init(&api->event_lock, NULL);
	pthread_mutex_init(&api->wake_lock, NULL);
	pthread_cond_init(&api->wake_signal, NULL);
	pthread_mutex_init(&api->cancel_lock, NULL);
	pthread_mutex_init(&api->storage_lock, NULL);
	api->views = calloc(RAIJU_JOBS_MAX, sizeof *api->views);
	if (api->views == NULL) {
		pthread_mutex_destroy(&api->queue_lock);
		pthread_mutex_destroy(&api->view_lock);
		pthread_mutex_destroy(&api->probe_lock);
		pthread_mutex_destroy(&api->event_lock);
		pthread_mutex_destroy(&api->wake_lock);
		pthread_cond_destroy(&api->wake_signal);
		pthread_mutex_destroy(&api->cancel_lock);
		pthread_mutex_destroy(&api->storage_lock);
		free(api);
		return NULL;
	}
	if (raiju_processor_open(&api->processor, &pipeline, api->jobs_path) != RAIJU_PROCESSOR_OK) {
		free(api->views);
		free(api);
		return NULL;
	}
	publish_views(api, true);
	if (!raiju_api_add_core_routes(gateway, api) || !raiju_api_add_job_routes(gateway, api) ||
	    !raiju_api_add_probe_route(gateway, api)) {
		raiju_processor_close(&api->processor);
		free(api->views);
		free(api);
		return NULL;
	}
	if (pthread_create(&api->worker, NULL, api_worker, api) != 0) {
		raiju_processor_close(&api->processor);
		free(api->views);
		free(api);
		return NULL;
	}
	api->worker_started = true;
	gateway->api = api;
	return api;
}

void raiju_api_unregister(raiju_gateway *gateway) {
	raiju_api *api = gateway->api;
	if (api == NULL) {
		return;
	}
	gateway->api = NULL;
	if (api->worker_started) {
		pthread_mutex_lock(&api->wake_lock);
		api->stopping = true;
		pthread_cond_broadcast(&api->wake_signal);
		pthread_mutex_unlock(&api->wake_lock);
		pthread_join(api->worker, NULL);
	}
	raiju_processor_close(&api->processor);
	pthread_mutex_destroy(&api->queue_lock);
	pthread_mutex_destroy(&api->view_lock);
	pthread_mutex_destroy(&api->probe_lock);
	pthread_mutex_destroy(&api->event_lock);
	pthread_mutex_destroy(&api->wake_lock);
	pthread_cond_destroy(&api->wake_signal);
	pthread_mutex_destroy(&api->cancel_lock);
	pthread_mutex_destroy(&api->storage_lock);
	free(api->views);
	free(api);
}
