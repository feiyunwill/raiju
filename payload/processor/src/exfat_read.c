#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "byte_order.h"
#include "image_read.h"
#include "raiju/image_fs.h"
#include "raiju/title_id.h"

enum {
	BOOT_SECTOR_BYTES = 512,
	BOOT_FILE_SYSTEM_NAME = 3,
	BOOT_VOLUME_LENGTH = 72,
	BOOT_FAT_OFFSET = 80,
	BOOT_FAT_LENGTH = 84,
	BOOT_CLUSTER_HEAP_OFFSET = 88,
	BOOT_CLUSTER_COUNT = 92,
	BOOT_ROOT_CLUSTER = 96,
	BOOT_VOLUME_FLAGS = 106,
	BOOT_BYTES_PER_SECTOR_SHIFT = 108,
	BOOT_SECTORS_PER_CLUSTER_SHIFT = 109,
	BOOT_NUMBER_OF_FATS = 110,
	BOOT_SIGNATURE = 510,
	MIN_SECTOR_SHIFT = 9,
	MAX_SECTOR_SHIFT = 12,
	MAX_CLUSTER_BYTES_SHIFT = 25,
	MIN_FAT_OFFSET_SECTORS = 24,
	VOLUME_FLAG_ACTIVE_FAT = 0x01
};

enum {
	ENTRY_BYTES = 32,
	ENTRY_END_OF_DIRECTORY = 0x00,
	ENTRY_FILE = 0x85,
	ENTRY_STREAM_EXTENSION = 0xC0,
	ENTRY_FILE_NAME = 0xC1,
	FILE_SECONDARY_COUNT = 1,
	FILE_SET_CHECKSUM = 2,
	FILE_ATTRIBUTES = 4,
	ATTRIBUTE_DIRECTORY = 0x10,
	MIN_SECONDARY_COUNT = 2,
	MAX_SECONDARY_COUNT = 18,
	STREAM_FLAGS = 1,
	STREAM_NAME_LENGTH = 3,
	STREAM_VALID_DATA_LENGTH = 8,
	STREAM_FIRST_CLUSTER = 20,
	STREAM_DATA_LENGTH = 24,
	STREAM_FLAG_ALLOCATION_POSSIBLE = 0x01,
	STREAM_FLAG_NO_FAT_CHAIN = 0x02,
	NAME_UNITS_PER_ENTRY = 15,
	NAME_OFFSET = 2
};

#define FIRST_DATA_CLUSTER 2u
#define MAX_CLUSTER_COUNT 0xFFFFFFF5u
#define FAT_END_OF_CHAIN 0xFFFFFFFFu

typedef struct {
	const raiju_image_io *io;
	unsigned cluster_shift;
	uint64_t fat_offset;
	uint64_t heap_offset;
	uint32_t cluster_count;
	uint32_t root_cluster;
} exfat_volume;

typedef struct {
	uint32_t first_cluster;
	uint64_t length;
	uint64_t valid_length;
	bool no_fat_chain;
	bool is_directory;
} exfat_extent;

typedef struct {
	const exfat_volume *volume;
	exfat_extent extent;
	uint32_t cluster;
	uint32_t clusters_visited;
	uint64_t position;
	uint64_t limit;
} exfat_stream;

/**
 * Returns whether cluster is a valid index into the cluster heap.
 */
static bool cluster_in_heap(const exfat_volume *volume, uint32_t cluster) {
	return cluster >= FIRST_DATA_CLUSTER && cluster - FIRST_DATA_CLUSTER < volume->cluster_count;
}

/**
 * Parses and validates the main boot sector, rejecting any geometry that
 * would place the FAT or the cluster heap outside the volume.
 */
static raiju_image_fs_status read_volume(const raiju_image_io *io, exfat_volume *volume) {
	uint8_t boot[BOOT_SECTOR_BYTES];
	raiju_image_fs_status status = raiju_image_read(io, 0, boot, sizeof boot);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status == RAIJU_IMAGE_FS_CORRUPT ? RAIJU_IMAGE_FS_UNSUPPORTED : status;
	}
	if (memcmp(boot + BOOT_FILE_SYSTEM_NAME, "EXFAT   ", 8) != 0 || raiju_read_le16(boot + BOOT_SIGNATURE) != 0xAA55) {
		return RAIJU_IMAGE_FS_UNSUPPORTED;
	}
	unsigned sector_shift = boot[BOOT_BYTES_PER_SECTOR_SHIFT];
	unsigned per_cluster_shift = boot[BOOT_SECTORS_PER_CLUSTER_SHIFT];
	unsigned fats = boot[BOOT_NUMBER_OF_FATS];
	if (sector_shift < MIN_SECTOR_SHIFT || sector_shift > MAX_SECTOR_SHIFT ||
	    per_cluster_shift > MAX_CLUSTER_BYTES_SHIFT - sector_shift || (fats != 1 && fats != 2)) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	uint64_t volume_sectors = raiju_read_le64(boot + BOOT_VOLUME_LENGTH);
	uint64_t fat_offset = raiju_read_le32(boot + BOOT_FAT_OFFSET);
	uint64_t fat_length = raiju_read_le32(boot + BOOT_FAT_LENGTH);
	uint64_t heap_offset = raiju_read_le32(boot + BOOT_CLUSTER_HEAP_OFFSET);
	uint32_t cluster_count = raiju_read_le32(boot + BOOT_CLUSTER_COUNT);
	uint32_t root_cluster = raiju_read_le32(boot + BOOT_ROOT_CLUSTER);
	if (volume_sectors > (UINT64_MAX >> MAX_CLUSTER_BYTES_SHIFT) || cluster_count == 0 ||
	    cluster_count > MAX_CLUSTER_COUNT - FIRST_DATA_CLUSTER || fat_offset < MIN_FAT_OFFSET_SECTORS) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	unsigned cluster_shift = sector_shift + per_cluster_shift;
	uint64_t volume_bytes = volume_sectors << sector_shift;
	uint64_t fat_bytes = fat_length << sector_shift;
	uint64_t fats_end = (fat_offset + fat_length * fats) << sector_shift;
	uint64_t heap_bytes = (uint64_t)cluster_count << cluster_shift;
	if (fat_bytes < ((uint64_t)cluster_count + FIRST_DATA_CLUSTER) * 4 || heap_offset << sector_shift < fats_end ||
	    !raiju_range_fits(heap_offset << sector_shift, heap_bytes, volume_bytes)) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	volume->io = io;
	volume->cluster_shift = cluster_shift;
	volume->fat_offset = fat_offset << sector_shift;
	if (fats == 2 && (boot[BOOT_VOLUME_FLAGS] & VOLUME_FLAG_ACTIVE_FAT)) {
		volume->fat_offset += fat_bytes;
	}
	volume->heap_offset = heap_offset << sector_shift;
	volume->cluster_count = cluster_count;
	volume->root_cluster = root_cluster;
	return cluster_in_heap(volume, root_cluster) ? RAIJU_IMAGE_FS_OK : RAIJU_IMAGE_FS_CORRUPT;
}

/**
 * Looks up the FAT entry that follows cluster in its chain.
 */
static raiju_image_fs_status read_fat_entry(const exfat_volume *volume, uint32_t cluster, uint32_t *next) {
	uint8_t entry[4];
	raiju_image_fs_status status =
	    raiju_image_read(volume->io, volume->fat_offset + (uint64_t)cluster * 4, entry, sizeof entry);
	if (status == RAIJU_IMAGE_FS_OK) {
		*next = raiju_read_le32(entry);
	}
	return status;
}

/**
 * Checks an extent's first cluster and, for contiguous files, that the whole
 * run lies inside the cluster heap.
 */
static bool extent_sane(const exfat_volume *volume, const exfat_extent *extent) {
	if (extent->valid_length > extent->length) {
		return false;
	}
	if (extent->length == 0) {
		return true;
	}
	if (!cluster_in_heap(volume, extent->first_cluster)) {
		return false;
	}
	if (extent->no_fat_chain) {
		uint64_t clusters = ((extent->length - 1) >> volume->cluster_shift) + 1;
		return clusters <= volume->cluster_count - (extent->first_cluster - FIRST_DATA_CLUSTER);
	}
	return true;
}

/**
 * Walks the FAT chain of a chained extent for exactly the clusters its length
 * needs and checks that the chain ends there. A chain that loops never ends,
 * so this also rejects loops.
 */
static raiju_image_fs_status verify_chain(const exfat_volume *volume, const exfat_extent *extent) {
	if (extent->no_fat_chain || extent->length == 0) {
		return RAIJU_IMAGE_FS_OK;
	}
	uint64_t clusters = ((extent->length - 1) >> volume->cluster_shift) + 1;
	if (clusters > volume->cluster_count) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	uint32_t cluster = extent->first_cluster;
	for (uint64_t visited = 1;; visited++) {
		uint32_t next;
		raiju_image_fs_status status = read_fat_entry(volume, cluster, &next);
		if (status != RAIJU_IMAGE_FS_OK) {
			return status;
		}
		if (visited == clusters) {
			return next == FAT_END_OF_CHAIN ? RAIJU_IMAGE_FS_OK : RAIJU_IMAGE_FS_CORRUPT;
		}
		if (!cluster_in_heap(volume, next)) {
			return RAIJU_IMAGE_FS_CORRUPT;
		}
		cluster = next;
	}
}

/**
 * Starts a sequential reader over an extent, reading at most limit bytes.
 */
static void stream_open(exfat_stream *stream, const exfat_volume *volume, const exfat_extent *extent,
                        uint64_t limit) {
	stream->volume = volume;
	stream->extent = *extent;
	stream->cluster = extent->first_cluster;
	stream->clusters_visited = 1;
	stream->position = 0;
	stream->limit = limit;
}

/**
 * Moves the stream to the next cluster of its extent. Sets end when a FAT
 * chain terminates. Rejects bad clusters and chains longer than the heap,
 * which can only happen when the chain loops.
 */
static raiju_image_fs_status stream_advance(exfat_stream *stream, bool *end) {
	const exfat_volume *volume = stream->volume;
	if (stream->clusters_visited >= volume->cluster_count) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	uint32_t next;
	if (stream->extent.no_fat_chain) {
		next = stream->cluster + 1;
	} else {
		raiju_image_fs_status status = read_fat_entry(volume, stream->cluster, &next);
		if (status != RAIJU_IMAGE_FS_OK) {
			return status;
		}
		if (next == FAT_END_OF_CHAIN) {
			*end = true;
			return RAIJU_IMAGE_FS_OK;
		}
	}
	if (!cluster_in_heap(volume, next)) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	stream->cluster = next;
	stream->clusters_visited++;
	return RAIJU_IMAGE_FS_OK;
}

/**
 * Reads length bytes from the stream, following the cluster chain. Sets end
 * and reads nothing when the stream is exhausted before the read starts; a
 * stream that ends in the middle of a read is corrupt.
 */
static raiju_image_fs_status stream_read(exfat_stream *stream, uint8_t *buffer, size_t length, bool *end) {
	*end = false;
	if (stream->position >= stream->limit) {
		*end = true;
		return RAIJU_IMAGE_FS_OK;
	}
	if (length > stream->limit - stream->position) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	uint64_t cluster_size = (uint64_t)1 << stream->volume->cluster_shift;
	size_t done = 0;
	while (done < length) {
		uint64_t within = stream->position & (cluster_size - 1);
		if (within == 0 && stream->position > 0) {
			bool chain_end = false;
			raiju_image_fs_status status = stream_advance(stream, &chain_end);
			if (status != RAIJU_IMAGE_FS_OK) {
				return status;
			}
			if (chain_end) {
				if (done > 0 || stream->extent.length > 0) {
					return RAIJU_IMAGE_FS_CORRUPT;
				}
				*end = true;
				return RAIJU_IMAGE_FS_OK;
			}
		}
		uint64_t chunk = cluster_size - within;
		if (chunk > length - done) {
			chunk = length - done;
		}
		uint64_t offset = stream->volume->heap_offset +
		                  ((uint64_t)(stream->cluster - FIRST_DATA_CLUSTER) << stream->volume->cluster_shift) + within;
		raiju_image_fs_status status = raiju_image_read(stream->volume->io, offset, buffer + done, (size_t)chunk);
		if (status != RAIJU_IMAGE_FS_OK) {
			return status;
		}
		done += (size_t)chunk;
		stream->position += chunk;
	}
	return RAIJU_IMAGE_FS_OK;
}

/**
 * Computes the entry set checksum over every byte of the set except the
 * SetChecksum field of the primary entry.
 */
static uint16_t entry_set_checksum(const uint8_t *set, size_t length) {
	uint16_t checksum = 0;
	for (size_t i = 0; i < length; i++) {
		if (i == FILE_SET_CHECKSUM || i == FILE_SET_CHECKSUM + 1) {
			continue;
		}
		checksum = (uint16_t)(((checksum & 1u) ? 0x8000u : 0u) + (checksum >> 1) + set[i]);
	}
	return checksum;
}

/**
 * Folds an ASCII letter to uppercase; other values are returned unchanged.
 */
static uint16_t ascii_upper(uint16_t unit) {
	return (unit >= 'a' && unit <= 'z') ? (uint16_t)(unit - 'a' + 'A') : unit;
}

/**
 * Compares the UTF-16 name held by a set's File Name entries with an ASCII
 * name, folding ASCII case only.
 */
static bool set_name_matches(const uint8_t *set, unsigned name_length, const char *name) {
	if (strlen(name) != name_length) {
		return false;
	}
	for (unsigned i = 0; i < name_length; i++) {
		const uint8_t *entry = set + (size_t)(2 + i / NAME_UNITS_PER_ENTRY) * ENTRY_BYTES;
		uint16_t unit = raiju_read_le16(entry + NAME_OFFSET + (i % NAME_UNITS_PER_ENTRY) * 2);
		if (unit >= 0x80 || ascii_upper(unit) != ascii_upper((uint8_t)name[i])) {
			return false;
		}
	}
	return true;
}

/**
 * Validates a complete File entry set (types, counts and checksum) and fills
 * the extent from its Stream Extension entry. Sets matched when the name
 * equals name.
 */
static raiju_image_fs_status parse_entry_set(const exfat_volume *volume, const uint8_t *set, unsigned secondary_count,
                                             const char *name, bool *matched, exfat_extent *extent) {
	size_t set_length = (size_t)(secondary_count + 1) * ENTRY_BYTES;
	if (entry_set_checksum(set, set_length) != raiju_read_le16(set + FILE_SET_CHECKSUM)) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	const uint8_t *stream = set + ENTRY_BYTES;
	unsigned name_length = stream[STREAM_NAME_LENGTH];
	unsigned name_entries = (name_length + NAME_UNITS_PER_ENTRY - 1) / NAME_UNITS_PER_ENTRY;
	if (stream[0] != ENTRY_STREAM_EXTENSION || name_length == 0 || name_entries > secondary_count - 1) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	for (unsigned i = 0; i < name_entries; i++) {
		if (set[(size_t)(2 + i) * ENTRY_BYTES] != ENTRY_FILE_NAME) {
			return RAIJU_IMAGE_FS_CORRUPT;
		}
	}
	*matched = set_name_matches(set, name_length, name);
	if (!*matched) {
		return RAIJU_IMAGE_FS_OK;
	}
	uint8_t flags = stream[STREAM_FLAGS];
	extent->first_cluster = raiju_read_le32(stream + STREAM_FIRST_CLUSTER);
	extent->length = raiju_read_le64(stream + STREAM_DATA_LENGTH);
	extent->valid_length = raiju_read_le64(stream + STREAM_VALID_DATA_LENGTH);
	extent->no_fat_chain = (flags & STREAM_FLAG_NO_FAT_CHAIN) != 0;
	extent->is_directory = (raiju_read_le16(set + FILE_ATTRIBUTES) & ATTRIBUTE_DIRECTORY) != 0;
	bool allocated = (flags & STREAM_FLAG_ALLOCATION_POSSIBLE) != 0;
	if ((extent->length > 0 && !allocated) || !extent_sane(volume, extent)) {
		return RAIJU_IMAGE_FS_CORRUPT;
	}
	return RAIJU_IMAGE_FS_OK;
}

/**
 * Scans a directory for an entry named name. Directories are read through at
 * most RAIJU_IMAGE_DIR_MAX_BYTES; a longer one is treated as corrupt.
 */
static raiju_image_fs_status find_entry(const exfat_volume *volume, const exfat_extent *directory, const char *name,
                                        exfat_extent *found) {
	uint64_t limit = directory->length;
	bool open_ended = directory->length == 0 && directory->first_cluster != 0;
	if (open_ended) {
		limit = RAIJU_IMAGE_DIR_MAX_BYTES;
	} else if (limit > RAIJU_IMAGE_DIR_MAX_BYTES) {
		return RAIJU_IMAGE_FS_CORRUPT;
	} else {
		raiju_image_fs_status status = verify_chain(volume, directory);
		if (status != RAIJU_IMAGE_FS_OK) {
			return status;
		}
	}
	exfat_stream stream;
	stream_open(&stream, volume, directory, limit);
	uint8_t set[(MAX_SECONDARY_COUNT + 1) * ENTRY_BYTES];
	for (;;) {
		bool end = false;
		raiju_image_fs_status status = stream_read(&stream, set, ENTRY_BYTES, &end);
		if (status != RAIJU_IMAGE_FS_OK) {
			return status;
		}
		if (end) {
			return open_ended && stream.position >= limit ? RAIJU_IMAGE_FS_CORRUPT : RAIJU_IMAGE_FS_NOT_FOUND;
		}
		if (set[0] == ENTRY_END_OF_DIRECTORY) {
			return RAIJU_IMAGE_FS_NOT_FOUND;
		}
		if (set[0] != ENTRY_FILE) {
			continue;
		}
		unsigned secondary_count = set[FILE_SECONDARY_COUNT];
		if (secondary_count < MIN_SECONDARY_COUNT || secondary_count > MAX_SECONDARY_COUNT) {
			return RAIJU_IMAGE_FS_CORRUPT;
		}
		status = stream_read(&stream, set + ENTRY_BYTES, (size_t)secondary_count * ENTRY_BYTES, &end);
		if (status != RAIJU_IMAGE_FS_OK) {
			return status;
		}
		if (end) {
			return RAIJU_IMAGE_FS_CORRUPT;
		}
		bool matched = false;
		status = parse_entry_set(volume, set, secondary_count, name, &matched, found);
		if (status != RAIJU_IMAGE_FS_OK || matched) {
			return status;
		}
	}
}

/**
 * Reads a whole file into a new buffer. Bytes past ValidDataLength read as
 * zero, as the exFAT specification requires.
 */
static raiju_image_fs_status read_file(const exfat_volume *volume, const exfat_extent *file, uint8_t **out,
                                       size_t *out_len) {
	if (file->length > RAIJU_PARAM_JSON_MAX_BYTES) {
		return RAIJU_IMAGE_FS_TOO_LARGE;
	}
	raiju_image_fs_status status = verify_chain(volume, file);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status;
	}
	size_t length = (size_t)file->length;
	uint8_t *buffer = calloc(1, length > 0 ? length : 1);
	if (buffer == NULL) {
		return RAIJU_IMAGE_FS_NO_MEMORY;
	}
	exfat_stream stream;
	stream_open(&stream, volume, file, file->length);
	bool end = false;
	status = stream_read(&stream, buffer, (size_t)file->valid_length, &end);
	if (status == RAIJU_IMAGE_FS_OK && end && file->valid_length > 0) {
		status = RAIJU_IMAGE_FS_CORRUPT;
	}
	if (status != RAIJU_IMAGE_FS_OK) {
		free(buffer);
		return status;
	}
	*out = buffer;
	*out_len = length;
	return RAIJU_IMAGE_FS_OK;
}

raiju_image_fs_status raiju_exfat_read_param_json(const raiju_image_io *io, uint8_t **out, size_t *out_len) {
	if (io == NULL || io->read_at == NULL || out == NULL || out_len == NULL) {
		return RAIJU_IMAGE_FS_IO_ERROR;
	}
	*out = NULL;
	*out_len = 0;
	exfat_volume volume;
	raiju_image_fs_status status = read_volume(io, &volume);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status;
	}
	exfat_extent root = {volume.root_cluster, 0, 0, false, true};
	exfat_extent sce_sys;
	status = find_entry(&volume, &root, "sce_sys", &sce_sys);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status;
	}
	if (!sce_sys.is_directory || sce_sys.length == 0) {
		return RAIJU_IMAGE_FS_NOT_FOUND;
	}
	exfat_extent param_json;
	status = find_entry(&volume, &sce_sys, "param.json", &param_json);
	if (status != RAIJU_IMAGE_FS_OK) {
		return status;
	}
	if (param_json.is_directory) {
		return RAIJU_IMAGE_FS_NOT_FOUND;
	}
	return read_file(&volume, &param_json, out, out_len);
}
