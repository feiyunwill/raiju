#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/extract.h"

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "raiju/classify.h"

#define READ_BLOCK_SIZE (1024u * 1024u)
#define IMAGE_SEARCH_MAX_DEPTH 32
#define PARAM_JSON_RELATIVE "sce_sys/param.json"
#define DUMP_MARKER "sce_sys"

typedef struct {
	const raiju_extract_request *request;
	raiju_extract_result *result;
	raiju_extract_control requested;
} extract_state;

typedef struct {
	unsigned count;
	char *first_path;
} image_search;

/**
 * Joins a directory and a relative name into a newly allocated path, or
 * returns NULL when out of memory.
 */
static char *join_path(const char *dir, const char *name) {
	size_t dir_len = strlen(dir);
	bool needs_slash = dir_len > 0 && dir[dir_len - 1] != '/';
	size_t length = dir_len + (needs_slash ? 1 : 0) + strlen(name) + 1;
	char *path = malloc(length);
	if (path != NULL) {
		snprintf(path, length, "%s%s%s", dir, needs_slash ? "/" : "", name);
	}
	return path;
}

/**
 * Returns a newly allocated copy of text, or NULL.
 */
static char *copy_string(const char *text) {
	size_t length = strlen(text) + 1;
	char *copy = malloc(length);
	if (copy != NULL) {
		memcpy(copy, text, length);
	}
	return copy;
}

/**
 * Normalizes an archive entry path into a relative path made of plain
 * components: empty and '.' components are dropped, while an absolute path,
 * a '..' component or a backslash makes the path unsafe. Writes NULL to out
 * when the path names the staging root itself.
 */
static raiju_extract_status normalize_entry_path(const char *raw, char **out) {
	*out = NULL;
	if (raw == NULL || raw[0] == '\0' || raw[0] == '/' || strchr(raw, '\\') != NULL) {
		return RAIJU_EXTRACT_UNSAFE_PATH;
	}
	char *normalized = malloc(strlen(raw) + 1);
	if (normalized == NULL) {
		return RAIJU_EXTRACT_IO_ERROR;
	}
	size_t length = 0;
	const char *cursor = raw;
	while (*cursor != '\0') {
		const char *end = strchr(cursor, '/');
		size_t part = end ? (size_t)(end - cursor) : strlen(cursor);
		bool skip = part == 0 || (part == 1 && cursor[0] == '.');
		if (part == 2 && cursor[0] == '.' && cursor[1] == '.') {
			free(normalized);
			return RAIJU_EXTRACT_UNSAFE_PATH;
		}
		if (!skip) {
			if (length > 0) {
				normalized[length++] = '/';
			}
			memcpy(normalized + length, cursor, part);
			length += part;
		}
		cursor += part;
		if (*cursor == '/') {
			cursor++;
		}
	}
	normalized[length] = '\0';
	if (length == 0) {
		free(normalized);
		return RAIJU_EXTRACT_OK;
	}
	*out = normalized;
	return RAIJU_EXTRACT_OK;
}

/**
 * Checks the components of a relative path below the staging root that
 * already exist on disk: every parent must be a real directory and the final
 * component must not be a symlink, so nothing can be written outside
 * staging through a link.
 */
static raiju_extract_status check_existing_components(const char *staging, const char *relative) {
	char *path = join_path(staging, relative);
	if (path == NULL) {
		return RAIJU_EXTRACT_IO_ERROR;
	}
	size_t prefix = strlen(path) - strlen(relative);
	raiju_extract_status status = RAIJU_EXTRACT_OK;
	for (char *cursor = path + prefix;; cursor++) {
		bool last = *cursor == '\0';
		if (*cursor != '/' && !last) {
			continue;
		}
		char saved = *cursor;
		*cursor = '\0';
		struct stat info;
		if (lstat(path, &info) != 0) {
			if (errno != ENOENT) {
				status = RAIJU_EXTRACT_IO_ERROR;
			}
			*cursor = saved;
			break;
		}
		if (S_ISLNK(info.st_mode) || (!last && !S_ISDIR(info.st_mode))) {
			status = RAIJU_EXTRACT_UNSAFE_PATH;
			*cursor = saved;
			break;
		}
		*cursor = saved;
		if (last) {
			break;
		}
	}
	free(path);
	return status;
}

/**
 * Decides whether an entry's type may be extracted: only regular files and
 * directories are, links fail with LINK_ENTRY and anything else with
 * UNSUPPORTED_ENTRY.
 */
static raiju_extract_status check_entry_type(struct archive_entry *entry) {
	if (archive_entry_hardlink(entry) != NULL || archive_entry_symlink(entry) != NULL) {
		return RAIJU_EXTRACT_LINK_ENTRY;
	}
	switch (archive_entry_filetype(entry)) {
	case AE_IFREG:
	case AE_IFDIR:
		return RAIJU_EXTRACT_OK;
	case AE_IFLNK:
		return RAIJU_EXTRACT_LINK_ENTRY;
	default:
		return RAIJU_EXTRACT_UNSUPPORTED_ENTRY;
	}
}

/**
 * Reports progress to the caller and latches the first pause or cancel
 * request, which is then returned until the extraction stops.
 */
static raiju_extract_control notify_progress(extract_state *state) {
	const raiju_extract_request *request = state->request;
	if (request->progress != NULL) {
		raiju_extract_control answer =
		    request->progress(state->result->bytes_written, request->total_bytes, request->context);
		if (state->requested == RAIJU_EXTRACT_CONTINUE && answer != RAIJU_EXTRACT_CONTINUE) {
			state->requested = answer == RAIJU_EXTRACT_PAUSE ? RAIJU_EXTRACT_PAUSE : RAIJU_EXTRACT_CANCEL;
		}
	}
	return state->requested;
}

/**
 * Copies the data blocks of the current entry from the reader to the disk
 * writer, counting bytes and reporting progress after each block.
 */
static raiju_extract_status copy_entry_data(struct archive *reader, struct archive *writer, extract_state *state) {
	for (;;) {
		const void *block;
		size_t size;
		la_int64_t offset;
		int read_status = archive_read_data_block(reader, &block, &size, &offset);
		if (read_status == ARCHIVE_EOF) {
			return RAIJU_EXTRACT_OK;
		}
		if (read_status < ARCHIVE_WARN) {
			return RAIJU_EXTRACT_ARCHIVE_ERROR;
		}
		if (archive_write_data_block(writer, block, size, offset) < ARCHIVE_WARN) {
			return RAIJU_EXTRACT_IO_ERROR;
		}
		state->result->bytes_written += size;
		notify_progress(state);
	}
}

/**
 * Vets one entry and writes it below the staging root. Unsafe entries are
 * recorded in failed_entry and stop the extraction.
 */
static raiju_extract_status extract_entry(struct archive *reader, struct archive *writer, struct archive_entry *entry,
                                          const char *staging, extract_state *state) {
	const char *raw = archive_entry_pathname(entry);
	char *relative = NULL;
	raiju_extract_status status = normalize_entry_path(raw, &relative);
	if (status == RAIJU_EXTRACT_OK) {
		status = check_entry_type(entry);
	}
	if (status == RAIJU_EXTRACT_OK && relative == NULL && archive_entry_filetype(entry) != AE_IFDIR) {
		status = RAIJU_EXTRACT_UNSAFE_PATH;
	}
	if (status == RAIJU_EXTRACT_OK && relative != NULL) {
		status = check_existing_components(staging, relative);
	}
	if (status != RAIJU_EXTRACT_OK) {
		if (status != RAIJU_EXTRACT_IO_ERROR && state->result->failed_entry == NULL) {
			state->result->failed_entry = copy_string(raw != NULL ? raw : "");
		}
		free(relative);
		return status;
	}
	if (relative == NULL) {
		return RAIJU_EXTRACT_OK;
	}
	char *target = join_path(staging, relative);
	free(relative);
	if (target == NULL) {
		return RAIJU_EXTRACT_IO_ERROR;
	}
	bool is_directory = archive_entry_filetype(entry) == AE_IFDIR;
	archive_entry_copy_pathname(entry, target);
	archive_entry_set_perm(entry, is_directory ? 0755 : 0644);
	free(target);
	if (archive_write_header(writer, entry) < ARCHIVE_WARN) {
		return RAIJU_EXTRACT_IO_ERROR;
	}
	if (!is_directory) {
		status = copy_entry_data(reader, writer, state);
	}
	if (archive_write_finish_entry(writer) < ARCHIVE_WARN && status == RAIJU_EXTRACT_OK) {
		status = RAIJU_EXTRACT_IO_ERROR;
	}
	return status;
}

/**
 * Opens an archive for reading with the formats and filters Raiju accepts.
 * Returns NULL when the archive cannot be opened.
 */
static struct archive *open_reader(const char *archive_path) {
	struct archive *reader = archive_read_new();
	if (reader == NULL) {
		return NULL;
	}
	archive_read_support_format_7zip(reader);
	archive_read_support_format_zip(reader);
	archive_read_support_format_rar(reader);
	archive_read_support_format_rar5(reader);
	archive_read_support_format_tar(reader);
	archive_read_support_filter_zstd(reader);
	archive_read_support_filter_xz(reader);
	archive_read_support_filter_lzma(reader);
	archive_read_support_filter_gzip(reader);
	archive_read_support_filter_bzip2(reader);
	if (archive_read_open_filename(reader, archive_path, READ_BLOCK_SIZE) != ARCHIVE_OK) {
		archive_read_free(reader);
		return NULL;
	}
	return reader;
}

/**
 * Returns whether a directory has no entries besides '.' and '..'.
 */
static int directory_is_empty(const char *path, bool *empty) {
	DIR *dir = opendir(path);
	if (dir == NULL) {
		return -1;
	}
	*empty = true;
	struct dirent *item;
	while ((item = readdir(dir)) != NULL) {
		if (strcmp(item->d_name, ".") != 0 && strcmp(item->d_name, "..") != 0) {
			*empty = false;
			break;
		}
	}
	closedir(dir);
	return 0;
}

/**
 * Creates the staging directory or confirms an existing one is a real, empty
 * directory, then writes its canonical absolute path to out.
 */
static raiju_extract_status prepare_staging(const char *staging_dir, char **out) {
	*out = NULL;
	if (mkdir(staging_dir, 0755) != 0) {
		if (errno != EEXIST) {
			return RAIJU_EXTRACT_IO_ERROR;
		}
		struct stat info;
		if (lstat(staging_dir, &info) != 0 || !S_ISDIR(info.st_mode)) {
			return RAIJU_EXTRACT_INVALID_ARGUMENT;
		}
		bool empty = false;
		if (directory_is_empty(staging_dir, &empty) != 0) {
			return RAIJU_EXTRACT_IO_ERROR;
		}
		if (!empty) {
			return RAIJU_EXTRACT_STAGING_NOT_EMPTY;
		}
	}
	char *canonical = realpath(staging_dir, NULL);
	if (canonical == NULL) {
		return RAIJU_EXTRACT_IO_ERROR;
	}
	*out = canonical;
	return RAIJU_EXTRACT_OK;
}

/**
 * Reads entries until the archive ends, an entry fails, or the caller asks
 * to pause or cancel at an entry boundary.
 */
static raiju_extract_status extract_entries(struct archive *reader, struct archive *writer, const char *staging,
                                            extract_state *state) {
	for (;;) {
		raiju_extract_control control = notify_progress(state);
		if (control == RAIJU_EXTRACT_PAUSE) {
			return RAIJU_EXTRACT_PAUSED;
		}
		if (control == RAIJU_EXTRACT_CANCEL) {
			return RAIJU_EXTRACT_CANCELLED;
		}
		struct archive_entry *entry;
		int header_status = archive_read_next_header(reader, &entry);
		if (header_status == ARCHIVE_EOF) {
			return RAIJU_EXTRACT_OK;
		}
		if (header_status < ARCHIVE_WARN) {
			return RAIJU_EXTRACT_ARCHIVE_ERROR;
		}
		raiju_extract_status status = extract_entry(reader, writer, entry, staging, state);
		if (status != RAIJU_EXTRACT_OK) {
			return status;
		}
		state->result->entries++;
	}
}

raiju_extract_status raiju_extract_archive(const raiju_extract_request *request, raiju_extract_result *out) {
	if (out == NULL) {
		return RAIJU_EXTRACT_INVALID_ARGUMENT;
	}
	memset(out, 0, sizeof *out);
	out->param_status = RAIJU_PARAM_IO_ERROR;
	if (request == NULL || request->archive_path == NULL || request->staging_dir == NULL) {
		out->status = RAIJU_EXTRACT_INVALID_ARGUMENT;
		return out->status;
	}
	char *staging = NULL;
	raiju_extract_status status = prepare_staging(request->staging_dir, &staging);
	if (status != RAIJU_EXTRACT_OK) {
		out->status = status;
		return status;
	}
	struct archive *reader = open_reader(request->archive_path);
	struct archive *writer = archive_write_disk_new();
	if (reader == NULL || writer == NULL) {
		status = reader == NULL ? RAIJU_EXTRACT_ARCHIVE_ERROR : RAIJU_EXTRACT_IO_ERROR;
	} else {
		archive_write_disk_set_options(writer, ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT);
		extract_state state = {request, out, RAIJU_EXTRACT_CONTINUE};
		status = extract_entries(reader, writer, staging, &state);
		if (archive_write_close(writer) != ARCHIVE_OK && status == RAIJU_EXTRACT_OK) {
			status = RAIJU_EXTRACT_IO_ERROR;
		}
	}
	if (writer != NULL) {
		archive_write_free(writer);
	}
	if (reader != NULL) {
		archive_read_free(reader);
	}
	if (status == RAIJU_EXTRACT_OK) {
		status = raiju_normalize_layout(staging, out);
	}
	free(staging);
	out->status = status;
	return status;
}

/**
 * Returns whether a directory entry is operating-system clutter that never
 * counts as part of a dump: macOS resource forks and Finder metadata.
 */
static bool is_clutter(const char *name) {
	return strcmp(name, "__MACOSX") == 0 || strcmp(name, ".DS_Store") == 0;
}

/**
 * Returns whether path names a real directory, not following symlinks.
 */
static bool is_real_directory(const char *path) {
	struct stat info;
	return lstat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/**
 * Descends through wrapper folders, directories holding nothing but one
 * subdirectory, until one holding sce_sys/ is found. Writes that folder and
 * the number of wrappers passed, or leaves *root NULL when there is no dump.
 */
static int find_dump_root(const char *staging, char **root, unsigned *depth) {
	*root = NULL;
	*depth = 0;
	char *current = copy_string(staging);
	while (current != NULL) {
		DIR *dir = opendir(current);
		if (dir == NULL) {
			free(current);
			return -1;
		}
		unsigned count = 0;
		bool has_marker = false;
		char *sole = NULL;
		struct dirent *item;
		while ((item = readdir(dir)) != NULL) {
			const char *name = item->d_name;
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || is_clutter(name)) {
				continue;
			}
			count++;
			char *child = join_path(current, name);
			if (child != NULL && is_real_directory(child)) {
				if (strcmp(name, DUMP_MARKER) == 0) {
					has_marker = true;
				}
				if (count == 1) {
					sole = child;
					child = NULL;
				}
			}
			free(child);
		}
		closedir(dir);
		if (has_marker) {
			free(sole);
			*root = current;
			return 0;
		}
		free(current);
		current = NULL;
		if (count == 1 && sole != NULL && *depth < RAIJU_EXTRACT_MAX_WRAPPER_DEPTH) {
			current = sole;
			(*depth)++;
		} else {
			free(sole);
		}
	}
	*depth = 0;
	return 0;
}

/**
 * Walks a directory tree counting files with an image extension and keeps
 * the path of the first one found.
 */
static int find_images(const char *path, unsigned depth, image_search *search) {
	if (depth > IMAGE_SEARCH_MAX_DEPTH) {
		return 0;
	}
	DIR *dir = opendir(path);
	if (dir == NULL) {
		return -1;
	}
	int status = 0;
	struct dirent *item;
	while (status == 0 && (item = readdir(dir)) != NULL) {
		const char *name = item->d_name;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || is_clutter(name)) {
			continue;
		}
		char *child = join_path(path, name);
		struct stat info;
		if (child == NULL || lstat(child, &info) != 0) {
			status = -1;
		} else if (S_ISDIR(info.st_mode)) {
			status = find_images(child, depth + 1, search);
		} else if (S_ISREG(info.st_mode) && raiju_image_kind_from_name(name) != RAIJU_KIND_UNKNOWN) {
			search->count++;
			if (search->first_path == NULL) {
				search->first_path = child;
				child = NULL;
			}
		}
		free(child);
	}
	closedir(dir);
	return status;
}

/**
 * Loads <root>/sce_sys/param.json into the result and resolves its title ID
 * with the shared param.json parser. A missing or oversized file is reported
 * through param_status and does not fail the extraction.
 */
static void load_param_json(const char *root, raiju_extract_result *out) {
	out->param_status = RAIJU_PARAM_IO_ERROR;
	char *path = join_path(root, PARAM_JSON_RELATIVE);
	if (path == NULL) {
		return;
	}
	struct stat info;
	if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
		free(path);
		return;
	}
	if ((uint64_t)info.st_size > RAIJU_PARAM_JSON_MAX_BYTES) {
		out->param_status = RAIJU_PARAM_TOO_LARGE;
		free(path);
		return;
	}
	FILE *stream = fopen(path, "rb");
	free(path);
	if (stream == NULL) {
		return;
	}
	size_t capacity = (size_t)info.st_size;
	char *buffer = malloc(capacity + 1);
	size_t length = buffer != NULL ? fread(buffer, 1, capacity, stream) : 0;
	bool failed = buffer == NULL || ferror(stream) || length != capacity;
	fclose(stream);
	if (failed) {
		free(buffer);
		return;
	}
	buffer[length] = '\0';
	out->param_json = buffer;
	out->param_json_len = length;
	out->param_status = raiju_title_id_from_param_json(buffer, length, out->title_id);
	if (out->param_status != RAIJU_PARAM_OK) {
		out->title_id[0] = '\0';
	}
}

raiju_extract_status raiju_normalize_layout(const char *staging_dir, raiju_extract_result *out) {
	if (staging_dir == NULL || out == NULL) {
		return RAIJU_EXTRACT_INVALID_ARGUMENT;
	}
	char *root = NULL;
	unsigned depth = 0;
	if (find_dump_root(staging_dir, &root, &depth) != 0) {
		return RAIJU_EXTRACT_IO_ERROR;
	}
	if (root != NULL) {
		out->layout = RAIJU_LAYOUT_FOLDER_DUMP;
		out->payload_path = root;
		out->wrapper_depth = depth;
		load_param_json(root, out);
		return RAIJU_EXTRACT_OK;
	}
	image_search search = {0, NULL};
	if (find_images(staging_dir, 0, &search) != 0) {
		free(search.first_path);
		return RAIJU_EXTRACT_IO_ERROR;
	}
	if (search.count == 0) {
		return RAIJU_EXTRACT_NO_PAYLOAD;
	}
	if (search.count > 1) {
		free(search.first_path);
		return RAIJU_EXTRACT_MULTIPLE_IMAGES;
	}
	out->layout = RAIJU_LAYOUT_SINGLE_IMAGE;
	out->payload_path = search.first_path;
	if (raiju_inspect_file(search.first_path, &out->image) != 0) {
		return RAIJU_EXTRACT_IO_ERROR;
	}
	return raiju_inspection_accepted(&out->image) ? RAIJU_EXTRACT_OK : RAIJU_EXTRACT_BAD_IMAGE;
}

int raiju_archive_total_bytes(const char *archive_path, uint64_t *total) {
	if (archive_path == NULL || total == NULL) {
		return -1;
	}
	*total = 0;
	struct archive *reader = open_reader(archive_path);
	if (reader == NULL) {
		return -1;
	}
	int status = 0;
	struct archive_entry *entry;
	for (;;) {
		int header_status = archive_read_next_header(reader, &entry);
		if (header_status == ARCHIVE_EOF) {
			break;
		}
		if (header_status < ARCHIVE_WARN) {
			status = -1;
			break;
		}
		if (archive_entry_filetype(entry) != AE_IFREG) {
			continue;
		}
		if (!archive_entry_size_is_set(entry) || archive_entry_size(entry) < 0) {
			status = -1;
			break;
		}
		*total += (uint64_t)archive_entry_size(entry);
	}
	archive_read_free(reader);
	if (status != 0) {
		*total = 0;
	}
	return status;
}

/**
 * Recursively removes the contents of a directory without following
 * symlinks. Returns 0 when everything was removed.
 */
static int remove_children(const char *path) {
	DIR *dir = opendir(path);
	if (dir == NULL) {
		return -1;
	}
	int status = 0;
	struct dirent *item;
	while ((item = readdir(dir)) != NULL) {
		const char *name = item->d_name;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
			continue;
		}
		char *child = join_path(path, name);
		struct stat info;
		if (child == NULL || lstat(child, &info) != 0) {
			status = -1;
		} else if (S_ISDIR(info.st_mode)) {
			if (remove_children(child) != 0 || rmdir(child) != 0) {
				status = -1;
			}
		} else if (unlink(child) != 0) {
			status = -1;
		}
		free(child);
	}
	closedir(dir);
	return status;
}

int raiju_staging_clear(const char *staging_dir) {
	if (staging_dir == NULL || !is_real_directory(staging_dir)) {
		return -1;
	}
	return remove_children(staging_dir);
}

void raiju_extract_result_free(raiju_extract_result *result) {
	if (result == NULL) {
		return;
	}
	free(result->payload_path);
	free(result->failed_entry);
	free(result->param_json);
	memset(result, 0, sizeof *result);
}

const char *raiju_extract_status_name(raiju_extract_status status) {
	switch (status) {
	case RAIJU_EXTRACT_OK:
		return "ok";
	case RAIJU_EXTRACT_PAUSED:
		return "paused";
	case RAIJU_EXTRACT_CANCELLED:
		return "cancelled";
	case RAIJU_EXTRACT_UNSAFE_PATH:
		return "unsafe_path";
	case RAIJU_EXTRACT_LINK_ENTRY:
		return "link_entry";
	case RAIJU_EXTRACT_UNSUPPORTED_ENTRY:
		return "unsupported_entry";
	case RAIJU_EXTRACT_ARCHIVE_ERROR:
		return "archive_error";
	case RAIJU_EXTRACT_IO_ERROR:
		return "io_error";
	case RAIJU_EXTRACT_STAGING_NOT_EMPTY:
		return "staging_not_empty";
	case RAIJU_EXTRACT_NO_PAYLOAD:
		return "no_payload";
	case RAIJU_EXTRACT_MULTIPLE_IMAGES:
		return "multiple_images";
	case RAIJU_EXTRACT_BAD_IMAGE:
		return "bad_image";
	case RAIJU_EXTRACT_INVALID_ARGUMENT:
		return "invalid_argument";
	}
	return "unknown";
}

const char *raiju_layout_name(raiju_layout layout) {
	switch (layout) {
	case RAIJU_LAYOUT_NONE:
		return "none";
	case RAIJU_LAYOUT_FOLDER_DUMP:
		return "folder_dump";
	case RAIJU_LAYOUT_SINGLE_IMAGE:
		return "single_image";
	}
	return "unknown";
}
