#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Returns whether c is an ASCII decimal digit, independent of locale.
 */
static bool is_digit(int c) {
	return c >= '0' && c <= '9';
}

void raiju_json_skip_whitespace(raiju_json_cursor *cur) {
	while (cur->pos < cur->end && (*cur->pos == ' ' || *cur->pos == '\t' || *cur->pos == '\n' || *cur->pos == '\r')) {
		cur->pos++;
	}
}

bool raiju_json_consume(raiju_json_cursor *cur, unsigned char c) {
	raiju_json_skip_whitespace(cur);
	if (cur->pos < cur->end && *cur->pos == c) {
		cur->pos++;
		return true;
	}
	return false;
}

bool raiju_json_at_end(raiju_json_cursor *cur) {
	raiju_json_skip_whitespace(cur);
	return cur->pos == cur->end;
}

/**
 * Appends one byte to a bounded output, keeping room for the terminator.
 */
static bool put_byte(char *out, size_t capacity, size_t *length, unsigned char c) {
	if (out == NULL) {
		return true;
	}
	if (c == 0 || *length + 1 >= capacity) {
		return false;
	}
	out[(*length)++] = (char)c;
	return true;
}

/**
 * Parses the four hexadecimal digits of a \u escape.
 */
static bool parse_hex4(raiju_json_cursor *cur, uint32_t *out) {
	if (cur->end - cur->pos < 4) {
		return false;
	}
	uint32_t value = 0;
	for (int i = 0; i < 4; i++) {
		unsigned char c = *cur->pos++;
		uint32_t digit;
		if (is_digit(c)) {
			digit = (uint32_t)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			digit = (uint32_t)(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			digit = (uint32_t)(c - 'A' + 10);
		} else {
			return false;
		}
		value = (value << 4) | digit;
	}
	*out = value;
	return true;
}

/**
 * Decodes a \u escape, including surrogate pairs, and appends it as UTF-8.
 * Lone surrogates are invalid.
 */
static bool parse_unicode_escape(raiju_json_cursor *cur, char *out, size_t capacity, size_t *length) {
	uint32_t cp;
	if (!parse_hex4(cur, &cp) || (cp >= 0xDC00 && cp <= 0xDFFF)) {
		return false;
	}
	if (cp >= 0xD800 && cp <= 0xDBFF) {
		uint32_t low;
		if (cur->end - cur->pos < 2 || cur->pos[0] != '\\' || cur->pos[1] != 'u') {
			return false;
		}
		cur->pos += 2;
		if (!parse_hex4(cur, &low) || low < 0xDC00 || low > 0xDFFF) {
			return false;
		}
		cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
	}
	unsigned char bytes[4];
	size_t count;
	if (cp < 0x80) {
		bytes[0] = (unsigned char)cp;
		count = 1;
	} else if (cp < 0x800) {
		bytes[0] = (unsigned char)(0xC0 | (cp >> 6));
		bytes[1] = (unsigned char)(0x80 | (cp & 0x3F));
		count = 2;
	} else if (cp < 0x10000) {
		bytes[0] = (unsigned char)(0xE0 | (cp >> 12));
		bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		bytes[2] = (unsigned char)(0x80 | (cp & 0x3F));
		count = 3;
	} else {
		bytes[0] = (unsigned char)(0xF0 | (cp >> 18));
		bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
		bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		bytes[3] = (unsigned char)(0x80 | (cp & 0x3F));
		count = 4;
	}
	for (size_t i = 0; i < count; i++) {
		if (!put_byte(out, capacity, length, bytes[i])) {
			return false;
		}
	}
	return true;
}

/**
 * Reads a string at the cursor into out, or validates and discards it when
 * out is NULL.
 */
static bool read_string(raiju_json_cursor *cur, char *out, size_t capacity) {
	size_t length = 0;
	if (out != NULL && capacity == 0) {
		return false;
	}
	if (!raiju_json_consume(cur, '"')) {
		return false;
	}
	while (cur->pos < cur->end) {
		unsigned char c = *cur->pos++;
		if (c == '"') {
			if (out != NULL) {
				out[length] = '\0';
			}
			return true;
		}
		if (c < 0x20) {
			return false;
		}
		if (c != '\\') {
			if (!put_byte(out, capacity, &length, c)) {
				return false;
			}
			continue;
		}
		if (cur->pos == cur->end) {
			return false;
		}
		unsigned char escape = *cur->pos++;
		unsigned char decoded;
		switch (escape) {
		case '"':
		case '\\':
		case '/':
			decoded = escape;
			break;
		case 'b':
			decoded = '\b';
			break;
		case 'f':
			decoded = '\f';
			break;
		case 'n':
			decoded = '\n';
			break;
		case 'r':
			decoded = '\r';
			break;
		case 't':
			decoded = '\t';
			break;
		case 'u':
			if (!parse_unicode_escape(cur, out, capacity, &length)) {
				return false;
			}
			continue;
		default:
			return false;
		}
		if (!put_byte(out, capacity, &length, decoded)) {
			return false;
		}
	}
	return false;
}

bool raiju_json_read_string(raiju_json_cursor *cur, char *out, size_t capacity) {
	return out != NULL && read_string(cur, out, capacity);
}

bool raiju_json_read_uint64(raiju_json_cursor *cur, uint64_t *out) {
	raiju_json_skip_whitespace(cur);
	if (cur->pos == cur->end || !is_digit(*cur->pos)) {
		return false;
	}
	if (*cur->pos == '0') {
		cur->pos++;
		*out = 0;
	} else {
		uint64_t value = 0;
		while (cur->pos < cur->end && is_digit(*cur->pos)) {
			uint64_t digit = (uint64_t)(*cur->pos - '0');
			if (value > (UINT64_MAX - digit) / 10) {
				return false;
			}
			value = value * 10 + digit;
			cur->pos++;
		}
		*out = value;
	}
	return cur->pos == cur->end || (*cur->pos != '.' && *cur->pos != 'e' && *cur->pos != 'E' && !is_digit(*cur->pos));
}

/**
 * Consumes the exact keyword text if it is next.
 */
static bool consume_literal(raiju_json_cursor *cur, const char *text) {
	size_t length = strlen(text);
	if ((size_t)(cur->end - cur->pos) < length || memcmp(cur->pos, text, length) != 0) {
		return false;
	}
	cur->pos += length;
	return true;
}

bool raiju_json_read_bool(raiju_json_cursor *cur, bool *out) {
	raiju_json_skip_whitespace(cur);
	if (consume_literal(cur, "true")) {
		*out = true;
		return true;
	}
	if (consume_literal(cur, "false")) {
		*out = false;
		return true;
	}
	return false;
}

/**
 * Consumes a run of decimal digits, requiring at least one.
 */
static bool skip_digits(raiju_json_cursor *cur) {
	if (cur->pos == cur->end || !is_digit(*cur->pos)) {
		return false;
	}
	while (cur->pos < cur->end && is_digit(*cur->pos)) {
		cur->pos++;
	}
	return true;
}

/**
 * Validates and skips a number following the strict JSON grammar.
 */
static bool skip_number(raiju_json_cursor *cur) {
	if (cur->pos < cur->end && *cur->pos == '-') {
		cur->pos++;
	}
	if (cur->pos < cur->end && *cur->pos == '0') {
		cur->pos++;
		if (cur->pos < cur->end && is_digit(*cur->pos)) {
			return false;
		}
	} else if (!skip_digits(cur)) {
		return false;
	}
	if (cur->pos < cur->end && *cur->pos == '.') {
		cur->pos++;
		if (!skip_digits(cur)) {
			return false;
		}
	}
	if (cur->pos < cur->end && (*cur->pos == 'e' || *cur->pos == 'E')) {
		cur->pos++;
		if (cur->pos < cur->end && (*cur->pos == '+' || *cur->pos == '-')) {
			cur->pos++;
		}
		if (!skip_digits(cur)) {
			return false;
		}
	}
	return true;
}

/**
 * Skips the members of an object whose opening brace was consumed.
 */
static bool skip_object_body(raiju_json_cursor *cur, int depth) {
	if (raiju_json_consume(cur, '}')) {
		return true;
	}
	for (;;) {
		if (!read_string(cur, NULL, 0) || !raiju_json_consume(cur, ':') || !raiju_json_skip_value(cur, depth + 1)) {
			return false;
		}
		if (raiju_json_consume(cur, '}')) {
			return true;
		}
		if (!raiju_json_consume(cur, ',')) {
			return false;
		}
	}
}

/**
 * Skips the elements of an array whose opening bracket was consumed.
 */
static bool skip_array_body(raiju_json_cursor *cur, int depth) {
	if (raiju_json_consume(cur, ']')) {
		return true;
	}
	for (;;) {
		if (!raiju_json_skip_value(cur, depth + 1)) {
			return false;
		}
		if (raiju_json_consume(cur, ']')) {
			return true;
		}
		if (!raiju_json_consume(cur, ',')) {
			return false;
		}
	}
}

bool raiju_json_skip_value(raiju_json_cursor *cur, int depth) {
	if (depth > RAIJU_JSON_MAX_DEPTH) {
		return false;
	}
	raiju_json_skip_whitespace(cur);
	if (cur->pos == cur->end) {
		return false;
	}
	switch (*cur->pos) {
	case '{':
		cur->pos++;
		return skip_object_body(cur, depth);
	case '[':
		cur->pos++;
		return skip_array_body(cur, depth);
	case '"':
		return read_string(cur, NULL, 0);
	case 't':
		return consume_literal(cur, "true");
	case 'f':
		return consume_literal(cur, "false");
	case 'n':
		return consume_literal(cur, "null");
	default:
		return skip_number(cur);
	}
}

/**
 * Appends length bytes, growing the buffer geometrically.
 */
static void write_bytes(raiju_json_writer *writer, const char *bytes, size_t length) {
	if (writer->failed) {
		return;
	}
	if (writer->length + length + 1 > writer->capacity) {
		size_t capacity = writer->capacity ? writer->capacity : 256;
		while (writer->length + length + 1 > capacity) {
			capacity *= 2;
		}
		char *grown = realloc(writer->data, capacity);
		if (grown == NULL) {
			writer->failed = true;
			return;
		}
		writer->data = grown;
		writer->capacity = capacity;
	}
	memcpy(writer->data + writer->length, bytes, length);
	writer->length += length;
	writer->data[writer->length] = '\0';
}

void raiju_json_write_raw(raiju_json_writer *writer, const char *text) {
	write_bytes(writer, text, strlen(text));
}

void raiju_json_write_string(raiju_json_writer *writer, const char *text) {
	write_bytes(writer, "\"", 1);
	for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; c++) {
		if (*c == '"' || *c == '\\') {
			char escaped[2] = {'\\', (char)*c};
			write_bytes(writer, escaped, 2);
		} else if (*c < 0x20 || *c == 0x7F) {
			char escaped[8];
			snprintf(escaped, sizeof escaped, "\\u%04x", (unsigned)*c);
			write_bytes(writer, escaped, 6);
		} else {
			write_bytes(writer, (const char *)c, 1);
		}
	}
	write_bytes(writer, "\"", 1);
}

void raiju_json_write_uint64(raiju_json_writer *writer, uint64_t value) {
	char digits[24];
	snprintf(digits, sizeof digits, "%llu", (unsigned long long)value);
	raiju_json_write_raw(writer, digits);
}

void raiju_json_write_bool(raiju_json_writer *writer, bool value) {
	raiju_json_write_raw(writer, value ? "true" : "false");
}
