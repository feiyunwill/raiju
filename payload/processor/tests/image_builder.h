#ifndef RAIJU_IMAGE_BUILDER_H
#define RAIJU_IMAGE_BUILDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raiju/image_fs.h"

#define BUILDER_EXFAT_ATTR_DIRECTORY 0x10
#define BUILDER_EXFAT_ALLOCATED 0x01
#define BUILDER_EXFAT_NO_FAT_CHAIN 0x03
#define BUILDER_EXFAT_END_OF_CHAIN 0xFFFFFFFFu
#define BUILDER_UFS2_IFDIR 0040000
#define BUILDER_UFS2_IFREG 0100000
#define BUILDER_UFS2_ROOT 2u

/**
 * An in-memory exFAT volume with 512-byte sectors: boot region, one or two
 * FATs, and a cluster heap. Offsets are in bytes from the start of data.
 */
typedef struct {
	uint8_t *data;
	size_t size;
	uint32_t cluster_size;
	uint32_t cluster_count;
	uint64_t fat_offset;
	uint64_t heap_offset;
	uint32_t next_free;
	uint32_t root_cluster;
} builder_exfat;

/**
 * One exFAT directory entry set to write: a File entry, its Stream Extension
 * and enough File Name entries for name.
 */
typedef struct {
	const char *name;
	uint16_t attributes;
	uint8_t flags;
	uint32_t first_cluster;
	uint64_t length;
	uint64_t valid_length;
} builder_exfat_entry;

/**
 * Creates an exFAT volume of cluster_count clusters of 512 << cluster_shift
 * bytes, with a one-cluster root directory that already holds allocation
 * bitmap and up-case table entries. Returns NULL on allocation failure.
 */
builder_exfat *builder_exfat_create(unsigned cluster_shift, uint32_t cluster_count);

/**
 * Frees a volume made by builder_exfat_create.
 */
void builder_exfat_free(builder_exfat *volume);

/**
 * Reserves count consecutive clusters. When chained, the FAT links them and
 * ends the chain; otherwise the FAT is left untouched, as for NoFatChain.
 */
uint32_t builder_exfat_alloc(builder_exfat *volume, uint32_t count, bool chained);

/**
 * Returns a pointer to the first byte of a cluster in the heap.
 */
uint8_t *builder_exfat_cluster(builder_exfat *volume, uint32_t cluster);

/**
 * Overwrites the FAT entry of cluster.
 */
void builder_exfat_set_fat(builder_exfat *volume, uint32_t cluster, uint32_t next);

/**
 * Writes an entry set at byte offset within the byte stream of a directory
 * whose clusters are listed in order. Returns the offset just past the set.
 */
size_t builder_exfat_write_entry(builder_exfat *volume, const uint32_t *dir_clusters, size_t dir_cluster_count,
                                 size_t offset, const builder_exfat_entry *entry);

/**
 * Builds a valid exFAT volume holding /eboot.bin, /sce_sys/icon0.png and
 * /sce_sys/param.json with the given contents. param.json is stored on a FAT
 * chain spanning several clusters. Names are written as given, so callers can
 * vary their case.
 */
builder_exfat *builder_exfat_game(const char *sce_sys_name, const char *param_name, const char *json);

/**
 * An in-memory UFS2 filesystem with the superblock at 65536, cylinder group
 * headers and inode tables at the FreeBSD offsets, and a bump allocator for
 * data blocks in cylinder group 0.
 */
typedef struct {
	uint8_t *data;
	size_t size;
	uint32_t block_size;
	uint32_t fragment_size;
	uint32_t frags_per_block;
	uint32_t cylinder_groups;
	uint32_t inodes_per_group;
	uint32_t frags_per_group;
	uint32_t inodes_per_block;
	uint32_t cg_header_frag;
	uint32_t inode_table_frag;
	uint32_t data_frag;
	uint64_t next_free_frag;
} builder_ufs2;

/**
 * Creates a zeroed UFS2 filesystem with the given geometry. inode_blocks is
 * the number of inode table blocks per cylinder group and group_blocks the
 * number of blocks per group. Returns NULL on bad geometry.
 */
builder_ufs2 *builder_ufs2_create(uint32_t block_size, uint32_t fragment_size, uint32_t cylinder_groups,
                                  uint32_t inode_blocks, uint32_t group_blocks);

/**
 * Frees a filesystem made by builder_ufs2_create.
 */
void builder_ufs2_free(builder_ufs2 *fs);

/**
 * Writes a file or directory inode with the given contents, allocating
 * direct, single indirect and double indirect blocks as needed and marking
 * the inode used in its cylinder group. Returns 0 on success.
 */
int builder_ufs2_write_inode(builder_ufs2 *fs, uint32_t ino, uint16_t mode, const uint8_t *content, size_t length);

/**
 * Returns a pointer to the raw 256-byte inode ino.
 */
uint8_t *builder_ufs2_inode(builder_ufs2 *fs, uint32_t ino);

/**
 * Returns a pointer to the cylinder group header of group.
 */
uint8_t *builder_ufs2_cg(builder_ufs2 *fs, uint32_t group);

/**
 * Appends a directory entry to a directory buffer being built in DIRBLKSIZ
 * chunks. The last entry of each chunk owns its remaining space. Returns the
 * new length of the buffer.
 */
size_t builder_ufs2_dir_add(uint8_t *dir, size_t length, size_t *last_entry, uint32_t ino, uint8_t type,
                            const char *name);

/**
 * Builds a valid UFS2 filesystem holding /eboot.bin and /sce_sys/param.json
 * with the given contents. param.json's inode is placed in the last
 * cylinder group.
 */
builder_ufs2 *builder_ufs2_game(uint32_t block_size, uint32_t fragment_size, const char *json);

/**
 * Returns an image view over an in-memory buffer.
 */
raiju_image_io builder_memory_io(const uint8_t *data, size_t size);

#endif
