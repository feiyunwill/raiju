#ifndef RAIJU_IMAGE_FS_H
#define RAIJU_IMAGE_FS_H

#include <stddef.h>
#include <stdint.h>

#include "raiju/classify.h"
#include "raiju/title_id.h"

#define RAIJU_IMAGE_DIR_MAX_BYTES (4u * 1024u * 1024u)

typedef enum {
	RAIJU_IMAGE_FS_OK = 0,
	RAIJU_IMAGE_FS_IO_ERROR,
	RAIJU_IMAGE_FS_CORRUPT,
	RAIJU_IMAGE_FS_NOT_FOUND,
	RAIJU_IMAGE_FS_TOO_LARGE,
	RAIJU_IMAGE_FS_UNSUPPORTED,
	RAIJU_IMAGE_FS_NO_MEMORY
} raiju_image_fs_status;

/**
 * Random-access, read-only view of an image. read_at must fill exactly length
 * bytes starting at offset and return 0, or return -1. size is the image
 * length in bytes; readers never ask for bytes past it. The same readers run
 * on the host (pread on a file) and on the PS5.
 */
typedef struct {
	int (*read_at)(void *context, uint64_t offset, void *buffer, size_t length);
	void *context;
	uint64_t size;
} raiju_image_io;

/**
 * Wraps an open file descriptor in an image view backed by pread. The size
 * comes from fstat. Returns 0 on success and -1 when the file cannot be sized.
 * The descriptor stays owned by the caller.
 */
int raiju_image_io_from_fd(raiju_image_io *io, int fd);

/**
 * Walks root, sce_sys and param.json of an exFAT image and returns the file's
 * bytes in a newly allocated buffer the caller frees. Names match
 * case-insensitively with ASCII folding only. Files larger than
 * RAIJU_PARAM_JSON_MAX_BYTES give RAIJU_IMAGE_FS_TOO_LARGE.
 */
raiju_image_fs_status raiju_exfat_read_param_json(const raiju_image_io *io, uint8_t **out, size_t *out_len);

/**
 * Walks root, sce_sys and param.json of a UFS2 image and returns the file's
 * bytes in a newly allocated buffer the caller frees. Names match exactly.
 * Files larger than RAIJU_PARAM_JSON_MAX_BYTES give RAIJU_IMAGE_FS_TOO_LARGE.
 */
raiju_image_fs_status raiju_ufs2_read_param_json(const raiju_image_io *io, uint8_t **out, size_t *out_len);

/**
 * Dispatches to the exFAT or UFS2 reader by kind. Any other kind gives
 * RAIJU_IMAGE_FS_UNSUPPORTED.
 */
raiju_image_fs_status raiju_image_read_param_json(const raiju_image_io *io, raiju_kind kind, uint8_t **out,
                                                  size_t *out_len);

/**
 * Opens the image at path, detects exFAT or UFS2 from its magic bytes, reads
 * sce_sys/param.json from inside it and extracts the title ID. Any failure to
 * reach the file inside the image gives RAIJU_PARAM_IO_ERROR; fs_status, when
 * not NULL, receives the reader's detailed status.
 */
/**
 * Reads param.json inside an image and extracts the game's display name, using
 * the same reader as the title ID. Writes an empty string and returns false when
 * the image is unreadable, has no param.json or carries no localized title.
 */
bool raiju_display_name_from_image(const char *path, char *out, size_t out_size);

raiju_param_status raiju_title_id_from_image(const char *path, char out[RAIJU_TITLE_ID_LEN + 1],
                                             raiju_image_fs_status *fs_status);

/**
 * Picks the install name for an image from its param.json title ID, falling
 * back to the image's file name, as raiju_resolve_title_id does.
 */
raiju_title_resolution raiju_resolve_image_title_id(const char *path);

/**
 * Returns a stable lowercase name for an image reader status.
 */
const char *raiju_image_fs_status_name(raiju_image_fs_status status);

#endif
