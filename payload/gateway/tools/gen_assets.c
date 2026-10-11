#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define RAIJU_GEN_PATH_MAX 4096

typedef struct {
	char path[RAIJU_GEN_PATH_MAX];
	unsigned char *bytes;
	size_t length;
} gen_asset;

typedef struct {
	gen_asset *items;
	size_t count;
	size_t capacity;
} gen_list;

static int compare_assets(const void *a, const void *b) {
	const gen_asset *left = a;
	const gen_asset *right = b;
	return strcmp(left->path, right->path);
}

static bool read_asset(const char *full, const char *relative, gen_list *list) {
	FILE *file = fopen(full, "rb");
	if (file == NULL) {
		return false;
	}
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return false;
	}
	long size = ftell(file);
	if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return false;
	}
	unsigned char *bytes = malloc((size_t)size == 0 ? 1 : (size_t)size);
	if (bytes == NULL) {
		fclose(file);
		return false;
	}
	if (size > 0 && fread(bytes, 1, (size_t)size, file) != (size_t)size) {
		free(bytes);
		fclose(file);
		return false;
	}
	fclose(file);
	if (list->count == list->capacity) {
		size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
		gen_asset *grown = realloc(list->items, capacity * sizeof *grown);
		if (grown == NULL) {
			free(bytes);
			return false;
		}
		list->items = grown;
		list->capacity = capacity;
	}
	gen_asset *asset = &list->items[list->count++];
	snprintf(asset->path, sizeof asset->path, "%s", relative);
	asset->bytes = bytes;
	asset->length = (size_t)size;
	return true;
}

static bool walk(const char *full, const char *relative, gen_list *list) {
	DIR *directory = opendir(full);
	if (directory == NULL) {
		return false;
	}
	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		char child_full[RAIJU_GEN_PATH_MAX];
		char child_relative[RAIJU_GEN_PATH_MAX];
		if (snprintf(child_full, sizeof child_full, "%s/%s", full, entry->d_name) >= (int)sizeof child_full) {
			closedir(directory);
			return false;
		}
		if (relative[0] == '\0') {
			snprintf(child_relative, sizeof child_relative, "%s", entry->d_name);
		} else {
			snprintf(child_relative, sizeof child_relative, "%s/%s", relative, entry->d_name);
		}
		struct stat info;
		if (stat(child_full, &info) != 0) {
			closedir(directory);
			return false;
		}
		if (S_ISDIR(info.st_mode)) {
			if (!walk(child_full, child_relative, list)) {
				closedir(directory);
				return false;
			}
		} else if (S_ISREG(info.st_mode)) {
			if (!read_asset(child_full, child_relative, list)) {
				closedir(directory);
				return false;
			}
		}
	}
	closedir(directory);
	return true;
}

static void emit_bytes(FILE *out, const gen_asset *asset, size_t index) {
	fprintf(out, "static const unsigned char raiju_asset_bytes_%zu[] = {", index);
	for (size_t i = 0; i < asset->length; i++) {
		if (i % 12 == 0) {
			fprintf(out, "\n    ");
		}
		fprintf(out, "0x%02x,", asset->bytes[i]);
	}
	if (asset->length == 0) {
		fprintf(out, "0x00");
	}
	fprintf(out, "\n};\n");
}

static bool emit_table(const char *output, gen_list *list) {
	FILE *out = fopen(output, "wb");
	if (out == NULL) {
		return false;
	}
	fprintf(out, "#include \"raiju/assets.h\"\n\n");
	for (size_t i = 0; i < list->count; i++) {
		emit_bytes(out, &list->items[i], i);
	}
	fprintf(out, "\nconst raiju_asset raiju_embedded_assets[] = {\n");
	for (size_t i = 0; i < list->count; i++) {
		fprintf(out, "    {\"%s\", raiju_asset_bytes_%zu, %zu},\n", list->items[i].path, i,
		        list->items[i].length);
	}
	if (list->count == 0) {
		fprintf(out, "    {NULL, NULL, 0},\n");
	}
	fprintf(out, "};\n\nconst size_t raiju_embedded_asset_count = %zu;\n", list->count);
	bool ok = fflush(out) == 0 && ferror(out) == 0;
	fclose(out);
	return ok;
}

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: %s <embed-dir> <output.c>\n", argv[0]);
		return 2;
	}
	gen_list list;
	memset(&list, 0, sizeof list);
	struct stat info;
	if (stat(argv[1], &info) == 0 && S_ISDIR(info.st_mode)) {
		if (!walk(argv[1], "", &list)) {
			fprintf(stderr, "gen_assets: failed to read %s\n", argv[1]);
			return 1;
		}
	} else if (errno != ENOENT && errno != 0 && argc > 0) {
		/* Absent embed directory: emit an empty table. */
	}
	if (list.count > 1) {
		qsort(list.items, list.count, sizeof *list.items, compare_assets);
	}
	bool ok = emit_table(argv[2], &list);
	for (size_t i = 0; i < list.count; i++) {
		free(list.items[i].bytes);
	}
	free(list.items);
	if (!ok) {
		fprintf(stderr, "gen_assets: failed to write %s\n", argv[2]);
		return 1;
	}
	return 0;
}
