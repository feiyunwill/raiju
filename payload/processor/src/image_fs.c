#define _POSIX_C_SOURCE 200809L

#include "raiju/image_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "image_read.h"

/**
 * Reads exactly length bytes at offset from the descriptor stored in context,
 * retrying short reads and interrupted calls.
 */
static int fd_read_at(void *context, uint64_t offset, void *buffer, size_t length) {
	int fd = (int)(intptr_t)context;
	uint8_t *cursor = buffer;
	while (length > 0) {
		if (offset > (uint64_t)INT64_MAX) {
			return -1;
		}
		ssize_t got = pread(fd, cursor, length, (off_t)offset);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			return -1;
		}
		cursor += got;
		offset += (uint64_t)got;
		length -= (size_t)got;
	}
	return 0;
}

int raiju_image_io_from_fd(raiju_image_io *io, int fd) {
	if (io == NULL || fd < 0) {
		return -1;
	}
	struct stat info;
	if (fstat(fd, &info) != 0 || info.st_size < 0) {
		return -1;
	}
	io->read_at = fd_read_at;
	io->context = (void *)(intptr_t)fd;
	io->size = (uint64_t)info.st_size;
	return 0;
}

raiju_image_fs_status raiju_image_read(const raiju_image_io *io, uint64_t offset, void *buffer, size_t length) {
	if (!raiju_range_fits(offset, length, io->size)) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	if (length == 0) {
		return RAIJU_IMAGE_FS_OK;
	}
	return io->read_at(io->context, offset, buffer, length) == 0 ? RAIJU_IMAGE_FS_OK : RAIJU_IMAGE_FS_IO_ERROR;
}

raiju_image_fs_status raiju_image_read_param_json(const raiju_image_io *io, raiju_kind kind, uint8_t **out,
                                                  size_t *out_len) {
	switch (kind) {
	case RAIJU_KIND_EXFAT:
		return raiju_exfat_read_param_json(io, out, out_len);
	case RAIJU_KIND_UFS2:
		return raiju_ufs2_read_param_json(io, out, out_len);
	default:
		return RAIJU_IMAGE_FS_UNSUPPORTED;
	}
}

/**
 * Detects the image kind from the leading bytes the classifier needs.
 */
static raiju_image_fs_status detect_image_kind(const raiju_image_io *io, raiju_kind *kind) {
	size_t probe_len = io->size < RAIJU_PROBE_SIZE ? (size_t)io->size : RAIJU_PROBE_SIZE;
	uint8_t *probe = malloc(RAIJU_PROBE_SIZE);
	if (probe == NULL) {
		return RAIJU_IMAGE_FS_NO_MEMORY;
	}
	raiju_image_fs_status status = raiju_image_read(io, 0, probe, probe_len);
	if (status == RAIJU_IMAGE_FS_OK) {
		*kind = raiju_detect_magic(probe, probe_len);
	}
	free(probe);
	return status;
}

raiju_param_status raiju_title_id_from_image(const char *path, char out[RAIJU_TITLE_ID_LEN + 1],
                                             raiju_image_fs_status *fs_status) {
	raiju_image_fs_status status = RAIJU_IMAGE_FS_IO_ERROR;
	raiju_param_status result = RAIJU_PARAM_IO_ERROR;
	int fd = (path != NULL && out != NULL) ? open(path, O_RDONLY) : -1;
	raiju_image_io io;
	if (fd >= 0 && raiju_image_io_from_fd(&io, fd) == 0) {
		raiju_kind kind = RAIJU_KIND_UNKNOWN;
		status = detect_image_kind(&io, &kind);
		uint8_t *json = NULL;
		size_t json_len = 0;
		if (status == RAIJU_IMAGE_FS_OK) {
			status = raiju_image_read_param_json(&io, kind, &json, &json_len);
		}
		if (status == RAIJU_IMAGE_FS_OK) {
			result = raiju_title_id_from_param_json((const char *)json, json_len, out);
		} else if (status == RAIJU_IMAGE_FS_TOO_LARGE) {
			result = RAIJU_PARAM_TOO_LARGE;
		}
		free(json);
	}
	if (fd >= 0) {
		close(fd);
	}
	if (fs_status != NULL) {
		*fs_status = status;
	}
	return result;
}

bool raiju_display_name_from_image(const char *path, char *out, size_t out_size) {
	if (path == NULL || out == NULL || out_size == 0) {
		return false;
	}
	out[0] = '\0';
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return false;
	}
	bool found = false;
	raiju_image_io io;
	if (raiju_image_io_from_fd(&io, fd) == 0) {
		raiju_kind kind = RAIJU_KIND_UNKNOWN;
		uint8_t *json = NULL;
		size_t json_len = 0;
		if (detect_image_kind(&io, &kind) == RAIJU_IMAGE_FS_OK &&
		    raiju_image_read_param_json(&io, kind, &json, &json_len) == RAIJU_IMAGE_FS_OK) {
			found = raiju_display_name_from_param_json((const char *)json, json_len, out, out_size);
		}
		free(json);
	}
	close(fd);
	return found;
}

raiju_title_resolution raiju_resolve_image_title_id(const char *path) {
	char title_id[RAIJU_TITLE_ID_LEN + 1];
	bool from_param = raiju_title_id_from_image(path, title_id, NULL) == RAIJU_PARAM_OK;
	const char *slash = path != NULL ? strrchr(path, '/') : NULL;
	const char *file_name = slash != NULL ? slash + 1 : path;
	return raiju_resolve_title_id(from_param ? title_id : NULL, file_name);
}

const char *raiju_image_fs_status_name(raiju_image_fs_status status) {
	switch (status) {
	case RAIJU_IMAGE_FS_OK:
		return "ok";
	case RAIJU_IMAGE_FS_IO_ERROR:
		return "io_error";
	case RAIJU_IMAGE_FS_CORRUPT:
		return "corrupt";
	case RAIJU_IMAGE_FS_NOT_FOUND:
		return "not_found";
	case RAIJU_IMAGE_FS_TOO_LARGE:
		return "too_large";
	case RAIJU_IMAGE_FS_UNSUPPORTED:
		return "unsupported";
	case RAIJU_IMAGE_FS_NO_MEMORY:
		return "no_memory";
	default:
		return "unknown";
	}
}
