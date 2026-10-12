#define _POSIX_C_SOURCE 200809L

#include "raiju/torrent.h"

#include <stdlib.h>
#include <string.h>

#define RAIJU_BENCODE_MAX_DEPTH 32

/** Decodes standard base64, refusing characters outside it and stray padding. */
static bool decode_base64(const char *text, size_t length, unsigned char **out_bytes, size_t *out_length) {
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	unsigned accumulator = 0;
	unsigned bits = 0;
	size_t data_length = length;
	size_t written = 0;
	unsigned char *bytes;

	while (data_length > 0 && text[data_length - 1] == '=') {
		data_length--;
	}
	if (length == 0 || length % 4 != 0 || length - data_length > 2) {
		return false;
	}
	bytes = malloc(length / 4 * 3);
	if (bytes == NULL) {
		return false;
	}
	for (size_t i = 0; i < data_length; i++) {
		const char *position = strchr(alphabet, text[i]);
		if (position == NULL) {
			free(bytes);
			return false;
		}
		accumulator = (accumulator << 6) | (unsigned)(position - alphabet);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			bytes[written++] = (unsigned char)((accumulator >> bits) & 0xFF);
		}
	}
	*out_bytes = bytes;
	*out_length = written;
	return true;
}

/** Skips one bencoded value, returning the byte after it or NULL when malformed. */
static const unsigned char *skip_value(const unsigned char *cursor, const unsigned char *limit, int depth) {
	if (cursor >= limit || depth > RAIJU_BENCODE_MAX_DEPTH) {
		return NULL;
	}
	if (*cursor == 'i') {
		const unsigned char *digits;
		cursor++;
		if (cursor < limit && *cursor == '-') {
			cursor++;
		}
		digits = cursor;
		while (cursor < limit && *cursor >= '0' && *cursor <= '9') {
			cursor++;
		}
		if (cursor == digits || cursor >= limit || *cursor != 'e') {
			return NULL;
		}
		return cursor + 1;
	}
	if (*cursor == 'l' || *cursor == 'd') {
		bool dictionary = *cursor == 'd';
		cursor++;
		while (cursor < limit && *cursor != 'e') {
			if (dictionary) {
				cursor = skip_value(cursor, limit, depth + 1);
				if (cursor == NULL) {
					return NULL;
				}
			}
			cursor = skip_value(cursor, limit, depth + 1);
			if (cursor == NULL) {
				return NULL;
			}
		}
		if (cursor >= limit) {
			return NULL;
		}
		return cursor + 1;
	}
	const unsigned char *digits = cursor;
	size_t length = 0;
	while (cursor < limit && *cursor >= '0' && *cursor <= '9') {
		if (length > (size_t)1 << 32) {
			return NULL;
		}
		length = length * 10 + (size_t)(*cursor - '0');
		cursor++;
	}
	if (cursor == digits || cursor >= limit || *cursor != ':') {
		return NULL;
	}
	cursor++;
	if ((size_t)(limit - cursor) < length) {
		return NULL;
	}
	return cursor + length;
}

/** Finds one key's value span inside the bencoded dictionary starting at start. */
static bool dict_find(const unsigned char *start, const unsigned char *limit, const char *key,
                      const unsigned char **out_start, const unsigned char **out_limit) {
	size_t key_length = strlen(key);
	const unsigned char *cursor;

	if (start >= limit || *start != 'd') {
		return false;
	}
	cursor = start + 1;
	while (cursor < limit && *cursor != 'e') {
		const unsigned char *digits = cursor;
		const unsigned char *value;
		const unsigned char *end;
		size_t length = 0;

		while (cursor < limit && *cursor >= '0' && *cursor <= '9') {
			if (length > (size_t)1 << 32) {
				return false;
			}
			length = length * 10 + (size_t)(*cursor - '0');
			cursor++;
		}
		if (cursor == digits || cursor >= limit || *cursor != ':') {
			return false;
		}
		cursor++;
		if ((size_t)(limit - cursor) < length) {
			return false;
		}
		value = cursor + length;
		end = skip_value(value, limit, 0);
		if (end == NULL) {
			return false;
		}
		if (length == key_length && memcmp(cursor, key, key_length) == 0) {
			*out_start = value;
			*out_limit = end;
			return true;
		}
		cursor = end;
	}
	return false;
}

/** Reads the contents of a bencoded byte string, excluding its length prefix. */
static bool byte_string_contents(const unsigned char *start, const unsigned char *limit, const unsigned char **out_start,
                                 const unsigned char **out_limit) {
	const unsigned char *digits = start;
	size_t declared = 0;

	while (start < limit && *start >= '0' && *start <= '9') {
		if (declared > (size_t)1 << 32) {
			return false;
		}
		declared = declared * 10 + (size_t)(*start - '0');
		start++;
	}
	if (start == digits || start >= limit || *start != ':') {
		return false;
	}
	start++;
	if ((size_t)(limit - start) != declared) {
		return false;
	}
	*out_start = start;
	*out_limit = limit;
	return true;
}

bool raiju_torrent_name_from_base64(const char *text, size_t length, char *out_name, size_t capacity) {
	const unsigned char *info_start = NULL;
	const unsigned char *info_limit = NULL;
	const unsigned char *name_start = NULL;
	const unsigned char *name_limit = NULL;
	const unsigned char *limit;
	unsigned char *bytes = NULL;
	size_t byte_length = 0;
	size_t name_length;
	bool found = false;

	if (capacity == 0 || !decode_base64(text, length, &bytes, &byte_length)) {
		return false;
	}
	limit = bytes + byte_length;
	if (dict_find(bytes, limit, "info", &info_start, &info_limit) &&
	    dict_find(info_start, info_limit, "name", &name_start, &name_limit) &&
	    byte_string_contents(name_start, name_limit, &name_start, &name_limit)) {
		name_length = (size_t)(name_limit - name_start);
		if (name_length > 0 && name_length < capacity && memchr(name_start, '\0', name_length) == NULL) {
			memcpy(out_name, name_start, name_length);
			out_name[name_length] = '\0';
			found = true;
		}
	}
	free(bytes);
	return found;
}
