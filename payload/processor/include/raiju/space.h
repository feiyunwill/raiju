#ifndef RAIJU_SPACE_H
#define RAIJU_SPACE_H

#include <stdbool.h>
#include <stdint.h>

#include "raiju/classify.h"

#define RAIJU_SPACE_MARGIN_BYTES (1024ull * 1024ull * 1024ull)

typedef enum {
	RAIJU_SPACE_OK = 0,
	RAIJU_SPACE_NOT_ENOUGH,
	RAIJU_SPACE_UNKNOWN
} raiju_space_status;

/**
 * Reports the bytes available to Raiju on the filesystem holding path.
 * Returns 0 on success and -1 when the filesystem cannot be queried.
 */
typedef int (*raiju_free_space_fn)(const char *path, uint64_t *free_bytes, void *context);

/**
 * Bytes a download needs before it starts. estimated is set when the
 * uncompressed size of an archive was unknown and 1x the archive size was
 * assumed instead, so the UI can warn.
 */
typedef struct {
	uint64_t required_bytes;
	bool estimated;
} raiju_space_need;

/**
 * Verdict of a space check, with the numbers it compared.
 */
typedef struct {
	raiju_space_status status;
	uint64_t required_bytes;
	uint64_t free_bytes;
	bool estimated;
} raiju_space_report;

/**
 * Computes download_size + uncompressed_size + 1 GiB. Only archives count an
 * uncompressed size; for an archive whose uncompressed size is 0 (unknown,
 * as for tar.*) the download size is used again and the result is marked
 * estimated. Sums saturate at UINT64_MAX.
 */
raiju_space_need raiju_space_needed(raiju_category category, uint64_t download_size, uint64_t uncompressed_size);

/**
 * Compares a need against the free space reported for path by free_space.
 * The check passes when required_bytes <= free bytes.
 */
raiju_space_report raiju_space_check(raiju_space_need need, const char *path, raiju_free_space_fn free_space,
                                     void *context);

/**
 * Free-space callback backed by statvfs: f_bavail * f_frsize.
 */
int raiju_statvfs_free_space(const char *path, uint64_t *free_bytes, void *context);

/**
 * Returns a stable lowercase name for a space status.
 */
const char *raiju_space_status_name(raiju_space_status status);

#endif
