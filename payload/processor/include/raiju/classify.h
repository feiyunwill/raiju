#ifndef RAIJU_CLASSIFY_H
#define RAIJU_CLASSIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAIJU_UFS2_SUPERBLOCK_OFFSET 65536u
#define RAIJU_UFS2_MAGIC_FIELD 1372u
#define RAIJU_UFS2_SBLOCKLOC_FIELD 1000u
#define RAIJU_UFS2_MAGIC_OFFSET (RAIJU_UFS2_SUPERBLOCK_OFFSET + RAIJU_UFS2_MAGIC_FIELD)
#define RAIJU_UFS2_MAGIC 0x19540119u
#define RAIJU_UFS2_SUPERBLOCK_PROBE 1376u
#define RAIJU_UFS2_SUPERBLOCK_SEARCH_COUNT 4u
#define RAIJU_UFS2_SUPERBLOCK_SEARCH_MAX 262144u
#define RAIJU_PROBE_SIZE (RAIJU_UFS2_SUPERBLOCK_SEARCH_MAX + RAIJU_UFS2_SUPERBLOCK_PROBE)
#define RAIJU_UFS2_NOT_FOUND UINT32_MAX

extern const uint32_t raiju_ufs2_superblock_search[RAIJU_UFS2_SUPERBLOCK_SEARCH_COUNT];

typedef enum {
	RAIJU_KIND_UNKNOWN = 0,
	RAIJU_KIND_7Z,
	RAIJU_KIND_ZIP,
	RAIJU_KIND_RAR,
	RAIJU_KIND_ZSTD,
	RAIJU_KIND_XZ,
	RAIJU_KIND_GZIP,
	RAIJU_KIND_EXFAT,
	RAIJU_KIND_UFS2,
	RAIJU_KIND_PFS,
	RAIJU_KIND_PFSC
} raiju_kind;

typedef enum {
	RAIJU_CATEGORY_OTHER = 0,
	RAIJU_CATEGORY_ARCHIVE,
	RAIJU_CATEGORY_IMAGE
} raiju_category;

/**
 * Outcome of classifying a download. When header_mismatch is set the file
 * claims to be an image by its extension but its bytes disagree, and the job
 * must fail instead of installing it. header_checked is false for kinds that
 * are recognised by extension only (.ffpfs, .ffpfsc).
 */
typedef struct {
	raiju_kind kind;
	raiju_category category;
	bool header_mismatch;
	bool header_checked;
} raiju_classification;

/**
 * Detects the kind of a file from its leading bytes. head must hold the start
 * of the file; pass at least RAIJU_PROBE_SIZE bytes to recognise UFS2 images.
 * Returns RAIJU_KIND_UNKNOWN when no signature matches.
 */
raiju_kind raiju_detect_magic(const uint8_t *head, size_t head_len);

/**
 * Maps an image file extension (.exfat, .ffpkg, .ffpfs, .ffpfsc, compared
 * case-insensitively) to the kind it promises. Returns RAIJU_KIND_UNKNOWN for
 * any other name.
 */
raiju_kind raiju_image_kind_from_name(const char *file_name);

/**
 * Returns whether a kind is an archive, an image, or something else.
 */
raiju_category raiju_kind_category(raiju_kind kind);

/**
 * Returns a stable lowercase name for a kind, for logs and the API.
 */
const char *raiju_kind_name(raiju_kind kind);

/**
 * Classifies a download from its leading bytes and its file name. Magic bytes
 * decide, except that a name ending in an image extension must be backed by
 * that image's signature.
 */
raiju_classification raiju_classify(const uint8_t *head, size_t head_len, const char *file_name);

/**
 * Returns true when a UFS2 superblock sits at offset in the given bytes: the
 * magic matches and fs_sblockloc records that same offset, which is how FreeBSD
 * tells a primary superblock from a backup copy.
 */
bool raiju_ufs2_superblock_at(const uint8_t *bytes, size_t len, uint32_t offset);

/**
 * Searches the head of an image for its UFS2 superblock in FreeBSD's
 * SBLOCKSEARCH order (65536, 8192, 0, 262144) and returns its offset, or
 * RAIJU_UFS2_NOT_FOUND.
 */
uint32_t raiju_find_ufs2_superblock(const uint8_t *head, size_t head_len);

#endif
