#include "raiju/inspect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Returns the final path component, the part after the last '/'.
 */
static const char *base_name(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/**
 * Determines the size of an open binary stream and rewinds it.
 */
static int stream_size(FILE *stream, uint64_t *size) {
	if (fseek(stream, 0, SEEK_END) != 0) {
		return -1;
	}
	long end = ftell(stream);
	if (end < 0 || fseek(stream, 0, SEEK_SET) != 0) {
		return -1;
	}
	*size = (uint64_t)end;
	return 0;
}

int raiju_inspect_file(const char *path, raiju_inspection *out) {
	if (path == NULL || out == NULL) {
		return -1;
	}
	memset(out, 0, sizeof *out);
	FILE *stream = fopen(path, "rb");
	if (stream == NULL) {
		return -1;
	}
	uint8_t *head = malloc(RAIJU_PROBE_SIZE);
	if (head == NULL || stream_size(stream, &out->file_size) != 0) {
		free(head);
		fclose(stream);
		return -1;
	}
	size_t head_len = fread(head, 1, RAIJU_PROBE_SIZE, stream);
	int read_failed = ferror(stream);
	fclose(stream);
	if (read_failed) {
		free(head);
		return -1;
	}

	out->classification = raiju_classify(head, head_len, base_name(path));
	if (!out->classification.header_mismatch) {
		if (out->classification.kind == RAIJU_KIND_EXFAT) {
			out->image = raiju_check_exfat(head, head_len, out->file_size);
			out->image_checked = true;
		} else if (out->classification.kind == RAIJU_KIND_UFS2) {
			out->image = raiju_check_ufs2(head, head_len, out->file_size);
			out->image_checked = true;
		}
	}
	free(head);
	return 0;
}

bool raiju_inspection_accepted(const raiju_inspection *inspection) {
	if (inspection == NULL || inspection->classification.header_mismatch) {
		return false;
	}
	return !inspection->image_checked || inspection->image.status == RAIJU_IMAGE_OK;
}
