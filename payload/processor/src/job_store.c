#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/job_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"

#define FIELD_NAME_MAX 64

typedef enum {
	FIELD_STRING = 0,
	FIELD_PATH,
	FIELD_NAME,
	FIELD_TITLE_ID,
	FIELD_ID,
	FIELD_U64,
	FIELD_BOOL,
	FIELD_STAGE,
	FIELD_KIND,
	FIELD_PAYLOAD,
	FIELD_CONFLICT,
	FIELD_TITLE_SOURCE
} field_type;

typedef struct {
	const char *name;
	field_type type;
	size_t offset;
	size_t capacity;
	bool required;
} field_spec;

#define STRING_FIELD(name, type, member, required) \
	{name, type, offsetof(raiju_job, member), sizeof(((raiju_job *)0)->member), required}
#define VALUE_FIELD(name, type, member) {name, type, offsetof(raiju_job, member), 0, false}

static const field_spec job_fields[] = {
    STRING_FIELD("id", FIELD_ID, id, true),
    {"stage", FIELD_STAGE, offsetof(raiju_job, stage), 0, true},
    VALUE_FIELD("failedStage", FIELD_STAGE, failed_stage),
    STRING_FIELD("error", FIELD_STRING, error, false),
    STRING_FIELD("sourcePath", FIELD_PATH, source_path, false),
    STRING_FIELD("fileName", FIELD_NAME, file_name, false),
    VALUE_FIELD("expectedSize", FIELD_U64, expected_size),
    VALUE_FIELD("uncompressedSize", FIELD_U64, uncompressed_size),
    STRING_FIELD("installRoot", FIELD_PATH, install_root, true),
    VALUE_FIELD("keepArchive", FIELD_BOOL, keep_archive),
    VALUE_FIELD("conflictPolicy", FIELD_CONFLICT, conflict_policy),
    VALUE_FIELD("kind", FIELD_KIND, kind),
    VALUE_FIELD("payloadType", FIELD_PAYLOAD, payload_type),
    STRING_FIELD("payloadPath", FIELD_PATH, payload_path, false),
    STRING_FIELD("titleId", FIELD_TITLE_ID, title_id, false),
    STRING_FIELD("displayName", FIELD_STRING, display_name, false),
    VALUE_FIELD("titleSource", FIELD_TITLE_SOURCE, title_source),
    VALUE_FIELD("namesDisagree", FIELD_BOOL, names_disagree),
    STRING_FIELD("installPath", FIELD_PATH, install_path, false),
    VALUE_FIELD("conflictChoice", FIELD_CONFLICT, conflict_choice),
    VALUE_FIELD("progressDone", FIELD_U64, progress_done),
    VALUE_FIELD("progressTotal", FIELD_U64, progress_total),
};

#define JOB_FIELD_COUNT (sizeof job_fields / sizeof job_fields[0])

raiju_job *raiju_job_list_add(raiju_job_list *list, const raiju_job *job) {
	if (list->count >= RAIJU_JOBS_MAX) {
		return NULL;
	}
	if (list->count == list->capacity) {
		size_t capacity = list->capacity ? list->capacity * 2 : 8;
		raiju_job *grown = realloc(list->jobs, capacity * sizeof *grown);
		if (grown == NULL) {
			return NULL;
		}
		list->jobs = grown;
		list->capacity = capacity;
	}
	list->jobs[list->count] = *job;
	return &list->jobs[list->count++];
}

raiju_job *raiju_job_list_find(raiju_job_list *list, const char *id) {
	for (size_t i = 0; id != NULL && i < list->count; i++) {
		if (strcmp(list->jobs[i].id, id) == 0) {
			return &list->jobs[i];
		}
	}
	return NULL;
}

bool raiju_job_list_remove(raiju_job_list *list, const char *id) {
	raiju_job *job = raiju_job_list_find(list, id);
	if (job == NULL) {
		return false;
	}
	size_t index = (size_t)(job - list->jobs);
	memmove(&list->jobs[index], &list->jobs[index + 1], (list->count - index - 1) * sizeof *job);
	list->count--;
	return true;
}

void raiju_job_list_free(raiju_job_list *list) {
	free(list->jobs);
	memset(list, 0, sizeof *list);
}

/**
 * Writes one field of a job as a "key":value pair.
 */
static void write_field(raiju_json_writer *writer, const raiju_job *job, const field_spec *field) {
	const char *base = (const char *)job + field->offset;
	raiju_json_write_string(writer, field->name);
	raiju_json_write_raw(writer, ":");
	switch (field->type) {
	case FIELD_STRING:
	case FIELD_PATH:
	case FIELD_NAME:
	case FIELD_TITLE_ID:
	case FIELD_ID:
		raiju_json_write_string(writer, base);
		break;
	case FIELD_U64:
		raiju_json_write_uint64(writer, *(const uint64_t *)(const void *)base);
		break;
	case FIELD_BOOL:
		raiju_json_write_bool(writer, *(const bool *)(const void *)base);
		break;
	case FIELD_STAGE:
		raiju_json_write_string(writer, raiju_stage_name(*(const raiju_stage *)(const void *)base));
		break;
	case FIELD_KIND:
		raiju_json_write_string(writer, raiju_kind_name(*(const raiju_kind *)(const void *)base));
		break;
	case FIELD_PAYLOAD:
		raiju_json_write_string(writer, raiju_payload_type_name(*(const raiju_payload_type *)(const void *)base));
		break;
	case FIELD_CONFLICT:
		raiju_json_write_string(writer,
		                        raiju_conflict_choice_name(*(const raiju_conflict_choice *)(const void *)base));
		break;
	case FIELD_TITLE_SOURCE:
		raiju_json_write_string(writer, raiju_title_source_name(*(const raiju_title_source *)(const void *)base));
		break;
	}
}

char *raiju_jobs_to_json(const raiju_job_list *list, size_t *out_len) {
	raiju_json_writer writer = {NULL, 0, 0, false};
	raiju_json_write_raw(&writer, "{\"version\":");
	raiju_json_write_uint64(&writer, RAIJU_JOBS_FILE_VERSION);
	raiju_json_write_raw(&writer, ",\"jobs\":[");
	for (size_t i = 0; i < list->count; i++) {
		raiju_json_write_raw(&writer, i == 0 ? "\n{" : ",\n{");
		for (size_t f = 0; f < JOB_FIELD_COUNT; f++) {
			if (f > 0) {
				raiju_json_write_raw(&writer, ",");
			}
			write_field(&writer, &list->jobs[i], &job_fields[f]);
		}
		raiju_json_write_raw(&writer, "}");
	}
	raiju_json_write_raw(&writer, "\n]}\n");
	if (writer.failed) {
		free(writer.data);
		return NULL;
	}
	if (out_len != NULL) {
		*out_len = writer.length;
	}
	return writer.data;
}

/**
 * Reads a short enum name into a buffer.
 */
static bool read_name(raiju_json_cursor *cur, char name[FIELD_NAME_MAX]) {
	return raiju_json_read_string(cur, name, FIELD_NAME_MAX);
}

/**
 * Reads and validates one field value into the job.
 */
static bool read_field(raiju_json_cursor *cur, raiju_job *job, const field_spec *field) {
	char *base = (char *)job + field->offset;
	char name[FIELD_NAME_MAX];
	switch (field->type) {
	case FIELD_STRING:
		return raiju_json_read_string(cur, base, field->capacity);
	case FIELD_PATH:
		return raiju_json_read_string(cur, base, field->capacity) && (base[0] == '\0' || raiju_job_path_valid(base));
	case FIELD_NAME:
		return raiju_json_read_string(cur, base, field->capacity) &&
		       (base[0] == '\0' || raiju_job_file_name_valid(base));
	case FIELD_TITLE_ID:
		return raiju_json_read_string(cur, base, field->capacity) && (base[0] == '\0' || raiju_title_id_valid(base));
	case FIELD_ID:
		return raiju_json_read_string(cur, base, field->capacity) && raiju_job_id_valid(base);
	case FIELD_U64:
		return raiju_json_read_uint64(cur, (uint64_t *)(void *)base);
	case FIELD_BOOL:
		return raiju_json_read_bool(cur, (bool *)(void *)base);
	case FIELD_STAGE:
		return read_name(cur, name) && raiju_stage_from_name(name, (raiju_stage *)(void *)base);
	case FIELD_KIND:
		return read_name(cur, name) && raiju_kind_from_name(name, (raiju_kind *)(void *)base);
	case FIELD_PAYLOAD:
		return read_name(cur, name) && raiju_payload_type_from_name(name, (raiju_payload_type *)(void *)base);
	case FIELD_CONFLICT:
		return read_name(cur, name) && raiju_conflict_choice_from_name(name, (raiju_conflict_choice *)(void *)base);
	case FIELD_TITLE_SOURCE:
		return read_name(cur, name) && raiju_title_source_from_name(name, (raiju_title_source *)(void *)base);
	}
	return false;
}

/**
 * Returns whether path names a direct child of root.
 */
static bool is_direct_child(const char *path, const char *root) {
	size_t root_len = strlen(root);
	while (root_len > 1 && root[root_len - 1] == '/') {
		root_len--;
	}
	if (strncmp(path, root, root_len) != 0) {
		return false;
	}
	const char *rest = path + root_len;
	if (root_len > 1) {
		if (*rest != '/') {
			return false;
		}
		rest++;
	}
	return rest[0] != '\0' && strchr(rest, '/') == NULL;
}

/**
 * Checks that the fields a stage relies on are present, so a resumed job
 * never runs a stage with half its inputs.
 */
static bool job_consistent(const raiju_job *job) {
	raiju_stage stage = job->stage == RAIJU_STAGE_FAILED ? job->failed_stage : job->stage;
	if (job->conflict_policy == RAIJU_CONFLICT_CANCEL) {
		return false;
	}
	if (job->stage == RAIJU_STAGE_FAILED && job->failed_stage > RAIJU_STAGE_NOTIFYING) {
		return false;
	}
	if (job->stage == RAIJU_STAGE_CANCELLED || stage == RAIJU_STAGE_DOWNLOADING) {
		return true;
	}
	if (job->source_path[0] == '\0' || job->file_name[0] == '\0') {
		return false;
	}
	bool past_classify = stage != RAIJU_STAGE_VERIFYING && stage != RAIJU_STAGE_CLASSIFYING &&
	                     stage != RAIJU_STAGE_EXTRACTING;
	if (past_classify && (job->payload_type == RAIJU_PAYLOAD_NONE || job->payload_path[0] == '\0')) {
		return false;
	}
	bool needs_title = past_classify && stage != RAIJU_STAGE_AWAITING_TITLE_ID &&
	                   job->payload_type != RAIJU_PAYLOAD_FILE;
	if (needs_title && job->title_id[0] == '\0') {
		return false;
	}
	if (job->install_path[0] != '\0' && !is_direct_child(job->install_path, job->install_root)) {
		return false;
	}
	bool needs_install_path = stage == RAIJU_STAGE_CONFLICT || stage == RAIJU_STAGE_RESCAN ||
	                          stage == RAIJU_STAGE_NOTIFYING || stage == RAIJU_STAGE_READY;
	return !needs_install_path || job->install_path[0] != '\0';
}

/**
 * Parses one job object. Duplicate known keys are malformed; unknown keys
 * are skipped so newer files still load.
 */
static raiju_store_status read_job(raiju_json_cursor *cur, raiju_job *job) {
	memset(job, 0, sizeof *job);
	bool seen[JOB_FIELD_COUNT] = {false};
	if (!raiju_json_consume(cur, '{')) {
		return RAIJU_STORE_MALFORMED;
	}
	if (!raiju_json_consume(cur, '}')) {
		for (;;) {
			char key[FIELD_NAME_MAX];
			if (!raiju_json_read_string(cur, key, sizeof key) || !raiju_json_consume(cur, ':')) {
				return RAIJU_STORE_MALFORMED;
			}
			size_t index = JOB_FIELD_COUNT;
			for (size_t f = 0; f < JOB_FIELD_COUNT; f++) {
				if (strcmp(job_fields[f].name, key) == 0) {
					index = f;
					break;
				}
			}
			if (index == JOB_FIELD_COUNT) {
				if (!raiju_json_skip_value(cur, 2)) {
					return RAIJU_STORE_MALFORMED;
				}
			} else {
				if (seen[index]) {
					return RAIJU_STORE_MALFORMED;
				}
				seen[index] = true;
				if (!read_field(cur, job, &job_fields[index])) {
					return RAIJU_STORE_INVALID_JOB;
				}
			}
			if (raiju_json_consume(cur, '}')) {
				break;
			}
			if (!raiju_json_consume(cur, ',')) {
				return RAIJU_STORE_MALFORMED;
			}
		}
	}
	for (size_t f = 0; f < JOB_FIELD_COUNT; f++) {
		if (job_fields[f].required && !seen[f]) {
			return RAIJU_STORE_INVALID_JOB;
		}
	}
	return job_consistent(job) ? RAIJU_STORE_OK : RAIJU_STORE_INVALID_JOB;
}

/**
 * Parses the jobs array whose opening bracket follows.
 */
static raiju_store_status read_jobs(raiju_json_cursor *cur, raiju_job_list *out) {
	if (!raiju_json_consume(cur, '[')) {
		return RAIJU_STORE_MALFORMED;
	}
	if (raiju_json_consume(cur, ']')) {
		return RAIJU_STORE_OK;
	}
	for (;;) {
		raiju_job job;
		raiju_store_status status = read_job(cur, &job);
		if (status != RAIJU_STORE_OK) {
			return status;
		}
		if (raiju_job_list_find(out, job.id) != NULL) {
			return RAIJU_STORE_INVALID_JOB;
		}
		if (raiju_job_list_add(out, &job) == NULL) {
			return out->count >= RAIJU_JOBS_MAX ? RAIJU_STORE_TOO_LARGE : RAIJU_STORE_NO_MEMORY;
		}
		if (raiju_json_consume(cur, ']')) {
			return RAIJU_STORE_OK;
		}
		if (!raiju_json_consume(cur, ',')) {
			return RAIJU_STORE_MALFORMED;
		}
	}
}

/**
 * Parses the top-level document: {"version":1,"jobs":[...]}.
 */
static raiju_store_status read_document(raiju_json_cursor *cur, raiju_job_list *out) {
	bool seen_version = false;
	bool seen_jobs = false;
	if (!raiju_json_consume(cur, '{')) {
		return RAIJU_STORE_MALFORMED;
	}
	if (!raiju_json_consume(cur, '}')) {
		for (;;) {
			char key[FIELD_NAME_MAX];
			if (!raiju_json_read_string(cur, key, sizeof key)) {
				return RAIJU_STORE_MALFORMED;
			}
			if (!raiju_json_consume(cur, ':')) {
				return RAIJU_STORE_MALFORMED;
			}
			if (strcmp(key, "version") == 0) {
				uint64_t version;
				if (seen_version || !raiju_json_read_uint64(cur, &version)) {
					return RAIJU_STORE_MALFORMED;
				}
				if (version != RAIJU_JOBS_FILE_VERSION) {
					return RAIJU_STORE_INVALID_JOB;
				}
				seen_version = true;
			} else if (strcmp(key, "jobs") == 0) {
				if (seen_jobs) {
					return RAIJU_STORE_MALFORMED;
				}
				seen_jobs = true;
				raiju_store_status status = read_jobs(cur, out);
				if (status != RAIJU_STORE_OK) {
					return status;
				}
			} else if (!raiju_json_skip_value(cur, 1)) {
				return RAIJU_STORE_MALFORMED;
			}
			if (raiju_json_consume(cur, '}')) {
				break;
			}
			if (!raiju_json_consume(cur, ',')) {
				return RAIJU_STORE_MALFORMED;
			}
		}
	}
	if (!raiju_json_at_end(cur)) {
		return RAIJU_STORE_MALFORMED;
	}
	return seen_version && seen_jobs ? RAIJU_STORE_OK : RAIJU_STORE_MALFORMED;
}

raiju_store_status raiju_jobs_from_json(const char *json, size_t json_len, raiju_job_list *out) {
	if (json == NULL || out == NULL) {
		return RAIJU_STORE_MALFORMED;
	}
	memset(out, 0, sizeof *out);
	if (json_len > RAIJU_JOBS_FILE_MAX_BYTES) {
		return RAIJU_STORE_TOO_LARGE;
	}
	raiju_json_cursor cur = {(const unsigned char *)json, (const unsigned char *)json + json_len};
	raiju_store_status status = read_document(&cur, out);
	if (status != RAIJU_STORE_OK) {
		raiju_job_list_free(out);
	}
	return status;
}

/**
 * Writes the whole buffer to a descriptor, retrying short and interrupted
 * writes.
 */
static int write_all(int fd, const char *data, size_t length) {
	while (length > 0) {
		ssize_t written = write(fd, data, length);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			return -1;
		}
		data += written;
		length -= (size_t)written;
	}
	return 0;
}

/**
 * Fsyncs the directory containing path so a rename inside it is durable.
 */
static int sync_parent(const char *path) {
	char dir[RAIJU_JOB_PATH_MAX];
	const char *slash = strrchr(path, '/');
	if (slash == NULL) {
		snprintf(dir, sizeof dir, ".");
	} else if (slash == path) {
		snprintf(dir, sizeof dir, "/");
	} else {
		size_t length = (size_t)(slash - path);
		if (length >= sizeof dir) {
			return -1;
		}
		memcpy(dir, path, length);
		dir[length] = '\0';
	}
	int fd = open(dir, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	int status = fsync(fd);
	close(fd);
	return status;
}

raiju_store_status raiju_jobs_save(const raiju_job_list *list, const char *path) {
	if (list == NULL || path == NULL) {
		return RAIJU_STORE_IO_ERROR;
	}
	char temp[RAIJU_JOB_PATH_MAX + 8];
	if ((size_t)snprintf(temp, sizeof temp, "%s.tmp", path) >= sizeof temp) {
		return RAIJU_STORE_IO_ERROR;
	}
	size_t length = 0;
	char *json = raiju_jobs_to_json(list, &length);
	if (json == NULL) {
		return RAIJU_STORE_NO_MEMORY;
	}
	int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		free(json);
		return RAIJU_STORE_IO_ERROR;
	}
	int status = write_all(fd, json, length);
	free(json);
	if (fsync(fd) != 0) {
		status = -1;
	}
	if (close(fd) != 0) {
		status = -1;
	}
	if (status != 0 || rename(temp, path) != 0) {
		unlink(temp);
		return RAIJU_STORE_IO_ERROR;
	}
	return sync_parent(path) == 0 ? RAIJU_STORE_OK : RAIJU_STORE_IO_ERROR;
}

raiju_store_status raiju_jobs_load(const char *path, raiju_job_list *out) {
	if (path == NULL || out == NULL) {
		return RAIJU_STORE_IO_ERROR;
	}
	memset(out, 0, sizeof *out);
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return errno == ENOENT ? RAIJU_STORE_MISSING : RAIJU_STORE_IO_ERROR;
	}
	struct stat info;
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
		close(fd);
		return RAIJU_STORE_IO_ERROR;
	}
	if ((uint64_t)info.st_size > RAIJU_JOBS_FILE_MAX_BYTES) {
		close(fd);
		return RAIJU_STORE_TOO_LARGE;
	}
	size_t capacity = (size_t)info.st_size;
	char *buffer = malloc(capacity + 1);
	if (buffer == NULL) {
		close(fd);
		return RAIJU_STORE_NO_MEMORY;
	}
	size_t length = 0;
	while (length < capacity) {
		ssize_t got = read(fd, buffer + length, capacity - length);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			break;
		}
		length += (size_t)got;
	}
	close(fd);
	if (length != capacity) {
		free(buffer);
		return RAIJU_STORE_IO_ERROR;
	}
	raiju_store_status status = raiju_jobs_from_json(buffer, length, out);
	free(buffer);
	return status;
}

const char *raiju_store_status_name(raiju_store_status status) {
	switch (status) {
	case RAIJU_STORE_OK:
		return "ok";
	case RAIJU_STORE_MISSING:
		return "missing";
	case RAIJU_STORE_IO_ERROR:
		return "io_error";
	case RAIJU_STORE_TOO_LARGE:
		return "too_large";
	case RAIJU_STORE_MALFORMED:
		return "malformed";
	case RAIJU_STORE_INVALID_JOB:
		return "invalid_job";
	case RAIJU_STORE_NO_MEMORY:
		return "no_memory";
	}
	return "unknown";
}
