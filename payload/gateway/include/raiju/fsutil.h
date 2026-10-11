#ifndef RAIJU_FSUTIL_H
#define RAIJU_FSUTIL_H

#include <stdbool.h>
#include <stddef.h>

#define RAIJU_PATH_MAX 4096

/**
 * Writes length bytes to path atomically: the bytes go to path.tmp, which is
 * fsynced, renamed over path, then the parent directory is fsynced, so a
 * crash leaves either the old or the new file, never a mix.
 */
bool raiju_atomic_write(const char *path, const void *bytes, size_t length);

/**
 * Reads a whole file into a newly allocated NUL-terminated buffer the caller
 * frees. Returns NULL when the file is missing or larger than max_bytes.
 */
char *raiju_read_file(const char *path, size_t max_bytes, size_t *out_length);

/**
 * Creates path, and its parents as needed, like mkdir -p. Succeeds when the
 * path already exists as a directory.
 */
bool raiju_mkdir_p(const char *path);

/**
 * Fills buffer with lowercase hexadecimal of 2 * (length) characters plus a
 * NUL. capacity must be at least 2 * length + 1.
 */
void raiju_hex_encode(const unsigned char *bytes, size_t length, char *buffer, size_t capacity);

#endif
