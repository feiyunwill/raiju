#include "raiju/image_check.h"

#include <string.h>

#include "byte_order.h"
#include "raiju/classify.h"

enum {
	EXFAT_JUMP_BOOT = 0,
	EXFAT_FILE_SYSTEM_NAME = 3,
	EXFAT_MUST_BE_ZERO = 11,
	EXFAT_MUST_BE_ZERO_END = 64,
	EXFAT_VOLUME_LENGTH = 72,
	EXFAT_VOLUME_FLAGS = 106,
	EXFAT_BYTES_PER_SECTOR_SHIFT = 108,
	EXFAT_SECTORS_PER_CLUSTER_SHIFT = 109,
	EXFAT_NUMBER_OF_FATS = 110,
	EXFAT_PERCENT_IN_USE = 112,
	EXFAT_BOOT_SIGNATURE = 510,
	EXFAT_CHECKSUMMED_SECTORS = 11,
	EXFAT_MIN_SECTOR_SHIFT = 9,
	EXFAT_MAX_SECTOR_SHIFT = 12,
	EXFAT_MAX_CLUSTER_SHIFT = 25
};

enum {
	UFS2_BLOCK_SIZE = 48,
	UFS2_FRAGMENT_SIZE = 52,
	UFS2_FRAGS_PER_BLOCK = 56,
	UFS2_SIZE = 1080,
	UFS2_MAGIC = 1372,
	UFS2_MIN_BLOCK_SIZE = 4096,
	UFS2_MAX_BLOCK_SIZE = 65536,
	UFS2_MIN_FRAGMENT_SIZE = 512,
	UFS2_MAX_FRAGS_PER_BLOCK = 8
};

/**
 * Returns whether value is a non-zero power of two.
 */
static bool is_power_of_two(uint32_t value) {
	return value != 0 && (value & (value - 1)) == 0;
}

/**
 * Computes the exFAT boot checksum over the first eleven sectors, skipping
 * VolumeFlags and PercentInUse as the exFAT specification requires.
 */
static uint32_t exfat_boot_checksum(const uint8_t *region, size_t length) {
	uint32_t checksum = 0;
	for (size_t i = 0; i < length; i++) {
		if (i == EXFAT_VOLUME_FLAGS || i == EXFAT_VOLUME_FLAGS + 1 || i == EXFAT_PERCENT_IN_USE) {
			continue;
		}
		checksum = ((checksum & 1u) ? 0x80000000u : 0u) + (checksum >> 1) + region[i];
	}
	return checksum;
}

/**
 * Validates the fixed fields of an exFAT main boot sector.
 */
static bool exfat_boot_sector_sane(const uint8_t *sector) {
	static const uint8_t jump_boot[] = {0xEB, 0x76, 0x90};
	static const char name[] = "EXFAT   ";
	if (memcmp(sector + EXFAT_JUMP_BOOT, jump_boot, sizeof jump_boot) != 0) {
		return false;
	}
	if (memcmp(sector + EXFAT_FILE_SYSTEM_NAME, name, 8) != 0) {
		return false;
	}
	for (size_t i = EXFAT_MUST_BE_ZERO; i < EXFAT_MUST_BE_ZERO_END; i++) {
		if (sector[i] != 0) {
			return false;
		}
	}
	return raiju_read_le16(sector + EXFAT_BOOT_SIGNATURE) == 0xAA55;
}

raiju_image_report raiju_check_exfat(const uint8_t *head, size_t head_len, uint64_t image_size) {
	raiju_image_report report;
	memset(&report, 0, sizeof report);
	if (head == NULL || head_len < 512) {
		report.status = RAIJU_IMAGE_SHORT_READ;
		return report;
	}
	if (!exfat_boot_sector_sane(head)) {
		report.status = RAIJU_IMAGE_BAD_SIGNATURE;
		return report;
	}
	unsigned sector_shift = head[EXFAT_BYTES_PER_SECTOR_SHIFT];
	unsigned cluster_shift = head[EXFAT_SECTORS_PER_CLUSTER_SHIFT];
	unsigned fats = head[EXFAT_NUMBER_OF_FATS];
	if (sector_shift < EXFAT_MIN_SECTOR_SHIFT || sector_shift > EXFAT_MAX_SECTOR_SHIFT ||
	    cluster_shift > EXFAT_MAX_CLUSTER_SHIFT - sector_shift || (fats != 1 && fats != 2)) {
		report.status = RAIJU_IMAGE_BAD_GEOMETRY;
		return report;
	}
	uint32_t sector_size = 1u << sector_shift;
	report.sector_size = sector_size;
	report.cluster_size = 1u << (sector_shift + cluster_shift);
	report.warn_cluster_size = report.cluster_size != RAIJU_EXFAT_RECOMMENDED_CLUSTER;

	size_t checksummed = (size_t)EXFAT_CHECKSUMMED_SECTORS * sector_size;
	if (head_len < checksummed + sector_size) {
		report.status = RAIJU_IMAGE_SHORT_READ;
		return report;
	}
	uint32_t checksum = exfat_boot_checksum(head, checksummed);
	const uint8_t *checksum_sector = head + checksummed;
	for (size_t i = 0; i < sector_size; i += 4) {
		if (raiju_read_le32(checksum_sector + i) != checksum) {
			report.status = RAIJU_IMAGE_BAD_CHECKSUM;
			return report;
		}
	}

	uint64_t volume_sectors = raiju_read_le64(head + EXFAT_VOLUME_LENGTH);
	if (volume_sectors == 0) {
		report.status = RAIJU_IMAGE_BAD_GEOMETRY;
		return report;
	}
	if (volume_sectors > (image_size >> sector_shift)) {
		report.status = RAIJU_IMAGE_TRUNCATED;
		return report;
	}
	report.filesystem_bytes = volume_sectors << sector_shift;
	report.status = RAIJU_IMAGE_OK;
	return report;
}

raiju_image_report raiju_check_ufs2(const uint8_t *head, size_t head_len, uint64_t image_size) {
	raiju_image_report report;
	memset(&report, 0, sizeof report);
	if (head == NULL || head_len < RAIJU_PROBE_SIZE) {
		report.status = RAIJU_IMAGE_SHORT_READ;
		return report;
	}
	const uint8_t *superblock = head + RAIJU_UFS2_SUPERBLOCK_OFFSET;
	if (raiju_read_le32(superblock + UFS2_MAGIC) != RAIJU_UFS2_MAGIC) {
		report.status = RAIJU_IMAGE_BAD_SIGNATURE;
		return report;
	}
	uint32_t block_size = raiju_read_le32(superblock + UFS2_BLOCK_SIZE);
	uint32_t fragment_size = raiju_read_le32(superblock + UFS2_FRAGMENT_SIZE);
	uint32_t frags_per_block = raiju_read_le32(superblock + UFS2_FRAGS_PER_BLOCK);
	report.block_size = block_size;
	report.fragment_size = fragment_size;
	if (!is_power_of_two(block_size) || block_size < UFS2_MIN_BLOCK_SIZE || block_size > UFS2_MAX_BLOCK_SIZE ||
	    !is_power_of_two(fragment_size) || fragment_size < UFS2_MIN_FRAGMENT_SIZE || fragment_size > block_size ||
	    block_size / fragment_size > UFS2_MAX_FRAGS_PER_BLOCK || frags_per_block != block_size / fragment_size) {
		report.status = RAIJU_IMAGE_BAD_GEOMETRY;
		return report;
	}
	uint64_t fragments = raiju_read_le64(superblock + UFS2_SIZE);
	if (fragments == 0 || fragments > (uint64_t)INT64_MAX) {
		report.status = RAIJU_IMAGE_BAD_GEOMETRY;
		return report;
	}
	if (fragments > image_size / fragment_size) {
		report.status = RAIJU_IMAGE_TRUNCATED;
		return report;
	}
	report.filesystem_bytes = fragments * fragment_size;
	report.status = RAIJU_IMAGE_OK;
	return report;
}

const char *raiju_image_status_name(raiju_image_status status) {
	switch (status) {
	case RAIJU_IMAGE_OK:
		return "ok";
	case RAIJU_IMAGE_SHORT_READ:
		return "short_read";
	case RAIJU_IMAGE_BAD_SIGNATURE:
		return "bad_signature";
	case RAIJU_IMAGE_BAD_GEOMETRY:
		return "bad_geometry";
	case RAIJU_IMAGE_BAD_CHECKSUM:
		return "bad_checksum";
	case RAIJU_IMAGE_TRUNCATED:
		return "truncated";
	default:
		return "unknown";
	}
}
