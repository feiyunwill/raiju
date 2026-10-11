#include "raiju/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool reserve(raiju_strbuf *buffer, size_t extra) {
	if (buffer->failed) {
		return false;
	}
	size_t needed = buffer->length + extra + 1;
	if (needed <= buffer->capacity) {
		return true;
	}
	size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
	while (capacity < needed) {
		capacity *= 2;
	}
	char *grown = realloc(buffer->data, capacity);
	if (grown == NULL) {
		buffer->failed = true;
		return false;
	}
	buffer->data = grown;
	buffer->capacity = capacity;
	return true;
}

void raiju_strbuf_init(raiju_strbuf *buffer) {
	buffer->data = NULL;
	buffer->length = 0;
	buffer->capacity = 0;
	buffer->failed = false;
}

void raiju_strbuf_append(raiju_strbuf *buffer, const char *text) {
	size_t length = strlen(text);
	if (!reserve(buffer, length)) {
		return;
	}
	memcpy(buffer->data + buffer->length, text, length);
	buffer->length += length;
	buffer->data[buffer->length] = '\0';
}

void raiju_strbuf_append_bytes(raiju_strbuf *buffer, const void *bytes, size_t length) {
	if (!reserve(buffer, length)) {
		return;
	}
	memcpy(buffer->data + buffer->length, bytes, length);
	buffer->length += length;
	buffer->data[buffer->length] = '\0';
}

void raiju_strbuf_appendf(raiju_strbuf *buffer, const char *format, ...) {
	char stack[512];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(stack, sizeof stack, format, args);
	va_end(args);
	if (length < 0) {
		buffer->failed = true;
		return;
	}
	if ((size_t)length < sizeof stack) {
		raiju_strbuf_append(buffer, stack);
		return;
	}
	if (!reserve(buffer, (size_t)length)) {
		return;
	}
	va_start(args, format);
	vsnprintf(buffer->data + buffer->length, (size_t)length + 1, format, args);
	va_end(args);
	buffer->length += (size_t)length;
}

void raiju_strbuf_append_char(raiju_strbuf *buffer, char value) {
	if (!reserve(buffer, 1)) {
		return;
	}
	buffer->data[buffer->length++] = value;
	buffer->data[buffer->length] = '\0';
}

void raiju_strbuf_append_uint(raiju_strbuf *buffer, unsigned long long value) {
	raiju_strbuf_appendf(buffer, "%llu", value);
}

void raiju_strbuf_append_json_string(raiju_strbuf *buffer, const char *text) {
	static const char digits[] = "0123456789abcdef";
	raiju_strbuf_append_char(buffer, '"');
	for (const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0'; cursor++) {
		unsigned char value = *cursor;
		if (value == '"' || value == '\\') {
			raiju_strbuf_append_char(buffer, '\\');
			raiju_strbuf_append_char(buffer, (char)value);
		} else if (value < 0x20) {
			raiju_strbuf_append(buffer, "\\u00");
			raiju_strbuf_append_char(buffer, digits[(value >> 4) & 0xf]);
			raiju_strbuf_append_char(buffer, digits[value & 0xf]);
		} else {
			raiju_strbuf_append_char(buffer, (char)value);
		}
	}
	raiju_strbuf_append_char(buffer, '"');
}

const char *raiju_strbuf_cstr(raiju_strbuf *buffer) {
	if (buffer->data == NULL) {
		if (!reserve(buffer, 0)) {
			return "";
		}
		buffer->data[0] = '\0';
	}
	return buffer->data;
}

void raiju_strbuf_free(raiju_strbuf *buffer) {
	free(buffer->data);
	buffer->data = NULL;
	buffer->length = 0;
	buffer->capacity = 0;
	buffer->failed = false;
}
