#ifndef RAIJU_INSPECT_H
#define RAIJU_INSPECT_H

#include <stdbool.h>
#include <stdint.h>

#include "raiju/classify.h"
#include "raiju/image_check.h"

/**
 * Everything the processor learns from a downloaded file before extracting
 * or installing it. image is only meaningful when image_checked is set, which
 * happens for exFAT and UFS2 images whose header matched.
 */
typedef struct {
	raiju_classification classification;
	uint64_t file_size;
	bool image_checked;
	raiju_image_report image;
} raiju_inspection;

/**
 * Reads the head of the file at path, classifies it by magic bytes and name,
 * and runs the structural check for exFAT and UFS2 images. Returns 0 on
 * success and -1 when the file cannot be opened or read.
 */
int raiju_inspect_file(const char *path, raiju_inspection *out);

/**
 * Returns whether an inspection allows the file to move on in the pipeline:
 * false for an image whose header or structure is wrong.
 */
bool raiju_inspection_accepted(const raiju_inspection *inspection);

#endif
