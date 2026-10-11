#ifndef RAIJU_STRBUF_H
#define RAIJU_STRBUF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * A growable NUL-terminated byte buffer. Once an allocation fails, failed
 * stays set and further writes are ignored, so callers check failed once at
 * the end.
 */
typedef struct {
	char *data;
	size_t length;
	size_t capacity;
	bool failed;
} raiju_strbuf;

/**
 * Initialises an empty buffer. Release it with raiju_strbuf_free.
 */
void raiju_strbuf_init(raiju_strbuf *buffer);

/**
 * Appends text.
 */
void raiju_strbuf_append(raiju_strbuf *buffer, const char *text);

/**
 * Appends length raw bytes, which may contain NUL.
 */
void raiju_strbuf_append_bytes(raiju_strbuf *buffer, const void *bytes, size_t length);

/**
 * Appends a formatted string.
 */
void raiju_strbuf_appendf(raiju_strbuf *buffer, const char *format, ...);

/**
 * Appends a single character.
 */
void raiju_strbuf_append_char(raiju_strbuf *buffer, char value);

/**
 * Appends an unsigned integer in decimal.
 */
void raiju_strbuf_append_uint(raiju_strbuf *buffer, unsigned long long value);

/**
 * Appends text as a JSON string literal, surrounding quotes included, with
 * quotes, backslashes and control characters escaped.
 */
void raiju_strbuf_append_json_string(raiju_strbuf *buffer, const char *text);

/**
 * Returns the buffer as a NUL-terminated string, allocating a one-byte empty
 * string when nothing was written and memory is short.
 */
const char *raiju_strbuf_cstr(raiju_strbuf *buffer);

/**
 * Releases the buffer's storage and resets it.
 */
void raiju_strbuf_free(raiju_strbuf *buffer);

#endif
