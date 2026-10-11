#include <stdlib.h>

#include "fixtures.h"
#include "raiju/classify.h"
#include "test.h"

static const char html_page[] = "<!DOCTYPE html>\n<html><head><title>404 Not Found</title></head>"
                                "<body><h1>Not Found</h1></body></html>\n";

/**
 * Classifies a short byte signature under a neutral file name.
 */
static raiju_classification classify_bytes(const uint8_t *bytes, size_t length) {
	return raiju_classify(bytes, length, "download.bin");
}

static void archive_signatures_are_detected(void) {
	static const uint8_t sevenzip[] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x04};
	static const uint8_t zip[] = {0x50, 0x4B, 0x03, 0x04, 0x14, 0x00};
	static const uint8_t rar[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00};
	static const uint8_t zstd[] = {0x28, 0xB5, 0x2F, 0xFD, 0x24, 0x00};
	static const uint8_t xz[] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00, 0x00, 0x04};
	static const uint8_t gzip[] = {0x1F, 0x8B, 0x08, 0x00};
	struct {
		const uint8_t *bytes;
		size_t length;
		raiju_kind kind;
	} cases[] = {
		{sevenzip, sizeof sevenzip, RAIJU_KIND_7Z}, {zip, sizeof zip, RAIJU_KIND_ZIP},
		{rar, sizeof rar, RAIJU_KIND_RAR},          {zstd, sizeof zstd, RAIJU_KIND_ZSTD},
		{xz, sizeof xz, RAIJU_KIND_XZ},             {gzip, sizeof gzip, RAIJU_KIND_GZIP},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		raiju_classification result = classify_bytes(cases[i].bytes, cases[i].length);
		CHECK_INT(result.kind, cases[i].kind);
		CHECK_INT(result.category, RAIJU_CATEGORY_ARCHIVE);
		CHECK(!result.header_mismatch);
	}
}

static void archive_extension_does_not_override_magic(void) {
	static const uint8_t zip[] = {0x50, 0x4B, 0x03, 0x04};
	raiju_classification result = raiju_classify(zip, sizeof zip, "PPSA24003-some-game-name.7z");
	CHECK_INT(result.kind, RAIJU_KIND_ZIP);
	CHECK_INT(result.category, RAIJU_CATEGORY_ARCHIVE);
}

static void truncated_signatures_are_unknown(void) {
	static const uint8_t partial_7z[] = {0x37, 0x7A, 0xBC};
	static const uint8_t one_byte_gzip[] = {0x1F};
	CHECK_INT(raiju_detect_magic(partial_7z, sizeof partial_7z), RAIJU_KIND_UNKNOWN);
	CHECK_INT(raiju_detect_magic(one_byte_gzip, sizeof one_byte_gzip), RAIJU_KIND_UNKNOWN);
	CHECK_INT(raiju_detect_magic(NULL, 0), RAIJU_KIND_UNKNOWN);
}

static void exfat_image_is_detected(void) {
	uint8_t *image = fixture_exfat(12 * 512, 9, 7, 12);
	raiju_classification result = raiju_classify(image, 12 * 512, "PPSA24003.exfat");
	CHECK_INT(result.kind, RAIJU_KIND_EXFAT);
	CHECK_INT(result.category, RAIJU_CATEGORY_IMAGE);
	CHECK(!result.header_mismatch);
	CHECK(result.header_checked);
	free(image);
}

static void ufs2_image_is_detected(void) {
	uint8_t *image = fixture_ufs2(RAIJU_PROBE_SIZE, 32768, 4096, 16);
	raiju_classification named = raiju_classify(image, RAIJU_PROBE_SIZE, "PPSA24003.ffpkg");
	CHECK_INT(named.kind, RAIJU_KIND_UFS2);
	CHECK_INT(named.category, RAIJU_CATEGORY_IMAGE);
	CHECK(!named.header_mismatch);
	raiju_classification unnamed = raiju_classify(image, RAIJU_PROBE_SIZE, "dump.img");
	CHECK_INT(unnamed.kind, RAIJU_KIND_UFS2);
	CHECK_INT(unnamed.category, RAIJU_CATEGORY_IMAGE);
	CHECK_INT(raiju_detect_magic(image, RAIJU_UFS2_MAGIC_OFFSET + 3), RAIJU_KIND_UNKNOWN);
	free(image);
}

static void html_page_named_ffpkg_is_rejected(void) {
	raiju_classification result = raiju_classify((const uint8_t *)html_page, sizeof html_page - 1, "PPSA24003.ffpkg");
	CHECK_INT(result.kind, RAIJU_KIND_UFS2);
	CHECK_INT(result.category, RAIJU_CATEGORY_IMAGE);
	CHECK(result.header_mismatch);
}

static void html_page_named_exfat_is_rejected(void) {
	raiju_classification result = raiju_classify((const uint8_t *)html_page, sizeof html_page - 1, "game.exfat");
	CHECK_INT(result.kind, RAIJU_KIND_EXFAT);
	CHECK(result.header_mismatch);
}

static void image_extension_needs_matching_image_magic(void) {
	static const uint8_t zip[] = {0x50, 0x4B, 0x03, 0x04};
	uint8_t *exfat = fixture_exfat(12 * 512, 9, 7, 12);
	CHECK(raiju_classify(zip, sizeof zip, "Game.EXFAT").header_mismatch);
	CHECK(raiju_classify(exfat, 12 * 512, "Game.ffpkg").header_mismatch);
	CHECK(!raiju_classify(exfat, 12 * 512, "Game.ExFat").header_mismatch);
	free(exfat);
}

static void pfs_images_are_classified_by_extension(void) {
	raiju_classification pfs = raiju_classify((const uint8_t *)html_page, sizeof html_page - 1, "PPSA24003.ffpfs");
	CHECK_INT(pfs.kind, RAIJU_KIND_PFS);
	CHECK_INT(pfs.category, RAIJU_CATEGORY_IMAGE);
	CHECK(!pfs.header_checked);
	CHECK(!pfs.header_mismatch);
	raiju_classification pfsc = raiju_classify(NULL, 0, "PPSA24003.FFPFSC");
	CHECK_INT(pfsc.kind, RAIJU_KIND_PFSC);
	CHECK_INT(pfsc.category, RAIJU_CATEGORY_IMAGE);
}

static void unrecognised_files_are_other(void) {
	static const uint8_t elf[] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
	raiju_classification result = raiju_classify(elf, sizeof elf, "homebrew.elf");
	CHECK_INT(result.kind, RAIJU_KIND_UNKNOWN);
	CHECK_INT(result.category, RAIJU_CATEGORY_OTHER);
	CHECK(!result.header_mismatch);
	CHECK_INT(raiju_classify(NULL, 0, NULL).category, RAIJU_CATEGORY_OTHER);
}

static void extensions_map_to_image_kinds(void) {
	CHECK_INT(raiju_image_kind_from_name("a.exfat"), RAIJU_KIND_EXFAT);
	CHECK_INT(raiju_image_kind_from_name("a.FFPKG"), RAIJU_KIND_UFS2);
	CHECK_INT(raiju_image_kind_from_name("a.ffpfs"), RAIJU_KIND_PFS);
	CHECK_INT(raiju_image_kind_from_name("a.ffpfsc"), RAIJU_KIND_PFSC);
	CHECK_INT(raiju_image_kind_from_name("a.ffpkg.part"), RAIJU_KIND_UNKNOWN);
	CHECK_INT(raiju_image_kind_from_name("exfat"), RAIJU_KIND_UNKNOWN);
	CHECK_INT(raiju_image_kind_from_name(""), RAIJU_KIND_UNKNOWN);
}

static void kind_names_are_stable(void) {
	CHECK_STR(raiju_kind_name(RAIJU_KIND_7Z), "7z");
	CHECK_STR(raiju_kind_name(RAIJU_KIND_UFS2), "ufs2");
	CHECK_STR(raiju_kind_name(RAIJU_KIND_UNKNOWN), "unknown");
}

static void ufs2_superblock_is_found_at_every_freebsd_location(void) {
	static const uint32_t locations[] = {65536u, 8192u, 0u, 262144u};
	for (size_t i = 0; i < sizeof locations / sizeof locations[0]; i++) {
		uint8_t *image = calloc(1, RAIJU_PROBE_SIZE);
		CHECK(image != NULL);
		fixture_put_ufs2_superblock(image, locations[i], locations[i], 32768, 4096, 16);
		CHECK_INT(raiju_find_ufs2_superblock(image, RAIJU_PROBE_SIZE), locations[i]);
		CHECK_INT(raiju_detect_magic(image, RAIJU_PROBE_SIZE), RAIJU_KIND_UFS2);
		free(image);
	}
}

static void ufs2_backup_copy_points_to_primary(void) {
	uint8_t *image = calloc(1, RAIJU_PROBE_SIZE);
	CHECK(image != NULL);
	fixture_put_ufs2_superblock(image, 65536, 8192, 65536, 65536, 16);
	fixture_put_ufs2_superblock(image, 8192, 8192, 65536, 65536, 16);
	CHECK_INT(raiju_find_ufs2_superblock(image, RAIJU_PROBE_SIZE), 8192);
	free(image);
}

static void ufs2_stray_copy_alone_is_not_an_image(void) {
	uint8_t *image = calloc(1, RAIJU_PROBE_SIZE);
	CHECK(image != NULL);
	fixture_put_ufs2_superblock(image, 65536, 8192, 32768, 4096, 16);
	CHECK_INT(raiju_find_ufs2_superblock(image, RAIJU_PROBE_SIZE), RAIJU_UFS2_NOT_FOUND);
	CHECK_INT(raiju_detect_magic(image, RAIJU_PROBE_SIZE), RAIJU_KIND_UNKNOWN);
	free(image);
}

static void ufs2_search_respects_buffer_length(void) {
	uint8_t *image = calloc(1, RAIJU_PROBE_SIZE);
	CHECK(image != NULL);
	fixture_put_ufs2_superblock(image, 262144, 262144, 32768, 4096, 16);
	CHECK_INT(raiju_find_ufs2_superblock(image, RAIJU_PROBE_SIZE - 1), RAIJU_UFS2_NOT_FOUND);
	CHECK(!raiju_ufs2_superblock_at(NULL, RAIJU_PROBE_SIZE, 0));
	free(image);
}

static void ffpkg_is_reported_as_unsupported(void) {
	CHECK(raiju_image_unsupported_reason(RAIJU_KIND_UFS2) != NULL);
	CHECK(raiju_image_unsupported_reason(RAIJU_KIND_EXFAT) == NULL);
	CHECK(raiju_image_unsupported_reason(RAIJU_KIND_PFS) == NULL);
	CHECK(raiju_image_unsupported_reason(RAIJU_KIND_PFSC) == NULL);
	CHECK(raiju_image_unsupported_reason(RAIJU_KIND_7Z) == NULL);
}

void run_classify_tests(void) {
	RUN_TEST(ufs2_superblock_is_found_at_every_freebsd_location);
	RUN_TEST(ufs2_backup_copy_points_to_primary);
	RUN_TEST(ufs2_stray_copy_alone_is_not_an_image);
	RUN_TEST(ufs2_search_respects_buffer_length);
	RUN_TEST(archive_signatures_are_detected);
	RUN_TEST(archive_extension_does_not_override_magic);
	RUN_TEST(truncated_signatures_are_unknown);
	RUN_TEST(exfat_image_is_detected);
	RUN_TEST(ufs2_image_is_detected);
	RUN_TEST(html_page_named_ffpkg_is_rejected);
	RUN_TEST(html_page_named_exfat_is_rejected);
	RUN_TEST(image_extension_needs_matching_image_magic);
	RUN_TEST(pfs_images_are_classified_by_extension);
	RUN_TEST(unrecognised_files_are_other);
	RUN_TEST(extensions_map_to_image_kinds);
	RUN_TEST(kind_names_are_stable);
	RUN_TEST(ffpkg_is_reported_as_unsupported);
}
