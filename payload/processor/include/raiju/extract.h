#ifndef RAIJU_EXTRACT_H
#define RAIJU_EXTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raiju/inspect.h"
#include "raiju/title_id.h"

#define RAIJU_EXTRACT_MAX_WRAPPER_DEPTH 16

typedef enum {
	RAIJU_EXTRACT_OK = 0,
	RAIJU_EXTRACT_PAUSED,
	RAIJU_EXTRACT_CANCELLED,
	RAIJU_EXTRACT_UNSAFE_PATH,
	RAIJU_EXTRACT_LINK_ENTRY,
	RAIJU_EXTRACT_UNSUPPORTED_ENTRY,
	RAIJU_EXTRACT_ARCHIVE_ERROR,
	RAIJU_EXTRACT_IO_ERROR,
	RAIJU_EXTRACT_STAGING_NOT_EMPTY,
	RAIJU_EXTRACT_NO_PAYLOAD,
	RAIJU_EXTRACT_MULTIPLE_IMAGES,
	RAIJU_EXTRACT_BAD_IMAGE,
	RAIJU_EXTRACT_INVALID_ARGUMENT
} raiju_extract_status;

typedef enum {
	RAIJU_LAYOUT_NONE = 0,
	RAIJU_LAYOUT_FOLDER_DUMP,
	RAIJU_LAYOUT_SINGLE_IMAGE
} raiju_layout;

typedef enum {
	RAIJU_EXTRACT_CONTINUE = 0,
	RAIJU_EXTRACT_PAUSE,
	RAIJU_EXTRACT_CANCEL
} raiju_extract_control;

/**
 * Receives extraction progress: bytes_written uncompressed bytes so far out
 * of bytes_total, which is 0 when the total is unknown. Called after every
 * data block and before every entry. Returning PAUSE or CANCEL stops the
 * extraction at the next entry boundary; the entry being written finishes
 * first.
 */
typedef raiju_extract_control (*raiju_extract_progress_fn)(uint64_t bytes_written, uint64_t bytes_total, void *context);

/**
 * What to extract and where. staging_dir is created when missing and must
 * otherwise be empty; its parent must exist. total_bytes is the uncompressed
 * size used for progress, 0 when unknown. progress may be NULL.
 */
typedef struct {
	const char *archive_path;
	const char *staging_dir;
	uint64_t total_bytes;
	raiju_extract_progress_fn progress;
	void *context;
} raiju_extract_request;

/**
 * Outcome of an extraction. On success layout says which payload was found
 * and payload_path points at it inside staging: the folder holding sce_sys/
 * after wrapper_depth wrapper folders were stripped, or the single image
 * file, whose inspection is stored in image. For a folder dump param_json
 * holds the bytes of sce_sys/param.json (NULL when absent or unreadable),
 * param_status the result of parsing it and title_id the ID it yielded when
 * param_status is RAIJU_PARAM_OK. For a rejected entry, failed_entry names it
 * as stored in the archive. Release with raiju_extract_result_free.
 */
typedef struct {
	raiju_extract_status status;
	raiju_layout layout;
	char *payload_path;
	unsigned wrapper_depth;
	uint64_t bytes_written;
	uint64_t entries;
	char *failed_entry;
	char *param_json;
	size_t param_json_len;
	raiju_param_status param_status;
	char title_id[RAIJU_TITLE_ID_LEN + 1];
	raiju_inspection image;
} raiju_extract_result;

/**
 * Extracts an archive (7z, zip, rar, tar with zstd, xz, gzip, bzip2 or no
 * compression) into the staging directory, then normalizes its layout. Every
 * entry path is checked before it is written: absolute paths, '..'
 * components, backslashes, symlinks, hardlinks and special files fail the
 * whole job. Returns the result status, also stored in out->status.
 */
raiju_extract_status raiju_extract_archive(const raiju_extract_request *request, raiju_extract_result *out);

/**
 * Finds the install payload in an already extracted staging directory:
 * a folder dump whose sce_sys/ may sit under wrapper folders, otherwise
 * exactly one image file (.exfat, .ffpkg, .ffpfs, .ffpfsc) that passes
 * inspection. Fills the layout, payload, image and param.json fields of out.
 */
raiju_extract_status raiju_normalize_layout(const char *staging_dir, raiju_extract_result *out);

/**
 * Sums the uncompressed sizes listed in an archive's entry headers without
 * writing anything. Formats compressed as one stream (tar.*, solid 7z) may
 * need to decompress to reach later headers. Returns 0 on success and -1
 * when the archive cannot be read or an entry has no recorded size.
 */
int raiju_archive_total_bytes(const char *archive_path, uint64_t *total);

/**
 * Removes everything inside a staging directory without following symlinks,
 * leaving the directory itself. Used before resuming, since extraction
 * restarts from the beginning. Returns 0 on success.
 */
int raiju_staging_clear(const char *staging_dir);

/**
 * Releases the strings and buffers owned by a result and zeroes it.
 */
void raiju_extract_result_free(raiju_extract_result *result);

/**
 * Returns a stable lowercase name for an extraction status.
 */
const char *raiju_extract_status_name(raiju_extract_status status);

/**
 * Returns a stable lowercase name for a layout.
 */
const char *raiju_layout_name(raiju_layout layout);

#endif
