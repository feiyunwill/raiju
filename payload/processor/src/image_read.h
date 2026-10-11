#ifndef RAIJU_IMAGE_READ_H
#define RAIJU_IMAGE_READ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raiju/image_fs.h"

/**
 * Reads length bytes at offset after checking that the whole range lies
 * inside the image. A range outside the image means an on-disk value pointed
 * somewhere impossible and gives RAIJU_IMAGE_FS_CORRUPT; a failed read gives
 * RAIJU_IMAGE_FS_IO_ERROR.
 */
raiju_image_fs_status raiju_image_read(const raiju_image_io *io, uint64_t offset, void *buffer, size_t length);

/**
 * Returns whether offset + length fits in limit without overflowing.
 */
static inline bool raiju_range_fits(uint64_t offset, uint64_t length, uint64_t limit) {
	return offset <= limit && length <= limit - offset;
}

#endif
