#include "raiju/job.h"

#include <stdio.h>
#include <string.h>

typedef struct {
	int value;
	const char *name;
} name_entry;

static const name_entry stage_names[] = {
    {RAIJU_STAGE_DOWNLOADING, "downloading"},
    {RAIJU_STAGE_VERIFYING, "verifying"},
    {RAIJU_STAGE_CLASSIFYING, "classifying"},
    {RAIJU_STAGE_EXTRACTING, "extracting"},
    {RAIJU_STAGE_INSTALLING, "installing"},
    {RAIJU_STAGE_RESCAN, "rescan"},
    {RAIJU_STAGE_NOTIFYING, "notifying"},
    {RAIJU_STAGE_READY, "ready"},
    {RAIJU_STAGE_AWAITING_TITLE_ID, "awaiting_title_id"},
    {RAIJU_STAGE_CONFLICT, "conflict"},
    {RAIJU_STAGE_FAILED, "failed"},
    {RAIJU_STAGE_CANCELLED, "cancelled"},
};

static const name_entry payload_names[] = {
    {RAIJU_PAYLOAD_NONE, "none"},
    {RAIJU_PAYLOAD_IMAGE, "image"},
    {RAIJU_PAYLOAD_FOLDER, "folder"},
    {RAIJU_PAYLOAD_FILE, "file"},
};

static const name_entry conflict_names[] = {
    {RAIJU_CONFLICT_ASK, "ask"},
    {RAIJU_CONFLICT_RENAME, "rename"},
    {RAIJU_CONFLICT_REPLACE, "replace"},
    {RAIJU_CONFLICT_CANCEL, "cancel"},
};

static const name_entry title_source_names[] = {
    {RAIJU_TITLE_SOURCE_NONE, "none"},
    {RAIJU_TITLE_SOURCE_PARAM_JSON, "param_json"},
    {RAIJU_TITLE_SOURCE_FILE_NAME, "file_name"},
    {RAIJU_TITLE_SOURCE_USER, "user"},
};

static const raiju_kind all_kinds[] = {
    RAIJU_KIND_UNKNOWN, RAIJU_KIND_7Z,    RAIJU_KIND_ZIP,  RAIJU_KIND_RAR, RAIJU_KIND_ZSTD, RAIJU_KIND_XZ,
    RAIJU_KIND_GZIP,    RAIJU_KIND_EXFAT, RAIJU_KIND_UFS2, RAIJU_KIND_PFS, RAIJU_KIND_PFSC,
};

/**
 * Looks up the name of value in a table, or "unknown".
 */
static const char *name_of(const name_entry *table, size_t count, int value) {
	for (size_t i = 0; i < count; i++) {
		if (table[i].value == value) {
			return table[i].name;
		}
	}
	return "unknown";
}

/**
 * Looks up the value named name in a table.
 */
static bool value_of(const name_entry *table, size_t count, const char *name, int *out) {
	for (size_t i = 0; name != NULL && i < count; i++) {
		if (strcmp(table[i].name, name) == 0) {
			*out = table[i].value;
			return true;
		}
	}
	return false;
}

/**
 * Copies text into a fixed buffer, failing instead of truncating.
 */
static bool copy_bounded(char *out, size_t capacity, const char *text) {
	size_t length = strlen(text);
	if (length >= capacity) {
		return false;
	}
	memcpy(out, text, length + 1);
	return true;
}

bool raiju_job_id_valid(const char *text) {
	if (text == NULL || text[0] == '\0') {
		return false;
	}
	size_t length = 0;
	for (const char *c = text; *c != '\0'; c++, length++) {
		bool allowed = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
		               *c == '_' || *c == '-';
		if (!allowed || length >= RAIJU_JOB_ID_MAX) {
			return false;
		}
	}
	return true;
}

bool raiju_job_path_valid(const char *text) {
	if (text == NULL || text[0] != '/' || strlen(text) >= RAIJU_JOB_PATH_MAX) {
		return false;
	}
	if (text[1] == '\0') {
		return true;
	}
	const char *component = text + 1;
	for (;;) {
		const char *slash = strchr(component, '/');
		size_t length = slash ? (size_t)(slash - component) : strlen(component);
		if (length == 0 || (length == 1 && component[0] == '.') ||
		    (length == 2 && component[0] == '.' && component[1] == '.')) {
			return false;
		}
		for (size_t i = 0; i < length; i++) {
			if ((unsigned char)component[i] < 0x20) {
				return false;
			}
		}
		if (slash == NULL) {
			return true;
		}
		component = slash + 1;
	}
}

bool raiju_job_file_name_valid(const char *text) {
	if (text == NULL || text[0] == '\0' || strlen(text) >= RAIJU_JOB_NAME_MAX || strcmp(text, ".") == 0 ||
	    strcmp(text, "..") == 0) {
		return false;
	}
	for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; c++) {
		if (*c == '/' || *c < 0x20) {
			return false;
		}
	}
	return true;
}

int raiju_job_init(raiju_job *job, const raiju_job_options *options) {
	if (job == NULL || options == NULL) {
		return -1;
	}
	memset(job, 0, sizeof *job);
	if (!raiju_job_id_valid(options->id) || !raiju_job_path_valid(options->install_root) ||
	    options->conflict_policy == RAIJU_CONFLICT_CANCEL || (unsigned)options->conflict_policy > RAIJU_CONFLICT_CANCEL) {
		return -1;
	}
	if (options->file_name != NULL && options->file_name[0] != '\0' &&
	    !raiju_job_file_name_valid(options->file_name)) {
		return -1;
	}
	copy_bounded(job->id, sizeof job->id, options->id);
	copy_bounded(job->install_root, sizeof job->install_root, options->install_root);
	if (options->file_name != NULL) {
		copy_bounded(job->file_name, sizeof job->file_name, options->file_name);
	}
	job->stage = RAIJU_STAGE_DOWNLOADING;
	job->failed_stage = RAIJU_STAGE_DOWNLOADING;
	job->expected_size = options->expected_size;
	job->uncompressed_size = options->uncompressed_size;
	job->keep_archive = options->keep_archive;
	job->conflict_policy = options->conflict_policy;
	return 0;
}

bool raiju_job_runnable(const raiju_job *job) {
	switch (job->stage) {
	case RAIJU_STAGE_VERIFYING:
	case RAIJU_STAGE_CLASSIFYING:
	case RAIJU_STAGE_EXTRACTING:
	case RAIJU_STAGE_INSTALLING:
	case RAIJU_STAGE_RESCAN:
	case RAIJU_STAGE_NOTIFYING:
		return true;
	default:
		return false;
	}
}

bool raiju_job_finished(const raiju_job *job) {
	return job->stage == RAIJU_STAGE_READY || job->stage == RAIJU_STAGE_FAILED || job->stage == RAIJU_STAGE_CANCELLED;
}

void raiju_job_fail(raiju_job *job, const char *error) {
	job->failed_stage = job->stage;
	job->stage = RAIJU_STAGE_FAILED;
	snprintf(job->error, sizeof job->error, "%s", error != NULL ? error : "");
	for (char *c = job->error; *c != '\0'; c++) {
		if ((unsigned char)*c < 0x20) {
			*c = ' ';
		}
	}
}

const char *raiju_stage_name(raiju_stage stage) {
	return name_of(stage_names, sizeof stage_names / sizeof stage_names[0], (int)stage);
}

bool raiju_stage_from_name(const char *name, raiju_stage *out) {
	int value;
	if (!value_of(stage_names, sizeof stage_names / sizeof stage_names[0], name, &value)) {
		return false;
	}
	*out = (raiju_stage)value;
	return true;
}

const char *raiju_payload_type_name(raiju_payload_type type) {
	return name_of(payload_names, sizeof payload_names / sizeof payload_names[0], (int)type);
}

bool raiju_payload_type_from_name(const char *name, raiju_payload_type *out) {
	int value;
	if (!value_of(payload_names, sizeof payload_names / sizeof payload_names[0], name, &value)) {
		return false;
	}
	*out = (raiju_payload_type)value;
	return true;
}

const char *raiju_conflict_choice_name(raiju_conflict_choice choice) {
	return name_of(conflict_names, sizeof conflict_names / sizeof conflict_names[0], (int)choice);
}

bool raiju_conflict_choice_from_name(const char *name, raiju_conflict_choice *out) {
	int value;
	if (!value_of(conflict_names, sizeof conflict_names / sizeof conflict_names[0], name, &value)) {
		return false;
	}
	*out = (raiju_conflict_choice)value;
	return true;
}

bool raiju_kind_from_name(const char *name, raiju_kind *out) {
	for (size_t i = 0; name != NULL && i < sizeof all_kinds / sizeof all_kinds[0]; i++) {
		if (strcmp(raiju_kind_name(all_kinds[i]), name) == 0) {
			*out = all_kinds[i];
			return true;
		}
	}
	return false;
}

const char *raiju_title_source_name(raiju_title_source source) {
	return name_of(title_source_names, sizeof title_source_names / sizeof title_source_names[0], (int)source);
}

bool raiju_title_source_from_name(const char *name, raiju_title_source *out) {
	int value;
	if (!value_of(title_source_names, sizeof title_source_names / sizeof title_source_names[0], name, &value)) {
		return false;
	}
	*out = (raiju_title_source)value;
	return true;
}
