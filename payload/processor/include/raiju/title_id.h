#ifndef RAIJU_TITLE_ID_H
#define RAIJU_TITLE_ID_H

#include <stdbool.h>
#include <stddef.h>

#define RAIJU_TITLE_ID_LEN 9
#define RAIJU_PARAM_JSON_MAX_BYTES (1024u * 1024u)

typedef enum {
	RAIJU_PARAM_OK = 0,
	RAIJU_PARAM_MALFORMED,
	RAIJU_PARAM_NO_TITLE_ID,
	RAIJU_PARAM_BAD_TITLE_ID,
	RAIJU_PARAM_TOO_LARGE,
	RAIJU_PARAM_IO_ERROR
} raiju_param_status;

typedef enum {
	RAIJU_TITLE_SOURCE_NONE = 0,
	RAIJU_TITLE_SOURCE_PARAM_JSON,
	RAIJU_TITLE_SOURCE_FILE_NAME,
	RAIJU_TITLE_SOURCE_USER
} raiju_title_source;

/**
 * The install name chosen for a job. source is NONE when neither param.json
 * nor the file name gave a title ID, and the job must ask the user; USER
 * marks an ID the user typed in answer.
 * names_disagree is set when both were found and differ; param.json wins.
 */
typedef struct {
	raiju_title_source source;
	char title_id[RAIJU_TITLE_ID_LEN + 1];
	char name_title_id[RAIJU_TITLE_ID_LEN + 1];
	bool names_disagree;
} raiju_title_resolution;

/**
 * Returns whether text is a well-formed title ID: four uppercase letters and
 * five digits, e.g. PPSA24003.
 */
bool raiju_title_id_valid(const char *text);

/**
 * Extracts the top-level "titleId" string from param.json bytes. The whole
 * document is validated as JSON; duplicate titleId keys are malformed and a
 * value that is not a well-formed title ID is rejected. Writes a
 * NUL-terminated ID to out on success.
 */
raiju_param_status raiju_title_id_from_param_json(const char *json, size_t json_len, char out[RAIJU_TITLE_ID_LEN + 1]);

/**
 * Reads <dump_dir>/sce_sys/param.json from disk and extracts its title ID.
 */
raiju_param_status raiju_title_id_from_folder(const char *dump_dir, char out[RAIJU_TITLE_ID_LEN + 1]);

/**
 * Finds the first match of (PPSA|CUSA)[0-9]{5} in a file or archive name,
 * case-sensitively. Returns false when the name has none.
 */
bool raiju_title_id_from_name(const char *file_name, char out[RAIJU_TITLE_ID_LEN + 1]);

/**
 * Picks the install name from a param.json title ID (NULL when unavailable)
 * and the original file name, reporting when the two disagree.
 */
raiju_title_resolution raiju_resolve_title_id(const char *param_title_id, const char *file_name);

/**
 * Returns a stable lowercase name for a param.json status.
 */
const char *raiju_param_status_name(raiju_param_status status);

#endif
