#ifndef RAIJU_JSON_H
#define RAIJU_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAIJU_JSON_MAX_DEPTH 32

/**
 * A position in untrusted JSON input. Every reader checks the end pointer
 * before each byte and fails instead of reading past it.
 */
typedef struct {
	const unsigned char *pos;
	const unsigned char *end;
} raiju_json_cursor;

/**
 * A growable output buffer. Once an allocation fails, failed stays set and
 * further writes are ignored.
 */
typedef struct {
	char *data;
	size_t length;
	size_t capacity;
	bool failed;
} raiju_json_writer;

/**
 * Skips JSON insignificant whitespace.
 */
void raiju_json_skip_whitespace(raiju_json_cursor *cur);

/**
 * Skips whitespace, then consumes byte c if it is next.
 */
bool raiju_json_consume(raiju_json_cursor *cur, unsigned char c);

/**
 * Skips whitespace and returns whether the input is exhausted.
 */
bool raiju_json_at_end(raiju_json_cursor *cur);

/**
 * Skips whitespace and reads a JSON string into out as NUL-terminated bytes.
 * Fails on malformed input, on a decoded NUL, and when the decoded string
 * does not fit in capacity - 1 bytes.
 */
bool raiju_json_read_string(raiju_json_cursor *cur, char *out, size_t capacity);

/**
 * Skips whitespace and reads a non-negative integer without fraction or
 * exponent that fits in 64 bits.
 */
bool raiju_json_read_uint64(raiju_json_cursor *cur, uint64_t *out);

/**
 * Skips whitespace and reads true or false.
 */
bool raiju_json_read_bool(raiju_json_cursor *cur, bool *out);

/**
 * Skips whitespace and one complete JSON value of any type, validating it,
 * with nesting limited to RAIJU_JSON_MAX_DEPTH.
 */
bool raiju_json_skip_value(raiju_json_cursor *cur, int depth);

/**
 * Appends raw text.
 */
void raiju_json_write_raw(raiju_json_writer *writer, const char *text);

/**
 * Appends text as a quoted JSON string, escaping quotes, backslashes and
 * control characters.
 */
void raiju_json_write_string(raiju_json_writer *writer, const char *text);

/**
 * Appends an unsigned integer.
 */
void raiju_json_write_uint64(raiju_json_writer *writer, uint64_t value);

/**
 * Appends true or false.
 */
void raiju_json_write_bool(raiju_json_writer *writer, bool value);

#endif
