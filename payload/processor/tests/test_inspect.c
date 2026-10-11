#include <stdlib.h>

#include "fixtures.h"
#include "raiju/inspect.h"
#include "test.h"

/**
 * Writes bytes to name inside a fresh temporary directory, inspects the file
 * and removes it again. Returns the raiju_inspect_file result.
 */
static int inspect_bytes(const char *name, const void *data, size_t length, raiju_inspection *out) {
	char *dir = fixture_temp_dir();
	if (dir == NULL) {
		return -2;
	}
	char *path = fixture_path(dir, name);
	int result = -2;
	if (path != NULL && fixture_write(path, data, length) == 0) {
		result = raiju_inspect_file(path, out);
	}
	if (path != NULL) {
		fixture_remove(path);
	}
	fixture_remove(dir);
	free(path);
	free(dir);
	return result;
}

static void valid_exfat_file_is_accepted(void) {
	size_t length = 256u * 512u;
	uint8_t *image = fixture_exfat(length, 9, 7, 256);
	raiju_inspection inspection;
	CHECK_INT(inspect_bytes("PPSA24003.exfat", image, length, &inspection), 0);
	CHECK_INT(inspection.classification.kind, RAIJU_KIND_EXFAT);
	CHECK_INT(inspection.file_size, length);
	CHECK(inspection.image_checked);
	CHECK_INT(inspection.image.status, RAIJU_IMAGE_OK);
	CHECK(!inspection.image.warn_cluster_size);
	CHECK(raiju_inspection_accepted(&inspection));
	free(image);
}

static void corrupted_exfat_file_is_refused(void) {
	size_t length = 256u * 512u;
	uint8_t *image = fixture_exfat(length, 9, 7, 256);
	image[5 * 512] = 0x42;
	raiju_inspection inspection;
	CHECK_INT(inspect_bytes("PPSA24003.exfat", image, length, &inspection), 0);
	CHECK_INT(inspection.image.status, RAIJU_IMAGE_BAD_CHECKSUM);
	CHECK(!raiju_inspection_accepted(&inspection));
	free(image);
}

static void html_page_saved_as_ffpkg_is_refused(void) {
	static const char page[] = "<html><body>Download limit reached</body></html>";
	raiju_inspection inspection;
	CHECK_INT(inspect_bytes("PPSA24003.ffpkg", page, sizeof page - 1, &inspection), 0);
	CHECK_INT(inspection.classification.kind, RAIJU_KIND_UFS2);
	CHECK(inspection.classification.header_mismatch);
	CHECK(!inspection.image_checked);
	CHECK(!raiju_inspection_accepted(&inspection));
}

static void valid_ffpkg_file_is_accepted(void) {
	size_t length = 32u * 4096u;
	uint8_t *image = fixture_ufs2(length, 32768, 4096, 32);
	raiju_inspection inspection;
	CHECK_INT(inspect_bytes("PPSA24003.ffpkg", image, length, &inspection), 0);
	CHECK_INT(inspection.classification.kind, RAIJU_KIND_UFS2);
	CHECK_INT(inspection.image.status, RAIJU_IMAGE_OK);
	CHECK(raiju_inspection_accepted(&inspection));
	free(image);
}

static void truncated_ffpkg_file_is_refused(void) {
	size_t length = 32u * 4096u;
	uint8_t *image = fixture_ufs2(length, 32768, 4096, 64);
	raiju_inspection inspection;
	CHECK_INT(inspect_bytes("PPSA24003.ffpkg", image, length, &inspection), 0);
	CHECK_INT(inspection.image.status, RAIJU_IMAGE_TRUNCATED);
	CHECK(!raiju_inspection_accepted(&inspection));
	free(image);
}

static void archive_file_is_accepted_without_image_check(void) {
	static const uint8_t sevenzip[] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x04, 0, 0, 0, 0};
	raiju_inspection inspection;
	CHECK_INT(inspect_bytes("PPSA24003-some-game-name.7z", sevenzip, sizeof sevenzip, &inspection), 0);
	CHECK_INT(inspection.classification.kind, RAIJU_KIND_7Z);
	CHECK_INT(inspection.classification.category, RAIJU_CATEGORY_ARCHIVE);
	CHECK(!inspection.image_checked);
	CHECK(raiju_inspection_accepted(&inspection));
}

static void pfs_file_is_accepted_without_header_check(void) {
	static const uint8_t bytes[] = {1, 2, 3, 4};
	raiju_inspection inspection;
	CHECK_INT(inspect_bytes("PPSA24003.ffpfsc", bytes, sizeof bytes, &inspection), 0);
	CHECK_INT(inspection.classification.kind, RAIJU_KIND_PFSC);
	CHECK(!inspection.classification.header_checked);
	CHECK(raiju_inspection_accepted(&inspection));
}

static void empty_file_is_other(void) {
	raiju_inspection inspection;
	CHECK_INT(inspect_bytes("empty.bin", "", 0, &inspection), 0);
	CHECK_INT(inspection.classification.category, RAIJU_CATEGORY_OTHER);
	CHECK_INT(inspection.file_size, 0);
}

static void missing_file_is_an_error(void) {
	raiju_inspection inspection;
	CHECK_INT(raiju_inspect_file("/nonexistent/raiju/PPSA24003.exfat", &inspection), -1);
	CHECK_INT(raiju_inspect_file(NULL, &inspection), -1);
	CHECK(!raiju_inspection_accepted(NULL));
}

void run_inspect_tests(void) {
	RUN_TEST(valid_exfat_file_is_accepted);
	RUN_TEST(corrupted_exfat_file_is_refused);
	RUN_TEST(html_page_saved_as_ffpkg_is_refused);
	RUN_TEST(valid_ffpkg_file_is_accepted);
	RUN_TEST(truncated_ffpkg_file_is_refused);
	RUN_TEST(archive_file_is_accepted_without_image_check);
	RUN_TEST(pfs_file_is_accepted_without_header_check);
	RUN_TEST(empty_file_is_other);
	RUN_TEST(missing_file_is_an_error);
}
