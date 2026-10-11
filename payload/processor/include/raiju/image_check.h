#ifndef RAIJU_IMAGE_CHECK_H
#define RAIJU_IMAGE_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAIJU_EXFAT_RECOMMENDED_CLUSTER 65536u

typedef enum {
	RAIJU_IMAGE_OK = 0,
	RAIJU_IMAGE_SHORT_READ,
	RAIJU_IMAGE_BAD_SIGNATURE,
	RAIJU_IMAGE_BAD_GEOMETRY,
	RAIJU_IMAGE_BAD_CHECKSUM,
	RAIJU_IMAGE_TRUNCATED
} raiju_image_status;

/**
 * Result of a structural image check. status is RAIJU_IMAGE_OK when the image
 * may be installed. warn_cluster_size is a non-fatal exFAT warning set when
 * the cluster size differs from the 64 KiB ShadowMountPlus recommends.
 * Geometry fields are filled as far as parsing got.
 */
typedef struct {
	raiju_image_status status;
	uint32_t sector_size;
	uint32_t cluster_size;
	uint32_t block_size;
	uint32_t fragment_size;
	uint64_t filesystem_bytes;
	bool warn_cluster_size;
} raiju_image_report;

/**
 * Checks an exFAT image: main boot sector fields, the boot-region checksum in
 * sector 11, the cluster size, and that the volume fits in image_size bytes.
 * head holds the start of the image and needs 12 sectors (up to 48 KiB).
 */
raiju_image_report raiju_check_exfat(const uint8_t *head, size_t head_len, uint64_t image_size);

/**
 * Checks a UFS2 image: superblock magic, plausible block and fragment sizes,
 * and that the filesystem size fits in image_size bytes. head holds the start
 * of the image and needs RAIJU_PROBE_SIZE bytes.
 */
raiju_image_report raiju_check_ufs2(const uint8_t *head, size_t head_len, uint64_t image_size);

/**
 * Returns a stable lowercase name for an image check status.
 */
const char *raiju_image_status_name(raiju_image_status status);

#endif
