#define _POSIX_C_SOURCE 200809L

#include "raiju/space.h"

#include <stddef.h>
#include <sys/statvfs.h>

/**
 * Adds two sizes, saturating at UINT64_MAX instead of wrapping.
 */
static uint64_t saturating_add(uint64_t a, uint64_t b) {
	return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

raiju_space_need raiju_space_needed(raiju_category category, uint64_t download_size, uint64_t uncompressed_size) {
	raiju_space_need need = {saturating_add(download_size, RAIJU_SPACE_MARGIN_BYTES), false};
	if (category == RAIJU_CATEGORY_ARCHIVE) {
		if (uncompressed_size == 0) {
			uncompressed_size = download_size;
			need.estimated = true;
		}
		need.required_bytes = saturating_add(need.required_bytes, uncompressed_size);
	}
	return need;
}

raiju_space_report raiju_space_check(raiju_space_need need, const char *path, raiju_free_space_fn free_space,
                                     void *context) {
	raiju_space_report report = {RAIJU_SPACE_UNKNOWN, need.required_bytes, 0, need.estimated};
	if (free_space == NULL) {
		free_space = raiju_statvfs_free_space;
	}
	if (path == NULL || free_space(path, &report.free_bytes, context) != 0) {
		report.free_bytes = 0;
		return report;
	}
	report.status = need.required_bytes <= report.free_bytes ? RAIJU_SPACE_OK : RAIJU_SPACE_NOT_ENOUGH;
	return report;
}

int raiju_statvfs_free_space(const char *path, uint64_t *free_bytes, void *context) {
	(void)context;
	struct statvfs info;
	if (path == NULL || free_bytes == NULL || statvfs(path, &info) != 0) {
		return -1;
	}
	uint64_t block = info.f_frsize ? (uint64_t)info.f_frsize : (uint64_t)info.f_bsize;
	uint64_t blocks = (uint64_t)info.f_bavail;
	*free_bytes = (block != 0 && blocks > UINT64_MAX / block) ? UINT64_MAX : blocks * block;
	return 0;
}

const char *raiju_space_status_name(raiju_space_status status) {
	switch (status) {
	case RAIJU_SPACE_OK:
		return "ok";
	case RAIJU_SPACE_NOT_ENOUGH:
		return "not_enough";
	case RAIJU_SPACE_UNKNOWN:
		return "unknown";
	}
	return "unknown";
}
