#include <stdlib.h>
#include <string.h>

#include "fixtures.h"
#include "image_builder.h"
#include "raiju/image_fs.h"
#include "raiju/inspect.h"
#include "raiju/title_id.h"
#include "test.h"

#define PARAM_JSON "{\"titleId\":\"PPSA24003\",\"localizedParameters\":{\"en-US\":{\"titleName\":\"Raiju Test\"}}}"
#define EXFAT_ENTRY 32u
#define DIRBLKSIZ 512u

/**
 * Builds a param.json document padded with a long string so that it spans
 * many clusters or blocks.
 */
static char *long_param_json(size_t padding) {
	const char *head = "{\"titleId\":\"PPSA24003\",\"padding\":\"";
	const char *tail = "\"}";
	size_t length = strlen(head) + padding + strlen(tail);
	char *json = malloc(length + 1);
	if (json == NULL) {
		return NULL;
	}
	strcpy(json, head);
	memset(json + strlen(head), 'x', padding);
	strcpy(json + strlen(head) + padding, tail);
	return json;
}

/**
 * Runs the exFAT reader over an in-memory volume, keeping the result bytes.
 */
static raiju_image_fs_status read_exfat(const builder_exfat *volume, size_t image_size, uint8_t **out,
                                        size_t *out_len) {
	raiju_image_io io = builder_memory_io(volume->data, image_size);
	return raiju_exfat_read_param_json(&io, out, out_len);
}

/**
 * Runs the exFAT reader and only reports its status.
 */
static raiju_image_fs_status exfat_status(const builder_exfat *volume) {
	uint8_t *json = NULL;
	size_t length = 0;
	raiju_image_fs_status status = read_exfat(volume, volume->size, &json, &length);
	free(json);
	return status;
}

/**
 * Returns the param.json inode number builder_ufs2_game chose.
 */
static uint32_t param_inode(const builder_ufs2 *fs) {
	return fs->cylinder_groups > 1 ? (fs->cylinder_groups - 1) * fs->inodes_per_group + 1 : 5;
}

/**
 * Runs the UFS2 reader over an in-memory filesystem, keeping the bytes.
 */
static raiju_image_fs_status read_ufs2(const builder_ufs2 *fs, size_t image_size, uint8_t **out, size_t *out_len) {
	raiju_image_io io = builder_memory_io(fs->data, image_size);
	return raiju_ufs2_read_param_json(&io, out, out_len);
}

/**
 * Runs the UFS2 reader and only reports its status.
 */
static raiju_image_fs_status ufs2_status(const builder_ufs2 *fs) {
	uint8_t *json = NULL;
	size_t length = 0;
	raiju_image_fs_status status = read_ufs2(fs, fs->size, &json, &length);
	free(json);
	return status;
}

/**
 * Returns the cluster holding the start of param.json in a game volume, found
 * through the reader-independent layout builder_exfat_game uses.
 */
static uint8_t *exfat_param_stream_entry(builder_exfat *volume) {
	uint32_t sce_sys = volume->root_cluster + 2;
	return builder_exfat_cluster(volume, sce_sys) + volume->cluster_size - 2 * EXFAT_ENTRY + EXFAT_ENTRY;
}

/**
 * Recomputes the checksum of the param.json entry set in a game volume after
 * a test edited it. The set straddles the two sce_sys clusters.
 */
static void exfat_reseal_param_set(builder_exfat *volume) {
	uint32_t sce_sys = volume->root_cluster + 2;
	uint8_t set[3 * EXFAT_ENTRY];
	size_t start = volume->cluster_size - 2 * EXFAT_ENTRY;
	for (size_t i = 0; i < sizeof set; i++) {
		size_t position = start + i;
		set[i] = builder_exfat_cluster(volume, sce_sys + (uint32_t)(position / volume->cluster_size))
		    [position % volume->cluster_size];
	}
	uint16_t checksum = 0;
	for (size_t i = 0; i < sizeof set; i++) {
		if (i != 2 && i != 3) {
			checksum = (uint16_t)(((checksum & 1u) ? 0x8000u : 0u) + (checksum >> 1) + set[i]);
		}
	}
	uint8_t *primary = builder_exfat_cluster(volume, sce_sys) + start;
	primary[2] = (uint8_t)checksum;
	primary[3] = (uint8_t)(checksum >> 8);
}

static void exfat_reads_param_json_across_chained_clusters(void) {
	char *json = long_param_json(1500);
	builder_exfat *volume = builder_exfat_game("sce_sys", "param.json", json);
	uint8_t *out = NULL;
	size_t length = 0;
	CHECK_INT(read_exfat(volume, volume->size, &out, &length), RAIJU_IMAGE_FS_OK);
	CHECK_INT(length, strlen(json));
	CHECK(out != NULL && memcmp(out, json, strlen(json)) == 0);
	char title_id[RAIJU_TITLE_ID_LEN + 1];
	CHECK_INT(raiju_title_id_from_param_json((const char *)out, length, title_id), RAIJU_PARAM_OK);
	CHECK_STR(title_id, "PPSA24003");
	free(out);
	builder_exfat_free(volume);
	free(json);
}

static void exfat_matches_names_case_insensitively(void) {
	builder_exfat *volume = builder_exfat_game("SCE_SYS", "Param.JSON", PARAM_JSON);
	uint8_t *out = NULL;
	size_t length = 0;
	CHECK_INT(read_exfat(volume, volume->size, &out, &length), RAIJU_IMAGE_FS_OK);
	CHECK_INT(length, strlen(PARAM_JSON));
	free(out);
	builder_exfat_free(volume);
}

static void exfat_zero_fills_past_valid_data_length(void) {
	builder_exfat *volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	uint8_t *stream = exfat_param_stream_entry(volume);
	fixture_put_le64(stream + 8, 4);
	exfat_reseal_param_set(volume);
	uint8_t *out = NULL;
	size_t length = 0;
	CHECK_INT(read_exfat(volume, volume->size, &out, &length), RAIJU_IMAGE_FS_OK);
	CHECK_INT(length, strlen(PARAM_JSON));
	CHECK(out != NULL && memcmp(out, PARAM_JSON, 4) == 0 && out[4] == 0 && out[length - 1] == 0);
	free(out);
	builder_exfat_free(volume);
}

static void exfat_follows_root_chain_and_second_fat(void) {
	builder_exfat *volume = builder_exfat_create(0, 64);
	uint32_t root[2] = {volume->root_cluster, builder_exfat_alloc(volume, 1, false)};
	builder_exfat_set_fat(volume, root[0], root[1]);
	builder_exfat_set_fat(volume, root[1], BUILDER_EXFAT_END_OF_CHAIN);
	for (size_t offset = 2 * EXFAT_ENTRY; offset < volume->cluster_size; offset += EXFAT_ENTRY) {
		builder_exfat_cluster(volume, root[0])[offset] = 0x05;
	}
	uint32_t sce_sys = builder_exfat_alloc(volume, 1, true);
	uint32_t param = builder_exfat_alloc(volume, 1, true);
	memcpy(builder_exfat_cluster(volume, param), PARAM_JSON, strlen(PARAM_JSON));
	builder_exfat_entry dir = {"sce_sys", BUILDER_EXFAT_ATTR_DIRECTORY, BUILDER_EXFAT_ALLOCATED, sce_sys,
	                           volume->cluster_size, volume->cluster_size};
	builder_exfat_entry file = {"param.json", 0, BUILDER_EXFAT_ALLOCATED, param, strlen(PARAM_JSON),
	                            strlen(PARAM_JSON)};
	builder_exfat_write_entry(volume, root, 2, volume->cluster_size - EXFAT_ENTRY, &dir);
	builder_exfat_write_entry(volume, &sce_sys, 1, 0, &file);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_OK);

	size_t fat_bytes = (size_t)(volume->heap_offset - volume->fat_offset);
	uint8_t *grown = realloc(volume->data, volume->size + fat_bytes);
	CHECK(grown != NULL);
	if (grown != NULL) {
		volume->data = grown;
		memmove(grown + volume->heap_offset + fat_bytes, grown + volume->heap_offset,
		        volume->size - volume->heap_offset);
		memcpy(grown + volume->heap_offset, grown + volume->fat_offset, fat_bytes);
		memset(grown + volume->fat_offset, 0, fat_bytes);
		volume->size += fat_bytes;
		uint32_t heap_sector = (uint32_t)((volume->heap_offset + fat_bytes) / 512);
		fixture_put_le32(grown + 84, (uint32_t)(fat_bytes / 512));
		fixture_put_le32(grown + 88, heap_sector);
		fixture_put_le64(grown + 72, volume->size / 512);
		grown[110] = 2;
		CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
		grown[106] |= 0x01;
		CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_OK);
	}
	builder_exfat_free(volume);
}

static void exfat_missing_entries_are_not_found(void) {
	builder_exfat *volume = builder_exfat_game("sce_sys", "param.jsonx", PARAM_JSON);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_NOT_FOUND);
	builder_exfat_free(volume);
	volume = builder_exfat_game("sce", "param.json", PARAM_JSON);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_NOT_FOUND);
	builder_exfat_free(volume);
	volume = builder_exfat_create(0, 16);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_NOT_FOUND);
	builder_exfat_free(volume);
}

static void exfat_sce_sys_file_is_not_a_directory(void) {
	builder_exfat *volume = builder_exfat_create(0, 16);
	uint32_t root = volume->root_cluster;
	uint32_t data = builder_exfat_alloc(volume, 1, true);
	builder_exfat_entry file = {"sce_sys", 0x20, BUILDER_EXFAT_ALLOCATED, data, 10, 10};
	builder_exfat_write_entry(volume, &root, 1, 2 * EXFAT_ENTRY, &file);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_NOT_FOUND);
	builder_exfat_free(volume);
}

static void exfat_cyclic_root_chain_fails(void) {
	builder_exfat *volume = builder_exfat_create(0, 16);
	uint32_t second = builder_exfat_alloc(volume, 1, false);
	for (size_t offset = 2 * EXFAT_ENTRY; offset < volume->cluster_size; offset += EXFAT_ENTRY) {
		builder_exfat_cluster(volume, volume->root_cluster)[offset] = 0x05;
		builder_exfat_cluster(volume, second)[offset - 2 * EXFAT_ENTRY] = 0x05;
		builder_exfat_cluster(volume, second)[offset] = 0x05;
	}
	builder_exfat_set_fat(volume, volume->root_cluster, second);
	builder_exfat_set_fat(volume, second, volume->root_cluster);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_set_fat(volume, second, second);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_free(volume);
}

static void exfat_cyclic_file_chain_fails(void) {
	char *json = long_param_json(1500);
	builder_exfat *volume = builder_exfat_game("sce_sys", "param.json", json);
	uint32_t first = (uint32_t)(exfat_param_stream_entry(volume)[20] | (exfat_param_stream_entry(volume)[21] << 8));
	builder_exfat_set_fat(volume, first, first);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_set_fat(volume, first, BUILDER_EXFAT_END_OF_CHAIN);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_set_fat(volume, first, 0xFFFFFFF7u);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_free(volume);
	free(json);
}

static void exfat_out_of_range_clusters_fail(void) {
	builder_exfat *volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	uint8_t *stream = exfat_param_stream_entry(volume);
	fixture_put_le32(stream + 20, volume->cluster_count + 2);
	exfat_reseal_param_set(volume);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le32(stream + 20, volume->cluster_count + 1);
	stream[1] = BUILDER_EXFAT_NO_FAT_CHAIN;
	fixture_put_le64(stream + 24, 2u * volume->cluster_size);
	fixture_put_le64(stream + 8, 2u * volume->cluster_size);
	exfat_reseal_param_set(volume);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le64(stream + 8, 4u * volume->cluster_size);
	exfat_reseal_param_set(volume);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_free(volume);

	volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	builder_exfat_set_fat(volume, volume->root_cluster, volume->cluster_count + 5);
	fixture_put_le32(volume->data + 96, 1);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_free(volume);
}

static void exfat_bad_boot_geometry_fails(void) {
	builder_exfat *volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	fixture_put_le32(volume->data + 88, 0xFFFFFF00u);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le32(volume->data + 88, 20);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_free(volume);

	volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	fixture_put_le32(volume->data + 84, 0);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_free(volume);

	volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	fixture_put_le32(volume->data + 92, 0xFFFFFFF0u);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le32(volume->data + 92, 64);
	volume->data[109] = 20;
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	volume->data[109] = 0;
	fixture_put_le32(volume->data + 80, 2);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_free(volume);
}

static void exfat_bad_entry_sets_fail(void) {
	builder_exfat *volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	uint8_t *stream = exfat_param_stream_entry(volume);
	stream[24] ^= 0x01;
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	stream[24] ^= 0x01;
	stream[3] = 40;
	exfat_reseal_param_set(volume);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	stream[3] = 10;
	stream[1] = 0;
	exfat_reseal_param_set(volume);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_free(volume);

	volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	builder_exfat_cluster(volume, volume->root_cluster)[2 * EXFAT_ENTRY + 1] = 1;
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_CORRUPT);
	builder_exfat_free(volume);
}

static void exfat_oversized_param_json_is_too_large(void) {
	builder_exfat *volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	uint8_t *stream = exfat_param_stream_entry(volume);
	fixture_put_le64(stream + 24, RAIJU_PARAM_JSON_MAX_BYTES + 1);
	fixture_put_le64(stream + 8, RAIJU_PARAM_JSON_MAX_BYTES + 1);
	exfat_reseal_param_set(volume);
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_TOO_LARGE);
	builder_exfat_free(volume);
}

static void exfat_truncated_or_foreign_image_fails(void) {
	builder_exfat *volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	uint8_t *out = NULL;
	size_t length = 0;
	CHECK_INT(read_exfat(volume, (size_t)volume->heap_offset + 512, &out, &length), RAIJU_IMAGE_FS_CORRUPT);
	CHECK(out == NULL);
	CHECK_INT(read_exfat(volume, 100, &out, &length), RAIJU_IMAGE_FS_UNSUPPORTED);
	volume->data[3] = 'N';
	CHECK_INT(exfat_status(volume), RAIJU_IMAGE_FS_UNSUPPORTED);
	builder_exfat_free(volume);
}

static void ufs2_reads_param_json_with_fragments(void) {
	builder_ufs2 *fs = builder_ufs2_game(4096, 512, PARAM_JSON);
	CHECK(fs != NULL);
	uint8_t *out = NULL;
	size_t length = 0;
	CHECK_INT(read_ufs2(fs, fs->size, &out, &length), RAIJU_IMAGE_FS_OK);
	CHECK_INT(length, strlen(PARAM_JSON));
	CHECK(out != NULL && memcmp(out, PARAM_JSON, length) == 0);
	free(out);
	builder_ufs2_free(fs);
}

static void ufs2_reads_shadowmountplus_profile(void) {
	builder_ufs2 *fs = builder_ufs2_game(65536, 65536, PARAM_JSON);
	CHECK(fs != NULL);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_OK);
	builder_ufs2_free(fs);
}

static void ufs2_reads_single_indirect_blocks(void) {
	char *json = long_param_json(20 * 4096);
	builder_ufs2 *fs = builder_ufs2_game(4096, 4096, json);
	CHECK(fs != NULL);
	uint8_t *out = NULL;
	size_t length = 0;
	CHECK_INT(read_ufs2(fs, fs->size, &out, &length), RAIJU_IMAGE_FS_OK);
	CHECK_INT(length, strlen(json));
	CHECK(out != NULL && memcmp(out, json, length) == 0);
	free(out);
	builder_ufs2_free(fs);
	free(json);
}

static void ufs2_reads_double_indirect_directory(void) {
	builder_ufs2 *fs = builder_ufs2_create(4096, 4096, 1, 1, 1024);
	CHECK(fs != NULL);
	if (fs == NULL) {
		return;
	}
	size_t blocks = 12 + 512 + 2;
	size_t dir_length = blocks * 4096;
	uint8_t *dir = calloc(1, dir_length);
	size_t last = 0;
	size_t length = builder_ufs2_dir_add(dir, 0, &last, 4, 4, ".");
	length = builder_ufs2_dir_add(dir, length, &last, BUILDER_UFS2_ROOT, 4, "..");
	for (size_t offset = length; offset < dir_length - DIRBLKSIZ; offset += DIRBLKSIZ) {
		dir[offset + 4] = DIRBLKSIZ & 0xFF;
		dir[offset + 5] = DIRBLKSIZ >> 8;
	}
	last = dir_length - DIRBLKSIZ;
	builder_ufs2_dir_add(dir, dir_length - DIRBLKSIZ, &last, 5, 8, "param.json");

	uint8_t root[DIRBLKSIZ];
	size_t root_last = 0;
	size_t root_length = builder_ufs2_dir_add(root, 0, &root_last, BUILDER_UFS2_ROOT, 4, ".");
	root_length = builder_ufs2_dir_add(root, root_length, &root_last, BUILDER_UFS2_ROOT, 4, "..");
	root_length = builder_ufs2_dir_add(root, root_length, &root_last, 4, 4, "sce_sys");
	CHECK_INT(builder_ufs2_write_inode(fs, BUILDER_UFS2_ROOT, BUILDER_UFS2_IFDIR | 0755, root, root_length), 0);
	CHECK_INT(builder_ufs2_write_inode(fs, 4, BUILDER_UFS2_IFDIR | 0755, dir, dir_length), 0);
	CHECK_INT(builder_ufs2_write_inode(fs, 5, BUILDER_UFS2_IFREG | 0644, (const uint8_t *)PARAM_JSON,
	                                   strlen(PARAM_JSON)),
	          0);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_OK);
	free(dir);
	builder_ufs2_free(fs);
}

static void ufs2_missing_entries_are_not_found(void) {
	builder_ufs2 *fs = builder_ufs2_game(4096, 512, PARAM_JSON);
	uint8_t *sce_sys = builder_ufs2_inode(fs, 4);
	uint8_t *entries = fs->data + (sce_sys[112] | (sce_sys[113] << 8) | (sce_sys[114] << 16)) * fs->fragment_size;
	entries[24 + 8] = 'P';
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_NOT_FOUND);
	entries[24 + 8] = 'p';
	sce_sys[1] = (uint8_t)(BUILDER_UFS2_IFREG >> 8);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_NOT_FOUND);
	builder_ufs2_free(fs);
}

static void ufs2_bad_superblock_fails(void) {
	struct {
		size_t offset;
		uint32_t value;
	} cases[] = {
		{120, 8}, {116, 100}, {184, 0}, {184, 17}, {44, 0}, {160, 8192}, {16, 0x7FFFFFF0u}, {188, 7}, {12, 600},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		builder_ufs2 *fs = builder_ufs2_game(4096, 512, PARAM_JSON);
		fixture_put_le32(fs->data + 65536 + cases[i].offset, cases[i].value);
		CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
		builder_ufs2_free(fs);
	}
	builder_ufs2 *fs = builder_ufs2_game(4096, 512, PARAM_JSON);
	fixture_put_le64(fs->data + 65536 + 1080, 0);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le64(fs->data + 65536 + 1080, UINT64_MAX);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le32(fs->data + 65536 + 1372, 0x011954);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_UNSUPPORTED);
	builder_ufs2_free(fs);
}

static void ufs2_bad_cylinder_group_fails(void) {
	builder_ufs2 *fs = builder_ufs2_game(4096, 512, PARAM_JSON);
	uint8_t *cg = builder_ufs2_cg(fs, 1);
	cg[4] ^= 0x01;
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	cg[4] ^= 0x01;
	fixture_put_le32(cg + 12, 0);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le32(cg + 12, 1);
	fixture_put_le32(cg + 120, 1);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le32(cg + 120, fs->inodes_per_group);
	fixture_put_le32(cg + 92, 0xFFFFFFF0u);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le32(cg + 92, 168);
	cg[168] = 0;
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	builder_ufs2_free(fs);
}

static void ufs2_bad_directory_entries_fail(void) {
	builder_ufs2 *fs = builder_ufs2_game(4096, 512, PARAM_JSON);
	uint8_t *root = builder_ufs2_inode(fs, BUILDER_UFS2_ROOT);
	uint8_t *entries = fs->data + (root[112] | (root[113] << 8) | (root[114] << 16)) * fs->fragment_size;
	entries[4] = 0;
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	entries[4] = 2;
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	entries[4] = 12;
	entries[7] = 200;
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	entries[7] = 1;
	fixture_put_le32(entries + 24 + 20, 0xFFFFFF);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le32(entries + 24 + 20, 1);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le32(entries + 24 + 20, 4);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_OK);
	fixture_put_le64(root + 16, 100);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le64(root + 16, (uint64_t)RAIJU_IMAGE_DIR_MAX_BYTES + DIRBLKSIZ);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	builder_ufs2_free(fs);
}

static void ufs2_bad_block_addresses_fail(void) {
	builder_ufs2 *fs = builder_ufs2_game(4096, 512, PARAM_JSON);
	uint8_t *param = builder_ufs2_inode(fs, param_inode(fs));
	uint64_t saved = 0;
	memcpy(&saved, param + 112, sizeof saved);
	fixture_put_le64(param + 112, fs->size / fs->fragment_size);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le64(param + 112, UINT64_MAX);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	memcpy(param + 112, &saved, sizeof saved);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_OK);
	builder_ufs2_free(fs);

	char *json = long_param_json(20 * 4096);
	fs = builder_ufs2_game(4096, 4096, json);
	param = builder_ufs2_inode(fs, param_inode(fs));
	fixture_put_le64(param + 208, (uint64_t)1 << 40);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_CORRUPT);
	fixture_put_le64(param + 208, 0);
	uint8_t *out = NULL;
	size_t length = 0;
	CHECK_INT(read_ufs2(fs, fs->size, &out, &length), RAIJU_IMAGE_FS_OK);
	CHECK(out != NULL && out[12 * 4096] == 0 && out[length - 1] == 0);
	free(out);
	builder_ufs2_free(fs);
	free(json);
}

static void ufs2_oversized_param_json_is_too_large(void) {
	builder_ufs2 *fs = builder_ufs2_game(4096, 512, PARAM_JSON);
	fixture_put_le64(builder_ufs2_inode(fs, param_inode(fs)) + 16, RAIJU_PARAM_JSON_MAX_BYTES + 1);
	CHECK_INT(ufs2_status(fs), RAIJU_IMAGE_FS_TOO_LARGE);
	builder_ufs2_free(fs);
}

static void ufs2_truncated_image_fails(void) {
	builder_ufs2 *fs = builder_ufs2_game(4096, 512, PARAM_JSON);
	uint8_t *out = NULL;
	size_t length = 0;
	CHECK_INT(read_ufs2(fs, fs->size / 2, &out, &length), RAIJU_IMAGE_FS_CORRUPT);
	CHECK_INT(read_ufs2(fs, 65536, &out, &length), RAIJU_IMAGE_FS_UNSUPPORTED);
	CHECK(out == NULL);
	builder_ufs2_free(fs);
}

/**
 * Writes an image buffer to a temporary file named name and returns its path.
 */
static char *write_image(const char *dir, const char *name, const uint8_t *data, size_t length) {
	char *path = fixture_path(dir, name);
	CHECK(path != NULL && fixture_write(path, data, length) == 0);
	return path;
}

static void title_id_from_image_files(void) {
	char *dir = fixture_temp_dir();
	CHECK(dir != NULL);
	if (dir == NULL) {
		return;
	}
	builder_exfat *volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	builder_ufs2 *fs = builder_ufs2_game(65536, 65536, PARAM_JSON);
	char *exfat_path = write_image(dir, "game.exfat", volume->data, volume->size);
	char *ufs2_path = write_image(dir, "CUSA00001.ffpkg", fs->data, fs->size);
	char *zip_path = write_image(dir, "PPSA11111.zip", (const uint8_t *)"PK\3\4", 4);

	char title_id[RAIJU_TITLE_ID_LEN + 1];
	raiju_image_fs_status fs_status = RAIJU_IMAGE_FS_IO_ERROR;
	CHECK_INT(raiju_title_id_from_image(exfat_path, title_id, &fs_status), RAIJU_PARAM_OK);
	CHECK_INT(fs_status, RAIJU_IMAGE_FS_OK);
	CHECK_STR(title_id, "PPSA24003");
	CHECK_INT(raiju_title_id_from_image(ufs2_path, title_id, NULL), RAIJU_PARAM_OK);
	CHECK_STR(title_id, "PPSA24003");

	raiju_title_resolution resolution = raiju_resolve_image_title_id(ufs2_path);
	CHECK_INT(resolution.source, RAIJU_TITLE_SOURCE_PARAM_JSON);
	CHECK_STR(resolution.title_id, "PPSA24003");
	CHECK(resolution.names_disagree);

	CHECK_INT(raiju_title_id_from_image(zip_path, title_id, &fs_status), RAIJU_PARAM_IO_ERROR);
	CHECK_INT(fs_status, RAIJU_IMAGE_FS_UNSUPPORTED);
	resolution = raiju_resolve_image_title_id(zip_path);
	CHECK_INT(resolution.source, RAIJU_TITLE_SOURCE_FILE_NAME);
	CHECK_STR(resolution.title_id, "PPSA11111");

	char *missing = fixture_path(dir, "missing.exfat");
	CHECK_INT(raiju_title_id_from_image(missing, title_id, &fs_status), RAIJU_PARAM_IO_ERROR);
	CHECK_INT(fs_status, RAIJU_IMAGE_FS_IO_ERROR);

	fixture_remove(exfat_path);
	fixture_remove(ufs2_path);
	fixture_remove(zip_path);
	fixture_remove(dir);
	free(missing);
	free(exfat_path);
	free(ufs2_path);
	free(zip_path);
	free(dir);
	builder_exfat_free(volume);
	builder_ufs2_free(fs);
}

static void title_id_from_image_reports_bad_param_json(void) {
	char *dir = fixture_temp_dir();
	CHECK(dir != NULL);
	if (dir == NULL) {
		return;
	}
	builder_exfat *volume = builder_exfat_game("sce_sys", "param.json", "{\"titleId\":\"ppsa24003\"}");
	char *path = write_image(dir, "game.exfat", volume->data, volume->size);
	char title_id[RAIJU_TITLE_ID_LEN + 1];
	CHECK_INT(raiju_title_id_from_image(path, title_id, NULL), RAIJU_PARAM_BAD_TITLE_ID);
	fixture_remove(path);
	free(path);
	builder_exfat_free(volume);

	volume = builder_exfat_game("sce_sys", "param.json", PARAM_JSON);
	fixture_put_le64(exfat_param_stream_entry(volume) + 24, RAIJU_PARAM_JSON_MAX_BYTES + 1);
	fixture_put_le64(exfat_param_stream_entry(volume) + 8, 0);
	exfat_reseal_param_set(volume);
	path = write_image(dir, "big.exfat", volume->data, volume->size);
	raiju_image_fs_status fs_status;
	CHECK_INT(raiju_title_id_from_image(path, title_id, &fs_status), RAIJU_PARAM_TOO_LARGE);
	CHECK_INT(fs_status, RAIJU_IMAGE_FS_TOO_LARGE);
	fixture_remove(path);
	free(path);
	builder_exfat_free(volume);
	fixture_remove(dir);
	free(dir);
}

/**
 * Checks one image produced by an external tool: the structural check must
 * accept it and its title ID must resolve through param.json.
 */
static void check_external_image(const char *path, const char *expected) {
	raiju_inspection inspection;
	CHECK_INT(raiju_inspect_file(path, &inspection), 0);
	CHECK(raiju_inspection_accepted(&inspection));
	char title_id[RAIJU_TITLE_ID_LEN + 1] = {0};
	raiju_image_fs_status fs_status = RAIJU_IMAGE_FS_IO_ERROR;
	raiju_param_status status = raiju_title_id_from_image(path, title_id, &fs_status);
	if (status != RAIJU_PARAM_OK) {
		fprintf(stderr, "%s: %s / %s\n", path, raiju_param_status_name(status), raiju_image_fs_status_name(fs_status));
	}
	CHECK_INT(status, RAIJU_PARAM_OK);
	CHECK_STR(title_id, expected);
}

static void external_images_resolve_title(void) {
	const char *list = getenv("RAIJU_TEST_IMAGES");
	const char *expected = getenv("RAIJU_TEST_TITLE_ID");
	if (list == NULL || list[0] == '\0') {
		printf("    skipped: set RAIJU_TEST_IMAGES to a colon-separated list of images\n");
		return;
	}
	char *paths = malloc(strlen(list) + 1);
	CHECK(paths != NULL);
	if (paths == NULL) {
		return;
	}
	strcpy(paths, list);
	char *path = paths;
	while (path != NULL) {
		char *separator = strchr(path, ':');
		if (separator != NULL) {
			*separator = '\0';
		}
		if (path[0] != '\0') {
			printf("    %s\n", path);
			check_external_image(path, expected != NULL ? expected : "PPSA24003");
		}
		path = separator != NULL ? separator + 1 : NULL;
	}
	free(paths);
}

static void dispatch_and_status_names(void) {
	builder_ufs2 *fs = builder_ufs2_game(4096, 512, PARAM_JSON);
	raiju_image_io io = builder_memory_io(fs->data, fs->size);
	uint8_t *out = NULL;
	size_t length = 0;
	CHECK_INT(raiju_image_read_param_json(&io, RAIJU_KIND_UFS2, &out, &length), RAIJU_IMAGE_FS_OK);
	free(out);
	out = NULL;
	CHECK_INT(raiju_image_read_param_json(&io, RAIJU_KIND_EXFAT, &out, &length), RAIJU_IMAGE_FS_UNSUPPORTED);
	CHECK_INT(raiju_image_read_param_json(&io, RAIJU_KIND_PFS, &out, &length), RAIJU_IMAGE_FS_UNSUPPORTED);
	CHECK(out == NULL);
	builder_ufs2_free(fs);
	CHECK_STR(raiju_image_fs_status_name(RAIJU_IMAGE_FS_OK), "ok");
	CHECK_STR(raiju_image_fs_status_name(RAIJU_IMAGE_FS_CORRUPT), "corrupt");
	CHECK_STR(raiju_image_fs_status_name(RAIJU_IMAGE_FS_TOO_LARGE), "too_large");
}

void run_image_fs_tests(void) {
	RUN_TEST(exfat_reads_param_json_across_chained_clusters);
	RUN_TEST(exfat_matches_names_case_insensitively);
	RUN_TEST(exfat_zero_fills_past_valid_data_length);
	RUN_TEST(exfat_follows_root_chain_and_second_fat);
	RUN_TEST(exfat_missing_entries_are_not_found);
	RUN_TEST(exfat_sce_sys_file_is_not_a_directory);
	RUN_TEST(exfat_cyclic_root_chain_fails);
	RUN_TEST(exfat_cyclic_file_chain_fails);
	RUN_TEST(exfat_out_of_range_clusters_fail);
	RUN_TEST(exfat_bad_boot_geometry_fails);
	RUN_TEST(exfat_bad_entry_sets_fail);
	RUN_TEST(exfat_oversized_param_json_is_too_large);
	RUN_TEST(exfat_truncated_or_foreign_image_fails);
	RUN_TEST(ufs2_reads_param_json_with_fragments);
	RUN_TEST(ufs2_reads_shadowmountplus_profile);
	RUN_TEST(ufs2_reads_single_indirect_blocks);
	RUN_TEST(ufs2_reads_double_indirect_directory);
	RUN_TEST(ufs2_missing_entries_are_not_found);
	RUN_TEST(ufs2_bad_superblock_fails);
	RUN_TEST(ufs2_bad_cylinder_group_fails);
	RUN_TEST(ufs2_bad_directory_entries_fail);
	RUN_TEST(ufs2_bad_block_addresses_fail);
	RUN_TEST(ufs2_oversized_param_json_is_too_large);
	RUN_TEST(ufs2_truncated_image_fails);
	RUN_TEST(title_id_from_image_files);
	RUN_TEST(title_id_from_image_reports_bad_param_json);
	RUN_TEST(dispatch_and_status_names);
	RUN_TEST(external_images_resolve_title);
}
