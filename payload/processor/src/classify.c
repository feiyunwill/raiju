#include "raiju/classify.h"

#include <string.h>

#include "byte_order.h"

typedef struct {
	raiju_kind kind;
	size_t offset;
	const uint8_t *bytes;
	size_t length;
} magic_signature;

static const uint8_t sevenzip_magic[] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
static const uint8_t zip_magic[] = {0x50, 0x4B, 0x03, 0x04};
static const uint8_t rar_magic[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07};
static const uint8_t zstd_magic[] = {0x28, 0xB5, 0x2F, 0xFD};
static const uint8_t xz_magic[] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};
static const uint8_t gzip_magic[] = {0x1F, 0x8B};
static const uint8_t exfat_magic[] = {'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};

static const magic_signature signatures[] = {
	{RAIJU_KIND_7Z, 0, sevenzip_magic, sizeof sevenzip_magic},
	{RAIJU_KIND_ZIP, 0, zip_magic, sizeof zip_magic},
	{RAIJU_KIND_RAR, 0, rar_magic, sizeof rar_magic},
	{RAIJU_KIND_ZSTD, 0, zstd_magic, sizeof zstd_magic},
	{RAIJU_KIND_XZ, 0, xz_magic, sizeof xz_magic},
	{RAIJU_KIND_GZIP, 0, gzip_magic, sizeof gzip_magic},
	{RAIJU_KIND_EXFAT, 3, exfat_magic, sizeof exfat_magic},
};

typedef struct {
	const char *extension;
	raiju_kind kind;
} image_extension;

static const image_extension image_extensions[] = {
	{".exfat", RAIJU_KIND_EXFAT},
	{".ffpkg", RAIJU_KIND_UFS2},
	{".ffpfs", RAIJU_KIND_PFS},
	{".ffpfsc", RAIJU_KIND_PFSC},
};

/**
 * Lowercases one ASCII letter, leaving every other byte unchanged.
 */
static char ascii_lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/**
 * Returns whether name ends with suffix, ignoring ASCII case.
 */
static bool ends_with_ignore_case(const char *name, const char *suffix) {
	size_t name_len = strlen(name);
	size_t suffix_len = strlen(suffix);
	if (suffix_len > name_len) {
		return false;
	}
	const char *tail = name + (name_len - suffix_len);
	for (size_t i = 0; i < suffix_len; i++) {
		if (ascii_lower(tail[i]) != suffix[i]) {
			return false;
		}
	}
	return true;
}

raiju_kind raiju_detect_magic(const uint8_t *head, size_t head_len) {
	if (head == NULL) {
		return RAIJU_KIND_UNKNOWN;
	}
	for (size_t i = 0; i < sizeof signatures / sizeof signatures[0]; i++) {
		const magic_signature *sig = &signatures[i];
		if (head_len >= sig->offset + sig->length && memcmp(head + sig->offset, sig->bytes, sig->length) == 0) {
			return sig->kind;
		}
	}
	if (head_len >= RAIJU_UFS2_MAGIC_OFFSET + 4 && raiju_read_le32(head + RAIJU_UFS2_MAGIC_OFFSET) == RAIJU_UFS2_MAGIC) {
		return RAIJU_KIND_UFS2;
	}
	return RAIJU_KIND_UNKNOWN;
}

raiju_kind raiju_image_kind_from_name(const char *file_name) {
	if (file_name == NULL) {
		return RAIJU_KIND_UNKNOWN;
	}
	for (size_t i = 0; i < sizeof image_extensions / sizeof image_extensions[0]; i++) {
		if (ends_with_ignore_case(file_name, image_extensions[i].extension)) {
			return image_extensions[i].kind;
		}
	}
	return RAIJU_KIND_UNKNOWN;
}

raiju_category raiju_kind_category(raiju_kind kind) {
	switch (kind) {
	case RAIJU_KIND_7Z:
	case RAIJU_KIND_ZIP:
	case RAIJU_KIND_RAR:
	case RAIJU_KIND_ZSTD:
	case RAIJU_KIND_XZ:
	case RAIJU_KIND_GZIP:
		return RAIJU_CATEGORY_ARCHIVE;
	case RAIJU_KIND_EXFAT:
	case RAIJU_KIND_UFS2:
	case RAIJU_KIND_PFS:
	case RAIJU_KIND_PFSC:
		return RAIJU_CATEGORY_IMAGE;
	case RAIJU_KIND_UNKNOWN:
	default:
		return RAIJU_CATEGORY_OTHER;
	}
}

const char *raiju_kind_name(raiju_kind kind) {
	switch (kind) {
	case RAIJU_KIND_7Z:
		return "7z";
	case RAIJU_KIND_ZIP:
		return "zip";
	case RAIJU_KIND_RAR:
		return "rar";
	case RAIJU_KIND_ZSTD:
		return "zstd";
	case RAIJU_KIND_XZ:
		return "xz";
	case RAIJU_KIND_GZIP:
		return "gzip";
	case RAIJU_KIND_EXFAT:
		return "exfat";
	case RAIJU_KIND_UFS2:
		return "ufs2";
	case RAIJU_KIND_PFS:
		return "pfs";
	case RAIJU_KIND_PFSC:
		return "pfsc";
	case RAIJU_KIND_UNKNOWN:
	default:
		return "unknown";
	}
}

raiju_classification raiju_classify(const uint8_t *head, size_t head_len, const char *file_name) {
	raiju_classification result = {RAIJU_KIND_UNKNOWN, RAIJU_CATEGORY_OTHER, false, true};
	raiju_kind promised = raiju_image_kind_from_name(file_name);
	if (promised == RAIJU_KIND_PFS || promised == RAIJU_KIND_PFSC) {
		result.kind = promised;
		result.category = RAIJU_CATEGORY_IMAGE;
		result.header_checked = false;
		return result;
	}
	raiju_kind detected = raiju_detect_magic(head, head_len);
	if (promised != RAIJU_KIND_UNKNOWN) {
		result.kind = promised;
		result.category = RAIJU_CATEGORY_IMAGE;
		result.header_mismatch = detected != promised;
		return result;
	}
	result.kind = detected;
	result.category = raiju_kind_category(detected);
	return result;
}
