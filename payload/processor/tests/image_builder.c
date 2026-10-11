#include "image_builder.h"

#include <stdlib.h>
#include <string.h>

#include "fixtures.h"

#define EXFAT_SECTOR_SHIFT 9u
#define EXFAT_SECTOR 512u
#define EXFAT_FAT_OFFSET_SECTORS 24u
#define EXFAT_ENTRY 32u
#define EXFAT_NAME_UNITS 15u
#define UFS2_SUPERBLOCK 65536u
#define UFS2_SUPERBLOCK_AREA 8192u
#define UFS2_INODE 256u
#define UFS2_USED_MAP 168u
#define UFS2_DIRBLKSIZ 512u

/**
 * Reads exactly length bytes at offset from the buffer stored in context.
 */
static int memory_read_at(void *context, uint64_t offset, void *buffer, size_t length) {
	memcpy(buffer, (const uint8_t *)context + offset, length);
	return 0;
}

raiju_image_io builder_memory_io(const uint8_t *data, size_t size) {
	raiju_image_io io = {memory_read_at, (void *)(uintptr_t)data, size};
	return io;
}

/**
 * Writes a little-endian 16-bit value at p.
 */
static void put_le16(uint8_t *p, uint16_t value) {
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

builder_exfat *builder_exfat_create(unsigned cluster_shift, uint32_t cluster_count) {
	builder_exfat *volume = calloc(1, sizeof *volume);
	if (volume == NULL) {
		return NULL;
	}
	uint32_t fat_sectors = ((cluster_count + 2) * 4 + EXFAT_SECTOR - 1) / EXFAT_SECTOR;
	uint32_t heap_sector = EXFAT_FAT_OFFSET_SECTORS + fat_sectors;
	uint64_t volume_sectors = heap_sector + ((uint64_t)cluster_count << cluster_shift);
	volume->size = (size_t)(volume_sectors * EXFAT_SECTOR);
	volume->data = fixture_exfat(volume->size, EXFAT_SECTOR_SHIFT, cluster_shift, volume_sectors);
	if (volume->data == NULL) {
		free(volume);
		return NULL;
	}
	volume->cluster_size = EXFAT_SECTOR << cluster_shift;
	volume->cluster_count = cluster_count;
	volume->fat_offset = (uint64_t)EXFAT_FAT_OFFSET_SECTORS * EXFAT_SECTOR;
	volume->heap_offset = (uint64_t)heap_sector * EXFAT_SECTOR;
	volume->next_free = 2;
	fixture_put_le32(volume->data + 80, EXFAT_FAT_OFFSET_SECTORS);
	fixture_put_le32(volume->data + 84, fat_sectors);
	fixture_put_le32(volume->data + 88, heap_sector);
	fixture_put_le32(volume->data + 92, cluster_count);
	builder_exfat_set_fat(volume, 0, 0xFFFFFFF8u);
	builder_exfat_set_fat(volume, 1, BUILDER_EXFAT_END_OF_CHAIN);

	uint32_t bitmap = builder_exfat_alloc(volume, 1, true);
	uint32_t upcase = builder_exfat_alloc(volume, 1, true);
	volume->root_cluster = builder_exfat_alloc(volume, 1, true);
	fixture_put_le32(volume->data + 96, volume->root_cluster);
	fixture_exfat_reseal(volume->data, EXFAT_SECTOR_SHIFT);

	uint8_t *root = builder_exfat_cluster(volume, volume->root_cluster);
	root[0] = 0x81;
	fixture_put_le32(root + 20, bitmap);
	fixture_put_le64(root + 24, (cluster_count + 7) / 8);
	root[EXFAT_ENTRY] = 0x82;
	fixture_put_le32(root + EXFAT_ENTRY + 20, upcase);
	fixture_put_le64(root + EXFAT_ENTRY + 24, 128);
	return volume;
}

void builder_exfat_free(builder_exfat *volume) {
	if (volume != NULL) {
		free(volume->data);
		free(volume);
	}
}

uint8_t *builder_exfat_cluster(builder_exfat *volume, uint32_t cluster) {
	return volume->data + volume->heap_offset + (size_t)(cluster - 2) * volume->cluster_size;
}

void builder_exfat_set_fat(builder_exfat *volume, uint32_t cluster, uint32_t next) {
	fixture_put_le32(volume->data + volume->fat_offset + (size_t)cluster * 4, next);
}

uint32_t builder_exfat_alloc(builder_exfat *volume, uint32_t count, bool chained) {
	uint32_t first = volume->next_free;
	volume->next_free += count;
	if (chained) {
		for (uint32_t i = 0; i < count; i++) {
			builder_exfat_set_fat(volume, first + i, i + 1 < count ? first + i + 1 : BUILDER_EXFAT_END_OF_CHAIN);
		}
	}
	return first;
}

size_t builder_exfat_write_entry(builder_exfat *volume, const uint32_t *dir_clusters, size_t dir_cluster_count,
                                 size_t offset, const builder_exfat_entry *entry) {
	uint8_t set[19 * EXFAT_ENTRY];
	memset(set, 0, sizeof set);
	size_t name_length = strlen(entry->name);
	size_t name_entries = (name_length + EXFAT_NAME_UNITS - 1) / EXFAT_NAME_UNITS;
	size_t secondary = 1 + name_entries;
	set[0] = 0x85;
	set[1] = (uint8_t)secondary;
	put_le16(set + 4, entry->attributes);
	uint8_t *stream = set + EXFAT_ENTRY;
	stream[0] = 0xC0;
	stream[1] = entry->flags;
	stream[3] = (uint8_t)name_length;
	fixture_put_le64(stream + 8, entry->valid_length);
	fixture_put_le32(stream + 20, entry->first_cluster);
	fixture_put_le64(stream + 24, entry->length);
	for (size_t i = 0; i < name_length; i++) {
		uint8_t *name_entry = set + (2 + i / EXFAT_NAME_UNITS) * EXFAT_ENTRY;
		name_entry[0] = 0xC1;
		put_le16(name_entry + 2 + (i % EXFAT_NAME_UNITS) * 2, (uint8_t)entry->name[i]);
	}
	size_t set_length = (secondary + 1) * EXFAT_ENTRY;
	uint16_t checksum = 0;
	for (size_t i = 0; i < set_length; i++) {
		if (i == 2 || i == 3) {
			continue;
		}
		checksum = (uint16_t)(((checksum & 1u) ? 0x8000u : 0u) + (checksum >> 1) + set[i]);
	}
	put_le16(set + 2, checksum);
	for (size_t i = 0; i < set_length; i++) {
		size_t position = offset + i;
		size_t index = position / volume->cluster_size;
		if (index >= dir_cluster_count) {
			break;
		}
		builder_exfat_cluster(volume, dir_clusters[index])[position % volume->cluster_size] = set[i];
	}
	return offset + set_length;
}

builder_exfat *builder_exfat_game(const char *sce_sys_name, const char *param_name, const char *json) {
	builder_exfat *volume = builder_exfat_create(0, 64);
	if (volume == NULL) {
		return NULL;
	}
	size_t json_length = strlen(json);
	uint32_t root = volume->root_cluster;

	uint32_t eboot = builder_exfat_alloc(volume, 1, true);
	memcpy(builder_exfat_cluster(volume, eboot), "\177ELF", 4);
	builder_exfat_entry eboot_entry = {"eboot.bin", 0x20, BUILDER_EXFAT_ALLOCATED, eboot, 4, 4};
	size_t offset = builder_exfat_write_entry(volume, &root, 1, 2 * EXFAT_ENTRY, &eboot_entry);

	uint32_t sce_sys = builder_exfat_alloc(volume, 2, false);
	uint32_t sce_sys_clusters[] = {sce_sys, sce_sys + 1};
	builder_exfat_entry dir_entry = {sce_sys_name, BUILDER_EXFAT_ATTR_DIRECTORY, BUILDER_EXFAT_NO_FAT_CHAIN, sce_sys,
	                                 2u * volume->cluster_size, 2u * volume->cluster_size};
	builder_exfat_write_entry(volume, &root, 1, offset, &dir_entry);

	uint32_t icon = builder_exfat_alloc(volume, 1, true);
	builder_exfat_entry icon_entry = {"icon0.png", 0x20, BUILDER_EXFAT_ALLOCATED, icon, 8, 8};
	size_t sce_offset = builder_exfat_write_entry(volume, sce_sys_clusters, 2, 0, &icon_entry);
	size_t param_offset = volume->cluster_size - 2 * EXFAT_ENTRY;
	for (; sce_offset < param_offset; sce_offset += EXFAT_ENTRY) {
		builder_exfat_cluster(volume, sce_sys)[sce_offset] = 0x05;
	}

	uint32_t clusters = (uint32_t)((json_length + volume->cluster_size - 1) / volume->cluster_size);
	uint32_t first = 0;
	uint32_t previous = 0;
	for (uint32_t i = 0; i < clusters; i++) {
		uint32_t cluster = builder_exfat_alloc(volume, 1, false);
		builder_exfat_alloc(volume, 1, false);
		size_t start = (size_t)i * volume->cluster_size;
		size_t chunk = json_length - start < volume->cluster_size ? json_length - start : volume->cluster_size;
		memcpy(builder_exfat_cluster(volume, cluster), json + start, chunk);
		if (previous != 0) {
			builder_exfat_set_fat(volume, previous, cluster);
		} else {
			first = cluster;
		}
		previous = cluster;
	}
	if (previous != 0) {
		builder_exfat_set_fat(volume, previous, BUILDER_EXFAT_END_OF_CHAIN);
	}
	builder_exfat_entry param_entry = {param_name, 0x20, BUILDER_EXFAT_ALLOCATED, first, json_length, json_length};
	builder_exfat_write_entry(volume, sce_sys_clusters, 2, param_offset, &param_entry);
	return volume;
}

builder_ufs2 *builder_ufs2_create(uint32_t block_size, uint32_t fragment_size, uint32_t cylinder_groups,
                                  uint32_t inode_blocks, uint32_t group_blocks) {
	if (fragment_size == 0 || block_size % fragment_size != 0 || cylinder_groups == 0) {
		return NULL;
	}
	builder_ufs2 *fs = calloc(1, sizeof *fs);
	if (fs == NULL) {
		return NULL;
	}
	fs->block_size = block_size;
	fs->fragment_size = fragment_size;
	fs->frags_per_block = block_size / fragment_size;
	fs->cylinder_groups = cylinder_groups;
	fs->inodes_per_block = block_size / UFS2_INODE;
	fs->inodes_per_group = inode_blocks * fs->inodes_per_block;
	fs->frags_per_group = group_blocks * fs->frags_per_block;
	uint32_t superblock_frag = UFS2_SUPERBLOCK / fragment_size;
	uint32_t superblock_area = (UFS2_SUPERBLOCK_AREA + block_size - 1) / block_size * block_size;
	fs->cg_header_frag = superblock_frag + superblock_area / fragment_size;
	fs->inode_table_frag = fs->cg_header_frag + fs->frags_per_block;
	fs->data_frag = fs->inode_table_frag + inode_blocks * fs->frags_per_block;
	fs->next_free_frag = fs->data_frag;
	if (fs->data_frag >= fs->frags_per_group) {
		free(fs);
		return NULL;
	}
	uint64_t fragments = (uint64_t)cylinder_groups * fs->frags_per_group;
	fs->size = (size_t)(fragments * fragment_size);
	fs->data = calloc(1, fs->size);
	if (fs->data == NULL) {
		free(fs);
		return NULL;
	}
	uint8_t *sb = fs->data + UFS2_SUPERBLOCK;
	fixture_put_le32(sb + 8, superblock_frag);
	fixture_put_le32(sb + 12, fs->cg_header_frag);
	fixture_put_le32(sb + 16, fs->inode_table_frag);
	fixture_put_le32(sb + 20, fs->data_frag);
	fixture_put_le32(sb + 44, cylinder_groups);
	fixture_put_le32(sb + 48, block_size);
	fixture_put_le32(sb + 52, fragment_size);
	fixture_put_le32(sb + 56, fs->frags_per_block);
	fixture_put_le32(sb + 104, UFS2_SUPERBLOCK_AREA);
	fixture_put_le32(sb + 116, block_size / 8);
	fixture_put_le32(sb + 120, fs->inodes_per_block);
	fixture_put_le32(sb + 160, block_size);
	fixture_put_le32(sb + 184, fs->inodes_per_group);
	fixture_put_le32(sb + 188, fs->frags_per_group);
	fixture_put_le64(sb + 1000, UFS2_SUPERBLOCK);
	fixture_put_le64(sb + 1080, fragments);
	fixture_put_le32(sb + 1372, 0x19540119u);
	for (uint32_t group = 0; group < cylinder_groups; group++) {
		uint8_t *cg = builder_ufs2_cg(fs, group);
		fixture_put_le32(cg + 4, 0x090255);
		fixture_put_le32(cg + 12, group);
		fixture_put_le32(cg + 92, UFS2_USED_MAP);
		fixture_put_le32(cg + 116, fs->inodes_per_group);
		fixture_put_le32(cg + 120, fs->inodes_per_group);
	}
	builder_ufs2_cg(fs, 0)[UFS2_USED_MAP] = 0x03;
	return fs;
}

void builder_ufs2_free(builder_ufs2 *fs) {
	if (fs != NULL) {
		free(fs->data);
		free(fs);
	}
}

uint8_t *builder_ufs2_cg(builder_ufs2 *fs, uint32_t group) {
	uint64_t frag = (uint64_t)group * fs->frags_per_group + fs->cg_header_frag;
	return fs->data + frag * fs->fragment_size;
}

uint8_t *builder_ufs2_inode(builder_ufs2 *fs, uint32_t ino) {
	uint32_t group = ino / fs->inodes_per_group;
	uint32_t index = ino % fs->inodes_per_group;
	uint64_t frag = (uint64_t)group * fs->frags_per_group + fs->inode_table_frag +
	                (uint64_t)(index / fs->inodes_per_block) * fs->frags_per_block;
	return fs->data + frag * fs->fragment_size + (size_t)(index % fs->inodes_per_block) * UFS2_INODE;
}

/**
 * Takes the next free block from cylinder group 0 and returns its fragment
 * address, or zero when the group is full.
 */
static uint64_t alloc_block(builder_ufs2 *fs) {
	if (fs->next_free_frag + fs->frags_per_block > fs->frags_per_group) {
		return 0;
	}
	uint64_t frag = fs->next_free_frag;
	fs->next_free_frag += fs->frags_per_block;
	return frag;
}

/**
 * Returns a pointer to the start of the block at fragment address frag.
 */
static uint8_t *block_at(builder_ufs2 *fs, uint64_t frag) {
	return fs->data + frag * fs->fragment_size;
}

/**
 * Reads the pointer slot of an indirect block, allocating the block it
 * points to when the slot is empty.
 */
static uint64_t pointer_slot(builder_ufs2 *fs, uint8_t *slot) {
	uint64_t frag = 0;
	for (int i = 7; i >= 0; i--) {
		frag = (frag << 8) | slot[i];
	}
	if (frag == 0) {
		frag = alloc_block(fs);
		fixture_put_le64(slot, frag);
	}
	return frag;
}

int builder_ufs2_write_inode(builder_ufs2 *fs, uint32_t ino, uint16_t mode, const uint8_t *content, size_t length) {
	uint8_t *inode = builder_ufs2_inode(fs, ino);
	memset(inode, 0, UFS2_INODE);
	put_le16(inode, mode);
	put_le16(inode + 2, (mode & 0170000) == BUILDER_UFS2_IFDIR ? 2 : 1);
	fixture_put_le32(inode + 12, fs->block_size);
	fixture_put_le64(inode + 16, length);
	uint64_t per_block = fs->block_size / 8;
	size_t blocks = (length + fs->block_size - 1) / fs->block_size;
	for (size_t lbn = 0; lbn < blocks; lbn++) {
		uint64_t frag = alloc_block(fs);
		if (frag == 0) {
			return -1;
		}
		size_t start = lbn * fs->block_size;
		size_t chunk = length - start < fs->block_size ? length - start : fs->block_size;
		memcpy(block_at(fs, frag), content + start, chunk);
		uint8_t *slot;
		if (lbn < 12) {
			slot = inode + 112 + lbn * 8;
		} else if (lbn - 12 < per_block) {
			slot = block_at(fs, pointer_slot(fs, inode + 208)) + (lbn - 12) * 8;
		} else {
			uint64_t index = lbn - 12 - per_block;
			uint64_t middle = pointer_slot(fs, block_at(fs, pointer_slot(fs, inode + 216)) + (index / per_block) * 8);
			slot = block_at(fs, middle) + (index % per_block) * 8;
		}
		fixture_put_le64(slot, frag);
	}
	uint32_t group = ino / fs->inodes_per_group;
	uint32_t index = ino % fs->inodes_per_group;
	builder_ufs2_cg(fs, group)[UFS2_USED_MAP + index / 8] |= (uint8_t)(1u << (index % 8));
	return 0;
}

size_t builder_ufs2_dir_add(uint8_t *dir, size_t length, size_t *last_entry, uint32_t ino, uint8_t type,
                            const char *name) {
	size_t name_length = strlen(name);
	size_t needed = (8 + name_length + 1 + 3) & ~(size_t)3;
	size_t position = 0;
	if (length > 0) {
		uint8_t *last = dir + *last_entry;
		position = *last_entry + ((8 + last[7] + 1 + 3) & ~(size_t)3);
		if (position % UFS2_DIRBLKSIZ + needed > UFS2_DIRBLKSIZ || position % UFS2_DIRBLKSIZ == 0) {
			position = length;
		} else {
			put_le16(last + 4, (uint16_t)(position - *last_entry));
		}
	}
	uint8_t *entry = dir + position;
	memset(entry, 0, UFS2_DIRBLKSIZ - position % UFS2_DIRBLKSIZ);
	fixture_put_le32(entry, ino);
	put_le16(entry + 4, (uint16_t)(UFS2_DIRBLKSIZ - position % UFS2_DIRBLKSIZ));
	entry[6] = type;
	entry[7] = (uint8_t)name_length;
	memcpy(entry + 8, name, name_length);
	*last_entry = position;
	return (position / UFS2_DIRBLKSIZ + 1) * UFS2_DIRBLKSIZ;
}

builder_ufs2 *builder_ufs2_game(uint32_t block_size, uint32_t fragment_size, const char *json) {
	uint32_t groups = block_size == fragment_size ? 1 : 2;
	uint32_t group_blocks = block_size == fragment_size ? 64 : 128;
	builder_ufs2 *fs = builder_ufs2_create(block_size, fragment_size, groups, 1, group_blocks);
	if (fs == NULL) {
		return NULL;
	}
	uint32_t param_ino = groups > 1 ? (groups - 1) * fs->inodes_per_group + 1 : 5;
	uint8_t dir[UFS2_DIRBLKSIZ * 2];
	size_t last = 0;
	size_t length = builder_ufs2_dir_add(dir, 0, &last, BUILDER_UFS2_ROOT, 4, ".");
	length = builder_ufs2_dir_add(dir, length, &last, BUILDER_UFS2_ROOT, 4, "..");
	length = builder_ufs2_dir_add(dir, length, &last, 3, 8, "eboot.bin");
	length = builder_ufs2_dir_add(dir, length, &last, 4, 4, "sce_sys");
	int failed = builder_ufs2_write_inode(fs, BUILDER_UFS2_ROOT, BUILDER_UFS2_IFDIR | 0755, dir, length);
	failed |= builder_ufs2_write_inode(fs, 3, BUILDER_UFS2_IFREG | 0644, (const uint8_t *)"\177ELF", 4);
	length = builder_ufs2_dir_add(dir, 0, &last, 4, 4, ".");
	length = builder_ufs2_dir_add(dir, length, &last, BUILDER_UFS2_ROOT, 4, "..");
	length = builder_ufs2_dir_add(dir, length, &last, param_ino, 8, "param.json");
	failed |= builder_ufs2_write_inode(fs, 4, BUILDER_UFS2_IFDIR | 0755, dir, length);
	failed |= builder_ufs2_write_inode(fs, param_ino, BUILDER_UFS2_IFREG | 0644, (const uint8_t *)json, strlen(json));
	if (failed) {
		builder_ufs2_free(fs);
		return NULL;
	}
	return fs;
}
