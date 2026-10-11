#include "archive_builder.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdbool.h>

const char *const archive_format_names[FORMAT_COUNT] = {"zip", "7z", "tar.zst", "tar.xz", "tar.gz"};

/**
 * Configures an archive writer for a fixture format. Returns false when the
 * host libarchive cannot write it natively.
 */
static bool configure_writer(struct archive *writer, fixture_format format) {
	switch (format) {
	case FORMAT_ZIP:
		return archive_write_set_format_zip(writer) == ARCHIVE_OK;
	case FORMAT_7Z:
		return archive_write_set_format_7zip(writer) == ARCHIVE_OK;
	case FORMAT_TAR_ZSTD:
		return archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK &&
		       archive_write_add_filter_zstd(writer) == ARCHIVE_OK;
	case FORMAT_TAR_XZ:
		return archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK &&
		       archive_write_add_filter_xz(writer) == ARCHIVE_OK;
	case FORMAT_TAR_GZ:
		return archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK &&
		       archive_write_add_filter_gzip(writer) == ARCHIVE_OK;
	case FORMAT_COUNT:
		break;
	}
	return false;
}

int archive_builder_write(const char *path, fixture_format format, const fixture_entry *entries, size_t count) {
	struct archive *writer = archive_write_new();
	if (writer == NULL) {
		return -1;
	}
	if (!configure_writer(writer, format)) {
		archive_write_free(writer);
		return 1;
	}
	if (archive_write_open_filename(writer, path) != ARCHIVE_OK) {
		archive_write_free(writer);
		return -1;
	}
	int status = 0;
	for (size_t i = 0; i < count && status == 0; i++) {
		struct archive_entry *entry = archive_entry_new();
		archive_entry_set_pathname(entry, entries[i].path);
		archive_entry_set_filetype(entry, entries[i].type);
		archive_entry_set_perm(entry, entries[i].type == AE_IFDIR ? 0755 : 0644);
		archive_entry_set_mtime(entry, 1700000000, 0);
		if (entries[i].type == AE_IFLNK) {
			archive_entry_set_symlink(entry, entries[i].link);
		} else if (entries[i].link != NULL) {
			archive_entry_set_hardlink(entry, entries[i].link);
		}
		if (entries[i].type == AE_IFREG) {
			archive_entry_set_size(entry, (la_int64_t)entries[i].length);
		}
		if (archive_write_header(writer, entry) != ARCHIVE_OK) {
			status = -1;
		} else if (entries[i].length > 0 &&
		           archive_write_data(writer, entries[i].data, entries[i].length) != (la_ssize_t)entries[i].length) {
			status = -1;
		}
		archive_entry_free(entry);
	}
	if (archive_write_close(writer) != ARCHIVE_OK) {
		status = -1;
	}
	archive_write_free(writer);
	return status;
}
