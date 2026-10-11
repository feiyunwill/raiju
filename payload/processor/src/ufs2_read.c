#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "byte_order.h"
#include "image_read.h"
#include "raiju/classify.h"
#include "raiju/image_fs.h"
#include "raiju/title_id.h"

enum {
	SB_CBLKNO = 12,
	SB_IBLKNO = 16,
	SB_NCG = 44,
	SB_BSIZE = 48,
	SB_FSIZE = 52,
	SB_FRAG = 56,
	SB_NINDIR = 116,
	SB_INOPB = 120,
	SB_CGSIZE = 160,
	SB_IPG = 184,
	SB_FPG = 188,
	SB_SIZE = 1080,
	SB_MAGIC = 1372,
	SB_BYTES = 1376,
	MIN_BLOCK_SIZE = 4096,
	MAX_BLOCK_SIZE = 65536,
	MIN_FRAGMENT_SIZE = 512,
	MAX_FRAGS_PER_BLOCK = 8
};

enum {
	CG_MAGIC = 4,
	CG_CGX = 12,
	CG_IUSEDOFF = 92,
	CG_INITEDIBLK = 120,
	CG_HEADER_BYTES = 124,
	CG_MAGIC_VALUE = 0x090255
};

enum {
	INODE_BYTES = 256,
	DI_MODE = 0,
	DI_NLINK = 2,
	DI_SIZE = 16,
	DI_DB = 112,
	DI_IB = 208,
	NDADDR = 12,
	NIADDR = 3,
	IFMT = 0170000,
	IFDIR = 0040000,
	IFREG = 0100000,
	ROOT_INODE = 2
};

enum {
	DIRBLKSIZ = 512,
	DIRECT_HEADER = 8,
	DIRECT_INO = 0,
	DIRECT_RECLEN = 4,
	DIRECT_NAMLEN = 7
};

typedef struct {
	const raiju_image_io *io;
	uint32_t block_size;
	uint32_t fragment_size;
	uint32_t frags_per_block;
	uint32_t cylinder_groups;
	uint32_t inodes_per_group;
	uint32_t frags_per_group;
	uint32_t inodes_per_block;
	uint32_t pointers_per_block;
	uint32_t cg_header_frag;
	uint32_t inode_table_frag;
	uint32_t cg_size;
	uint64_t fragments;
} ufs2_volume;

typedef struct {
	uint16_t mode;
	uint64_t size;
	int64_t direct[NDADDR];
	int64_t indirect[NIADDR];
} ufs2_inode;

/**
 * Returns whether value is a non-zero power of two.
 */
static bool is_power_of_two(uint32_t value) {
	return value != 0 && (value & (value - 1)) == 0;
}

/**
 * Parses the superblock at SBLOCK_UFS2 and checks every field the walk uses,
 * so that later address arithmetic cannot overflow or leave the filesystem.
 */
static raiju_image_fs_status read_volume(const raiju_image_io *io, ufs2_volume *volume) {
	uint8_t sb[SB_BYTES];
	raiju_image_fs_status status = raiju_image_read(io, RAIJU_UFS2_SUPERBLOCK_OFFSET, sb, sizeof sb);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status == RAIJU_IMAGE_FS_CORRUPT ? RAIJU_IMAGE_FS_UNSUPPORTED : status;
	}
	if (raiju_read_le32(sb + SB_MAGIC) != RAIJU_UFS2_MAGIC) {
		return RAIJU_IMAGE_FS_UNSUPPORTED;
	}
	volume->io = io;
	volume->block_size = raiju_read_le32(sb + SB_BSIZE);
	volume->fragment_size = raiju_read_le32(sb + SB_FSIZE);
	volume->frags_per_block = raiju_read_le32(sb + SB_FRAG);
	volume->cylinder_groups = raiju_read_le32(sb + SB_NCG);
	volume->inodes_per_group = raiju_read_le32(sb + SB_IPG);
	volume->frags_per_group = raiju_read_le32(sb + SB_FPG);
	volume->inodes_per_block = raiju_read_le32(sb + SB_INOPB);
	volume->pointers_per_block = raiju_read_le32(sb + SB_NINDIR);
	volume->cg_header_frag = raiju_read_le32(sb + SB_CBLKNO);
	volume->inode_table_frag = raiju_read_le32(sb + SB_IBLKNO);
	volume->cg_size = raiju_read_le32(sb + SB_CGSIZE);
	volume->fragments = raiju_read_le64(sb + SB_SIZE);

	uint32_t block = volume->block_size;
	uint32_t fragment = volume->fragment_size;
	if (!is_power_of_two(block) || block < MIN_BLOCK_SIZE || block > MAX_BLOCK_SIZE || !is_power_of_two(fragment) ||
	    fragment < MIN_FRAGMENT_SIZE || fragment > block || block / fragment > MAX_FRAGS_PER_BLOCK ||
	    volume->frags_per_block != block / fragment || volume->inodes_per_block != block / INODE_BYTES ||
	    volume->pointers_per_block != block / sizeof(int64_t)) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	uint32_t frag = volume->frags_per_block;
	if (volume->cylinder_groups == 0 || volume->inodes_per_group == 0 ||
	    volume->inodes_per_group % volume->inodes_per_block != 0 || volume->frags_per_group < frag ||
	    volume->frags_per_group % frag != 0 || volume->cg_size < CG_HEADER_BYTES || volume->cg_size > block) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	uint64_t inode_table_frags = (uint64_t)(volume->inodes_per_group / volume->inodes_per_block) * frag;
	if ((uint64_t)volume->cg_header_frag + frag > volume->frags_per_group ||
	    (uint64_t)volume->inode_table_frag + inode_table_frags > volume->frags_per_group) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	uint64_t group_frags = (uint64_t)volume->cylinder_groups * volume->frags_per_group;
	if (volume->fragments == 0 || volume->fragments > group_frags || volume->fragments > UINT64_MAX / MAX_BLOCK_SIZE) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	return RAIJU_IMAGE_FS_OK;
}

/**
 * Reads length bytes of the run that starts at fragment address frag_addr,
 * offset bytes in. The run must lie inside the filesystem.
 */
static raiju_image_fs_status read_frags(const ufs2_volume *volume, uint64_t frag_addr, uint64_t offset, void *buffer,
                                        size_t length) {
	uint64_t fs_bytes = volume->fragments * volume->fragment_size;
	if (frag_addr >= volume->fragments) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	uint64_t start = frag_addr * volume->fragment_size;
	if (!raiju_range_fits(start, offset, fs_bytes) || !raiju_range_fits(start + offset, length, fs_bytes)) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	return raiju_image_read(volume->io, start + offset, buffer, length);
}

/**
 * Locates an inode through its cylinder group: checks the group header,
 * that the inode lies in the initialised part of the table and that the
 * group's used-inode map marks it allocated, then decodes it.
 */
static raiju_image_fs_status read_inode(const ufs2_volume *volume, uint64_t ino, ufs2_inode *inode) {
	uint64_t total_inodes = (uint64_t)volume->cylinder_groups * volume->inodes_per_group;
	if (ino < ROOT_INODE || ino >= total_inodes) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	uint32_t group = (uint32_t)(ino / volume->inodes_per_group);
	uint32_t index = (uint32_t)(ino % volume->inodes_per_group);
	uint64_t group_base = (uint64_t)group * volume->frags_per_group;

	uint8_t header[CG_HEADER_BYTES];
	raiju_image_fs_status status = read_frags(volume, group_base + volume->cg_header_frag, 0, header, sizeof header);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status;
	}
	uint32_t used_map = raiju_read_le32(header + CG_IUSEDOFF);
	if (raiju_read_le32(header + CG_MAGIC) != CG_MAGIC_VALUE || raiju_read_le32(header + CG_CGX) != group ||
	    index >= raiju_read_le32(header + CG_INITEDIBLK) || used_map >= volume->cg_size ||
	    index / 8 >= volume->cg_size - used_map) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	uint8_t used_byte;
	status = read_frags(volume, group_base + volume->cg_header_frag, used_map + index / 8, &used_byte, 1);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status;
	}
	if ((used_byte & (1u << (index % 8))) == 0) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}

	uint64_t table_frag = group_base + volume->inode_table_frag +
	                      (uint64_t)(index / volume->inodes_per_block) * volume->frags_per_block;
	uint8_t raw[INODE_BYTES];
	status = read_frags(volume, table_frag, (uint64_t)(index % volume->inodes_per_block) * INODE_BYTES, raw, sizeof raw);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status;
	}
	if (raiju_read_le16(raw + DI_NLINK) == 0) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	inode->mode = raiju_read_le16(raw + DI_MODE);
	inode->size = raiju_read_le64(raw + DI_SIZE);
	for (int i = 0; i < NDADDR; i++) {
		inode->direct[i] = (int64_t)raiju_read_le64(raw + DI_DB + i * 8);
	}
	for (int i = 0; i < NIADDR; i++) {
		inode->indirect[i] = (int64_t)raiju_read_le64(raw + DI_IB + i * 8);
	}
	return RAIJU_IMAGE_FS_OK;
}

/**
 * Reads entry index of the indirect block at block_addr. A zero address is a
 * hole and yields zero; negative addresses are rejected.
 */
static raiju_image_fs_status read_pointer(const ufs2_volume *volume, int64_t block_addr, uint64_t index,
                                          int64_t *out) {
	if (block_addr < 0) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	if (block_addr == 0) {
		*out = 0;
		return RAIJU_IMAGE_FS_OK;
	}
	uint8_t pointer[8];
	raiju_image_fs_status status = read_frags(volume, (uint64_t)block_addr, index * 8, pointer, sizeof pointer);
	if (status == RAIJU_IMAGE_FS_OK) {
		*out = (int64_t)raiju_read_le64(pointer);
	}
	return status;
}

/**
 * Maps a logical block of a file to its fragment address through the direct,
 * single indirect and double indirect pointers. Zero means a hole.
 */
static raiju_image_fs_status map_block(const ufs2_volume *volume, const ufs2_inode *inode, uint64_t lbn,
                                       int64_t *addr) {
	uint64_t per_block = volume->pointers_per_block;
	if (lbn < NDADDR) {
		*addr = inode->direct[lbn];
		return RAIJU_IMAGE_FS_OK;
	}
	lbn -= NDADDR;
	if (lbn < per_block) {
		return read_pointer(volume, inode->indirect[0], lbn, addr);
	}
	lbn -= per_block;
	if (lbn < per_block * per_block) {
		int64_t middle;
		raiju_image_fs_status status = read_pointer(volume, inode->indirect[1], lbn / per_block, &middle);
		if (status != RAIJU_IMAGE_FS_OK) {
			return status;
		}
		return read_pointer(volume, middle, lbn % per_block, addr);
	}
	return RAIJU_IMAGE_FS_TOO_LARGE;
}

/**
 * Reads length bytes of a file's data starting at offset. Holes read as
 * zero. The caller keeps offset + length within the inode size.
 */
static raiju_image_fs_status read_inode_data(const ufs2_volume *volume, const ufs2_inode *inode, uint64_t offset,
                                             uint8_t *buffer, size_t length) {
	size_t done = 0;
	while (done < length) {
		uint64_t position = offset + done;
		uint64_t lbn = position / volume->block_size;
		uint64_t within = position % volume->block_size;
		size_t chunk = volume->block_size - (size_t)within;
		if (chunk > length - done) {
			chunk = length - done;
		}
		int64_t addr;
		raiju_image_fs_status status = map_block(volume, inode, lbn, &addr);
		if (status != RAIJU_IMAGE_FS_OK) {
			return status;
		}
		if (addr < 0) {
			return RAIJU_IMAGE_FS_CORRUPT;
		}
		if (addr == 0) {
			memset(buffer + done, 0, chunk);
		} else {
			status = read_frags(volume, (uint64_t)addr, within, buffer + done, chunk);
			if (status != RAIJU_IMAGE_FS_OK) {
				return status;
			}
		}
		done += chunk;
	}
	return RAIJU_IMAGE_FS_OK;
}

/**
 * Searches one DIRBLKSIZ chunk of a directory for name, validating every
 * record length on the way. Sets ino to zero when the name is absent.
 */
static raiju_image_fs_status scan_dir_chunk(const uint8_t *chunk, const char *name, uint32_t *ino) {
	size_t name_length = strlen(name);
	size_t position = 0;
	*ino = 0;
	while (position < DIRBLKSIZ) {
		if (DIRBLKSIZ - position < DIRECT_HEADER) {
			return RAIJU_IMAGE_FS_CORRUPT;
		}
		const uint8_t *entry = chunk + position;
		uint32_t entry_ino = raiju_read_le32(entry + DIRECT_INO);
		size_t record_length = raiju_read_le16(entry + DIRECT_RECLEN);
		size_t entry_name_length = entry[DIRECT_NAMLEN];
		size_t minimum = (DIRECT_HEADER + entry_name_length + 1 + 3) & ~(size_t)3;
		if (record_length % 4 != 0 || record_length < minimum || record_length > DIRBLKSIZ - position) {
			return RAIJU_IMAGE_FS_CORRUPT;
		}
		if (entry_ino != 0 && entry_name_length == name_length &&
		    memcmp(entry + DIRECT_HEADER, name, name_length) == 0) {
			*ino = entry_ino;
			return RAIJU_IMAGE_FS_OK;
		}
		position += record_length;
	}
	return RAIJU_IMAGE_FS_OK;
}

/**
 * Looks name up in a directory inode and returns the inode it names.
 * Directories larger than RAIJU_IMAGE_DIR_MAX_BYTES are treated as corrupt.
 */
static raiju_image_fs_status find_entry(const ufs2_volume *volume, const ufs2_inode *directory, const char *name,
                                        ufs2_inode *found) {
	if ((directory->mode & IFMT) != IFDIR) {
		return RAIJU_IMAGE_FS_NOT_FOUND;
	}
	if (directory->size > RAIJU_IMAGE_DIR_MAX_BYTES || directory->size % DIRBLKSIZ != 0) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	uint8_t chunk[DIRBLKSIZ];
	for (uint64_t offset = 0; offset < directory->size; offset += DIRBLKSIZ) {
		raiju_image_fs_status status = read_inode_data(volume, directory, offset, chunk, sizeof chunk);
		if (status != RAIJU_IMAGE_FS_OK) {
			return status;
		}
		uint32_t ino;
		status = scan_dir_chunk(chunk, name, &ino);
		if (status != RAIJU_IMAGE_FS_OK) {
			return status;
		}
		if (ino != 0) {
			return read_inode(volume, ino, found);
		}
	}
	return RAIJU_IMAGE_FS_NOT_FOUND;
}

raiju_image_fs_status raiju_ufs2_read_param_json(const raiju_image_io *io, uint8_t **out, size_t *out_len) {
	if (io == NULL || io->read_at == NULL || out == NULL || out_len == NULL) {
		return RAIJU_IMAGE_FS_IO_ERROR;
	}
	*out = NULL;
	*out_len = 0;
	ufs2_volume volume;
	raiju_image_fs_status status = read_volume(io, &volume);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status;
	}
	ufs2_inode root;
	status = read_inode(&volume, ROOT_INODE, &root);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status;
	}
	if ((root.mode & IFMT) != IFDIR) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	ufs2_inode sce_sys;
	status = find_entry(&volume, &root, "sce_sys", &sce_sys);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status;
	}
	ufs2_inode param_json;
	status = find_entry(&volume, &sce_sys, "param.json", &param_json);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status;
	}
	if ((param_json.mode & IFMT) != IFREG) {
		return RAIJU_IMAGE_FS_NOT_FOUND;
	}
	if (param_json.size > RAIJU_PARAM_JSON_MAX_BYTES) {
		return RAIJU_IMAGE_FS_TOO_LARGE;
	}
	size_t length = (size_t)param_json.size;
	uint8_t *buffer = malloc(length > 0 ? length : 1);
	if (buffer == NULL) {
		return RAIJU_IMAGE_FS_NO_MEMORY;
	}
	status = read_inode_data(&volume, &param_json, 0, buffer, length);
	if (status != RAIJU_IMAGE_FS_OK) {
		free(buffer);
		return status;
	}
	*out = buffer;
	*out_len = length;
	return RAIJU_IMAGE_FS_OK;
}
