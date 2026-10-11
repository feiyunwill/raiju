#include <stdlib.h>

#include "fixtures.h"
#include "raiju/classify.h"
#include "raiju/image_check.h"
#include "test.h"

#define BOOT_REGION_512 (12u * 512u)
#define VOLUME_SECTORS 256u
#define VOLUME_BYTES (VOLUME_SECTORS * 512u)

static void exfat_with_64k_clusters_passes(void) {
	uint8_t *image = fixture_exfat(BOOT_REGION_512, 9, 7, VOLUME_SECTORS);
	raiju_image_report report = raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES);
	CHECK_INT(report.status, RAIJU_IMAGE_OK);
	CHECK_INT(report.sector_size, 512);
	CHECK_INT(report.cluster_size, 65536);
	CHECK_INT(report.filesystem_bytes, VOLUME_BYTES);
	CHECK(!report.warn_cluster_size);
	free(image);
}

static void exfat_with_4k_sectors_passes(void) {
	size_t length = 12u * 4096u;
	uint8_t *image = fixture_exfat(length, 12, 4, 64);
	raiju_image_report report = raiju_check_exfat(image, length, 64u * 4096u);
	CHECK_INT(report.status, RAIJU_IMAGE_OK);
	CHECK_INT(report.sector_size, 4096);
	CHECK_INT(report.cluster_size, 65536);
	free(image);
}

static void exfat_small_clusters_warn_but_pass(void) {
	uint8_t *image = fixture_exfat(BOOT_REGION_512, 9, 3, VOLUME_SECTORS);
	raiju_image_report report = raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES);
	CHECK_INT(report.status, RAIJU_IMAGE_OK);
	CHECK_INT(report.cluster_size, 4096);
	CHECK(report.warn_cluster_size);
	free(image);
}

static void exfat_corrupted_boot_region_fails_checksum(void) {
	uint8_t *image = fixture_exfat(BOOT_REGION_512, 9, 7, VOLUME_SECTORS);
	image[3 * 512 + 17] ^= 0x01;
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_BAD_CHECKSUM);
	image[3 * 512 + 17] ^= 0x01;
	image[100] ^= 0xFF;
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_BAD_CHECKSUM);
	free(image);
}

static void exfat_checksum_sector_must_repeat(void) {
	uint8_t *image = fixture_exfat(BOOT_REGION_512, 9, 7, VOLUME_SECTORS);
	image[11 * 512 + 508] ^= 0x80;
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_BAD_CHECKSUM);
	free(image);
}

static void exfat_checksum_skips_volume_flags_and_percent_in_use(void) {
	uint8_t *image = fixture_exfat(BOOT_REGION_512, 9, 7, VOLUME_SECTORS);
	image[106] = 0x02;
	image[107] = 0x00;
	image[112] = 42;
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_OK);
	free(image);
}

static void exfat_bad_boot_sector_fails_signature(void) {
	uint8_t *image = fixture_exfat(BOOT_REGION_512, 9, 7, VOLUME_SECTORS);
	image[0] = 0xE9;
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_BAD_SIGNATURE);
	image[0] = 0xEB;
	image[511] = 0x00;
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_BAD_SIGNATURE);
	image[511] = 0xAA;
	image[20] = 0x01;
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_BAD_SIGNATURE);
	free(image);
}

static void exfat_impossible_geometry_fails(void) {
	uint8_t *image = fixture_exfat(BOOT_REGION_512, 9, 7, VOLUME_SECTORS);
	image[108] = 8;
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_BAD_GEOMETRY);
	image[108] = 13;
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_BAD_GEOMETRY);
	image[108] = 9;
	image[109] = 17;
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_BAD_GEOMETRY);
	image[109] = 7;
	image[110] = 3;
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_BAD_GEOMETRY);
	image[110] = 1;
	fixture_put_le64(image + 72, 0);
	fixture_exfat_reseal(image, 9);
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_BAD_GEOMETRY);
	free(image);
}

static void exfat_volume_larger_than_image_is_truncated(void) {
	uint8_t *image = fixture_exfat(BOOT_REGION_512, 9, 7, VOLUME_SECTORS);
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES - 1).status, RAIJU_IMAGE_TRUNCATED);
	fixture_put_le64(image + 72, UINT64_MAX);
	fixture_exfat_reseal(image, 9);
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512, VOLUME_BYTES).status, RAIJU_IMAGE_TRUNCATED);
	free(image);
}

static void exfat_short_boot_region_is_short_read(void) {
	uint8_t *image = fixture_exfat(BOOT_REGION_512, 9, 7, VOLUME_SECTORS);
	CHECK_INT(raiju_check_exfat(image, 511, VOLUME_BYTES).status, RAIJU_IMAGE_SHORT_READ);
	CHECK_INT(raiju_check_exfat(image, BOOT_REGION_512 - 1, VOLUME_BYTES).status, RAIJU_IMAGE_SHORT_READ);
	CHECK_INT(raiju_check_exfat(NULL, 0, VOLUME_BYTES).status, RAIJU_IMAGE_SHORT_READ);
	free(image);
}

static void ufs2_sane_superblock_passes(void) {
	uint8_t *image = fixture_ufs2(RAIJU_PROBE_SIZE, 32768, 4096, 64);
	raiju_image_report report = raiju_check_ufs2(image, RAIJU_PROBE_SIZE, 64u * 4096u);
	CHECK_INT(report.status, RAIJU_IMAGE_OK);
	CHECK_INT(report.block_size, 32768);
	CHECK_INT(report.fragment_size, 4096);
	CHECK_INT(report.filesystem_bytes, 64u * 4096u);
	free(image);
}

static void ufs2_implausible_sizes_fail(void) {
	struct {
		uint32_t block_size;
		uint32_t fragment_size;
	} cases[] = {
		{3000, 512}, {2048, 512}, {131072, 16384}, {32768, 1000}, {4096, 8192}, {65536, 512}, {32768, 256},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		uint8_t *image = fixture_ufs2(RAIJU_PROBE_SIZE, cases[i].block_size, cases[i].fragment_size, 16);
		CHECK_INT(raiju_check_ufs2(image, RAIJU_PROBE_SIZE, UINT64_MAX).status, RAIJU_IMAGE_BAD_GEOMETRY);
		free(image);
	}
}

static void ufs2_inconsistent_frag_count_fails(void) {
	uint8_t *image = fixture_ufs2(RAIJU_PROBE_SIZE, 32768, 4096, 16);
	fixture_put_le32(image + 65536 + 56, 4);
	CHECK_INT(raiju_check_ufs2(image, RAIJU_PROBE_SIZE, UINT64_MAX).status, RAIJU_IMAGE_BAD_GEOMETRY);
	free(image);
}

static void ufs2_empty_or_negative_size_fails(void) {
	uint8_t *image = fixture_ufs2(RAIJU_PROBE_SIZE, 32768, 4096, 0);
	CHECK_INT(raiju_check_ufs2(image, RAIJU_PROBE_SIZE, UINT64_MAX).status, RAIJU_IMAGE_BAD_GEOMETRY);
	fixture_put_le64(image + 65536 + 1080, UINT64_MAX);
	CHECK_INT(raiju_check_ufs2(image, RAIJU_PROBE_SIZE, UINT64_MAX).status, RAIJU_IMAGE_BAD_GEOMETRY);
	free(image);
}

static void ufs2_filesystem_larger_than_image_is_truncated(void) {
	uint8_t *image = fixture_ufs2(RAIJU_PROBE_SIZE, 32768, 4096, 64);
	CHECK_INT(raiju_check_ufs2(image, RAIJU_PROBE_SIZE, 64u * 4096u - 1).status, RAIJU_IMAGE_TRUNCATED);
	free(image);
}

static void ufs2_wrong_magic_or_short_head_fails(void) {
	uint8_t *image = fixture_ufs2(RAIJU_PROBE_SIZE, 32768, 4096, 64);
	CHECK_INT(raiju_check_ufs2(image, RAIJU_PROBE_SIZE - 1, UINT64_MAX).status, RAIJU_IMAGE_SHORT_READ);
	image[65536 + 1372] ^= 0x01;
	CHECK_INT(raiju_check_ufs2(image, RAIJU_PROBE_SIZE, UINT64_MAX).status, RAIJU_IMAGE_BAD_SIGNATURE);
	free(image);
}

static void status_names_are_stable(void) {
	CHECK_STR(raiju_image_status_name(RAIJU_IMAGE_OK), "ok");
	CHECK_STR(raiju_image_status_name(RAIJU_IMAGE_BAD_CHECKSUM), "bad_checksum");
	CHECK_STR(raiju_image_status_name(RAIJU_IMAGE_TRUNCATED), "truncated");
}

void run_image_check_tests(void) {
	RUN_TEST(exfat_with_64k_clusters_passes);
	RUN_TEST(exfat_with_4k_sectors_passes);
	RUN_TEST(exfat_small_clusters_warn_but_pass);
	RUN_TEST(exfat_corrupted_boot_region_fails_checksum);
	RUN_TEST(exfat_checksum_sector_must_repeat);
	RUN_TEST(exfat_checksum_skips_volume_flags_and_percent_in_use);
	RUN_TEST(exfat_bad_boot_sector_fails_signature);
	RUN_TEST(exfat_impossible_geometry_fails);
	RUN_TEST(exfat_volume_larger_than_image_is_truncated);
	RUN_TEST(exfat_short_boot_region_is_short_read);
	RUN_TEST(ufs2_sane_superblock_passes);
	RUN_TEST(ufs2_implausible_sizes_fail);
	RUN_TEST(ufs2_inconsistent_frag_count_fails);
	RUN_TEST(ufs2_empty_or_negative_size_fails);
	RUN_TEST(ufs2_filesystem_larger_than_image_is_truncated);
	RUN_TEST(ufs2_wrong_magic_or_short_head_fails);
	RUN_TEST(status_names_are_stable);
}
