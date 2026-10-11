#ifndef RAIJU_ARCHIVE_BUILDER_H
#define RAIJU_ARCHIVE_BUILDER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	FORMAT_ZIP = 0,
	FORMAT_7Z,
	FORMAT_TAR_ZSTD,
	FORMAT_TAR_XZ,
	FORMAT_TAR_GZ,
	FORMAT_COUNT
} fixture_format;

/**
 * One archive entry to write: its path, libarchive file type (AE_IFREG,
 * AE_IFDIR, AE_IFLNK...), contents, and the target of a symlink or hardlink.
 */
typedef struct {
	const char *path;
	unsigned type;
	const void *data;
	size_t length;
	const char *link;
} fixture_entry;

extern const char *const archive_format_names[FORMAT_COUNT];

/**
 * Writes a fixture archive with libarchive's write API. Returns 0 on
 * success, 1 when the format is not writable on this host and -1 on error.
 */
int archive_builder_write(const char *path, fixture_format format, const fixture_entry *entries, size_t count);

/**
 * Same as archive_builder_write, with 7z written uncompressed so its header
 * stays plain instead of LZMA-encoded. A reader that walks the header with
 * range requests, as the gateway probe does, can only read the plain form.
 */
int archive_builder_write_ex(const char *path, fixture_format format, const fixture_entry *entries, size_t count,
                             bool plain_sevenz);

#endif
