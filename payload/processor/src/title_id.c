#include "raiju/title_id.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64
#define TITLE_ID_KEY "titleId"
#define TITLE_NAME_KEY "titleName"
#define LOCALIZED_KEY "localizedParameters"
#define DEFAULT_LANGUAGE_KEY "defaultLanguage"
#define CAPTURE_CAPACITY 256
#define PARAM_JSON_RELATIVE "sce_sys/param.json"

typedef struct {
	const unsigned char *pos;
	const unsigned char *end;
} json_cursor;

typedef struct {
	char data[CAPTURE_CAPACITY];
	size_t length;
	bool overflow;
} string_sink;

typedef struct {
	bool seen;
	bool duplicate;
	bool is_string;
	string_sink value;
} title_capture;

/**
 * What a display-name pass kept from localizedParameters: the default language's
 * titleName, and the first one seen as a fallback for a document that names its
 * default only after listing a language. duplicate marks a repeated key, as the
 * title-ID pass does.
 */
typedef struct {
	string_sink default_language;
	string_sink chosen;
	string_sink first;
	bool have_default;
	bool have_chosen;
	bool have_first;
	bool seen_localized;
	bool duplicate;
} name_capture;

/**
 * Returns whether c is an ASCII decimal digit, independent of locale.
 */
static bool is_digit(int c) {
	return c >= '0' && c <= '9';
}

/**
 * Returns whether c is an ASCII uppercase letter, independent of locale.
 */
static bool is_upper(int c) {
	return c >= 'A' && c <= 'Z';
}

/**
 * Appends one byte to a sink, recording overflow instead of writing past it.
 * A NULL sink discards the byte.
 */
static void sink_put(string_sink *sink, unsigned char c) {
	if (sink == NULL) {
		return;
	}
	if (sink->length < sizeof sink->data) {
		sink->data[sink->length++] = (char)c;
	} else {
		sink->overflow = true;
	}
}

/**
 * Appends a Unicode code point to a sink as UTF-8.
 */
static void sink_put_code_point(string_sink *sink, uint32_t cp) {
	if (cp < 0x80) {
		sink_put(sink, (unsigned char)cp);
	} else if (cp < 0x800) {
		sink_put(sink, (unsigned char)(0xC0 | (cp >> 6)));
		sink_put(sink, (unsigned char)(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		sink_put(sink, (unsigned char)(0xE0 | (cp >> 12)));
		sink_put(sink, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
		sink_put(sink, (unsigned char)(0x80 | (cp & 0x3F)));
	} else {
		sink_put(sink, (unsigned char)(0xF0 | (cp >> 18)));
		sink_put(sink, (unsigned char)(0x80 | ((cp >> 12) & 0x3F)));
		sink_put(sink, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
		sink_put(sink, (unsigned char)(0x80 | (cp & 0x3F)));
	}
}

/**
 * Skips JSON insignificant whitespace.
 */
static void skip_whitespace(json_cursor *cur) {
	while (cur->pos < cur->end && (*cur->pos == ' ' || *cur->pos == '\t' || *cur->pos == '\n' || *cur->pos == '\r')) {
		cur->pos++;
	}
}

/**
 * Consumes byte c if it is next in the input.
 */
static bool consume(json_cursor *cur, unsigned char c) {
	if (cur->pos < cur->end && *cur->pos == c) {
		cur->pos++;
		return true;
	}
	return false;
}

/**
 * Parses four hexadecimal digits of a \u escape.
 */
static bool parse_hex4(json_cursor *cur, uint32_t *out) {
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
 * Decodes a \u escape, including surrogate pairs; lone surrogates are invalid.
 */
static bool parse_unicode_escape(json_cursor *cur, string_sink *sink) {
	uint32_t cp;
	if (!parse_hex4(cur, &cp)) {
		return false;
	}
	if (cp >= 0xDC00 && cp <= 0xDFFF) {
		return false;
	}
	if (cp >= 0xD800 && cp <= 0xDBFF) {
		uint32_t low;
		if (!consume(cur, '\\') || !consume(cur, 'u') || !parse_hex4(cur, &low) || low < 0xDC00 || low > 0xDFFF) {
			return false;
		}
		cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
	}
	sink_put_code_point(sink, cp);
	return true;
}

/**
 * Parses a JSON string starting at its opening quote, decoding it into sink.
 */
static bool parse_string(json_cursor *cur, string_sink *sink) {
	if (!consume(cur, '"')) {
		return false;
	}
	while (cur->pos < cur->end) {
		unsigned char c = *cur->pos++;
		if (c == '"') {
			return true;
		}
		if (c < 0x20) {
			return false;
		}
		if (c != '\\') {
			sink_put(sink, c);
			continue;
		}
		if (cur->pos == cur->end) {
			return false;
		}
		unsigned char escape = *cur->pos++;
		switch (escape) {
		case '"':
		case '\\':
		case '/':
			sink_put(sink, escape);
			break;
		case 'b':
			sink_put(sink, '\b');
			break;
		case 'f':
			sink_put(sink, '\f');
			break;
		case 'n':
			sink_put(sink, '\n');
			break;
		case 'r':
			sink_put(sink, '\r');
			break;
		case 't':
			sink_put(sink, '\t');
			break;
		case 'u':
			if (!parse_unicode_escape(cur, sink)) {
				return false;
			}
			break;
		default:
			return false;
		}
	}
	return false;
}

/**
 * Consumes a run of decimal digits, requiring at least one.
 */
static bool parse_digits(json_cursor *cur) {
	if (cur->pos == cur->end || !is_digit(*cur->pos)) {
		return false;
	}
	while (cur->pos < cur->end && is_digit(*cur->pos)) {
		cur->pos++;
	}
	return true;
}

/**
 * Parses a JSON number following the strict JSON grammar.
 */
static bool parse_number(json_cursor *cur) {
	consume(cur, '-');
	if (consume(cur, '0')) {
		if (cur->pos < cur->end && is_digit(*cur->pos)) {
			return false;
		}
	} else if (!parse_digits(cur)) {
		return false;
	}
	if (consume(cur, '.') && !parse_digits(cur)) {
		return false;
	}
	if (consume(cur, 'e') || consume(cur, 'E')) {
		if (!consume(cur, '+')) {
			consume(cur, '-');
		}
		if (!parse_digits(cur)) {
			return false;
		}
	}
	return true;
}

/**
 * Consumes the exact keyword text (true, false or null).
 */
static bool parse_literal(json_cursor *cur, const char *text) {
	size_t length = strlen(text);
	if ((size_t)(cur->end - cur->pos) < length || memcmp(cur->pos, text, length) != 0) {
		return false;
	}
	cur->pos += length;
	return true;
}

static bool parse_value(json_cursor *cur, int depth);

/**
 * Parses a JSON object. When capture is set, top-level "titleId" values are
 * recorded in it.
 */
static bool parse_object(json_cursor *cur, int depth, title_capture *capture) {
	if (depth > JSON_MAX_DEPTH || !consume(cur, '{')) {
		return false;
	}
	skip_whitespace(cur);
	if (consume(cur, '}')) {
		return true;
	}
	for (;;) {
		skip_whitespace(cur);
		string_sink key = {{0}, 0, false};
		if (!parse_string(cur, capture ? &key : NULL)) {
			return false;
		}
		skip_whitespace(cur);
		if (!consume(cur, ':')) {
			return false;
		}
		skip_whitespace(cur);
		bool is_title_key = capture != NULL && !key.overflow && key.length == strlen(TITLE_ID_KEY) &&
		                    memcmp(key.data, TITLE_ID_KEY, key.length) == 0;
		if (is_title_key) {
			if (capture->seen) {
				capture->duplicate = true;
			}
			capture->seen = true;
			capture->is_string = cur->pos < cur->end && *cur->pos == '"';
			if (capture->is_string ? !parse_string(cur, &capture->value) : !parse_value(cur, depth)) {
				return false;
			}
		} else if (!parse_value(cur, depth)) {
			return false;
		}
		skip_whitespace(cur);
		if (consume(cur, '}')) {
			return true;
		}
		if (!consume(cur, ',')) {
			return false;
		}
	}
}

/**
 * Parses a JSON array.
 */
static bool parse_array(json_cursor *cur, int depth) {
	if (depth > JSON_MAX_DEPTH || !consume(cur, '[')) {
		return false;
	}
	skip_whitespace(cur);
	if (consume(cur, ']')) {
		return true;
	}
	for (;;) {
		if (!parse_value(cur, depth)) {
			return false;
		}
		skip_whitespace(cur);
		if (consume(cur, ']')) {
			return true;
		}
		if (!consume(cur, ',')) {
			return false;
		}
	}
}

/**
 * Parses any JSON value; depth is the nesting level of its container.
 */
static bool parse_value(json_cursor *cur, int depth) {
	skip_whitespace(cur);
	if (cur->pos == cur->end) {
		return false;
	}
	switch (*cur->pos) {
	case '{':
		return parse_object(cur, depth + 1, NULL);
	case '[':
		return parse_array(cur, depth + 1);
	case '"':
		return parse_string(cur, NULL);
	case 't':
		return parse_literal(cur, "true");
	case 'f':
		return parse_literal(cur, "false");
	case 'n':
		return parse_literal(cur, "null");
	default:
		if (*cur->pos == '-' || is_digit(*cur->pos)) {
			return parse_number(cur);
		}
		return false;
	}
}


/** Returns whether a parsed key equals a keyword. */
static bool key_matches(const string_sink *key, const char *text) {
	return !key->overflow && key->length == strlen(text) && memcmp(key->data, text, key->length) == 0;
}

/**
 * Parses one language object inside localizedParameters, keeping its
 * titleName and validating the rest of the object as JSON.
 */
static bool parse_language_object(json_cursor *cur, int depth, string_sink *name, bool *found) {
	if (depth > JSON_MAX_DEPTH || !consume(cur, '{')) {
		return false;
	}
	skip_whitespace(cur);
	if (consume(cur, '}')) {
		return true;
	}
	for (;;) {
		skip_whitespace(cur);
		string_sink key = {{0}, 0, false};
		if (!parse_string(cur, &key)) {
			return false;
		}
		skip_whitespace(cur);
		if (!consume(cur, ':')) {
			return false;
		}
		skip_whitespace(cur);
		if (key_matches(&key, TITLE_NAME_KEY) && cur->pos < cur->end && *cur->pos == '"') {
			string_sink value = {{0}, 0, false};
			if (!parse_string(cur, &value)) {
				return false;
			}
			if (!*found) {
				*name = value;
				*found = true;
			}
		} else if (!parse_value(cur, depth)) {
			return false;
		}
		skip_whitespace(cur);
		if (consume(cur, '}')) {
			return true;
		}
		if (!consume(cur, ',')) {
			return false;
		}
	}
}

/**
 * Parses localizedParameters: keeps the default language's titleName, and the
 * first one seen for a document that lists languages before naming its default.
 */
static bool parse_localized_object(json_cursor *cur, int depth, name_capture *capture) {
	if (depth > JSON_MAX_DEPTH || !consume(cur, '{')) {
		return false;
	}
	skip_whitespace(cur);
	if (consume(cur, '}')) {
		return true;
	}
	for (;;) {
		skip_whitespace(cur);
		string_sink key = {{0}, 0, false};
		if (!parse_string(cur, &key)) {
			return false;
		}
		skip_whitespace(cur);
		if (!consume(cur, ':')) {
			return false;
		}
		skip_whitespace(cur);
		if (key_matches(&key, DEFAULT_LANGUAGE_KEY) && cur->pos < cur->end && *cur->pos == '"') {
			string_sink value = {{0}, 0, false};
			if (!parse_string(cur, &value)) {
				return false;
			}
			if (!capture->have_default) {
				capture->default_language = value;
				capture->have_default = true;
			}
		} else if (cur->pos < cur->end && *cur->pos == '{') {
			string_sink name = {{0}, 0, false};
			bool found = false;
			if (!parse_language_object(cur, depth + 1, &name, &found)) {
				return false;
			}
			if (found) {
				if (!capture->have_first) {
					capture->first = name;
					capture->have_first = true;
				}
				if (!capture->have_chosen && capture->have_default && capture->default_language.length == key.length &&
				    memcmp(capture->default_language.data, key.data, key.length) == 0) {
					capture->chosen = name;
					capture->have_chosen = true;
				}
			}
		} else if (!parse_value(cur, depth)) {
			return false;
		}
		skip_whitespace(cur);
		if (consume(cur, '}')) {
			return true;
		}
		if (!consume(cur, ',')) {
			return false;
		}
	}
}

/**
 * Parses a top-level object looking only for localizedParameters.
 */
static bool parse_object_for_name(json_cursor *cur, int depth, name_capture *capture) {
	if (depth > JSON_MAX_DEPTH || !consume(cur, '{')) {
		return false;
	}
	skip_whitespace(cur);
	if (consume(cur, '}')) {
		return true;
	}
	for (;;) {
		skip_whitespace(cur);
		string_sink key = {{0}, 0, false};
		if (!parse_string(cur, &key)) {
			return false;
		}
		skip_whitespace(cur);
		if (!consume(cur, ':')) {
			return false;
		}
		skip_whitespace(cur);
		if (key_matches(&key, LOCALIZED_KEY)) {
			if (capture->seen_localized) {
				capture->duplicate = true;
			}
			capture->seen_localized = true;
			if (cur->pos < cur->end && *cur->pos == '{') {
				if (!parse_localized_object(cur, depth + 1, capture)) {
					return false;
				}
			} else if (!parse_value(cur, depth)) {
				return false;
			}
		} else if (!parse_value(cur, depth)) {
			return false;
		}
		skip_whitespace(cur);
		if (consume(cur, '}')) {
			return true;
		}
		if (!consume(cur, ',')) {
			return false;
		}
	}
}

bool raiju_display_name_from_param_json(const char *json, size_t json_len, char *out, size_t out_size) {
	if (json == NULL || out == NULL || out_size == 0) {
		return false;
	}
	out[0] = '\0';
	if (json_len > RAIJU_PARAM_JSON_MAX_BYTES) {
		return false;
	}
	json_cursor cur = {(const unsigned char *)json, (const unsigned char *)json + json_len};
	static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
	if (json_len >= sizeof bom && memcmp(cur.pos, bom, sizeof bom) == 0) {
		cur.pos += sizeof bom;
	}
	name_capture capture;
	memset(&capture, 0, sizeof capture);
	skip_whitespace(&cur);
	if (!parse_object_for_name(&cur, 1, &capture)) {
		return false;
	}
	skip_whitespace(&cur);
	if (cur.pos != cur.end || capture.duplicate) {
		return false;
	}
	const string_sink *picked = capture.have_chosen ? &capture.chosen
	                          : capture.have_first   ? &capture.first
	                                                 : NULL;
	if (picked == NULL || picked->overflow || picked->length == 0) {
		return false;
	}
	size_t length = picked->length < out_size - 1 ? picked->length : out_size - 1;
	memcpy(out, picked->data, length);
	out[length] = '\0';
	return true;
}

bool raiju_title_id_valid(const char *text) {
	if (text == NULL) {
		return false;
	}
	for (int i = 0; i < RAIJU_TITLE_ID_LEN; i++) {
		bool expected = i < 4 ? is_upper(text[i]) : is_digit(text[i]);
		if (!expected) {
			return false;
		}
	}
	return text[RAIJU_TITLE_ID_LEN] == '\0';
}

raiju_param_status raiju_title_id_from_param_json(const char *json, size_t json_len, char out[RAIJU_TITLE_ID_LEN + 1]) {
	if (json == NULL || out == NULL) {
		return RAIJU_PARAM_MALFORMED;
	}
	if (json_len > RAIJU_PARAM_JSON_MAX_BYTES) {
		return RAIJU_PARAM_TOO_LARGE;
	}
	json_cursor cur = {(const unsigned char *)json, (const unsigned char *)json + json_len};
	static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
	if (json_len >= sizeof bom && memcmp(cur.pos, bom, sizeof bom) == 0) {
		cur.pos += sizeof bom;
	}
	title_capture capture;
	memset(&capture, 0, sizeof capture);
	skip_whitespace(&cur);
	if (!parse_object(&cur, 1, &capture)) {
		return RAIJU_PARAM_MALFORMED;
	}
	skip_whitespace(&cur);
	if (cur.pos != cur.end || capture.duplicate) {
		return RAIJU_PARAM_MALFORMED;
	}
	if (!capture.seen) {
		return RAIJU_PARAM_NO_TITLE_ID;
	}
	if (!capture.is_string || capture.value.overflow || capture.value.length != RAIJU_TITLE_ID_LEN) {
		return RAIJU_PARAM_BAD_TITLE_ID;
	}
	char candidate[RAIJU_TITLE_ID_LEN + 1];
	memcpy(candidate, capture.value.data, RAIJU_TITLE_ID_LEN);
	candidate[RAIJU_TITLE_ID_LEN] = '\0';
	if (!raiju_title_id_valid(candidate)) {
		return RAIJU_PARAM_BAD_TITLE_ID;
	}
	memcpy(out, candidate, sizeof candidate);
	return RAIJU_PARAM_OK;
}

raiju_param_status raiju_title_id_from_folder(const char *dump_dir, char out[RAIJU_TITLE_ID_LEN + 1]) {
	if (dump_dir == NULL || out == NULL) {
		return RAIJU_PARAM_IO_ERROR;
	}
	size_t dir_len = strlen(dump_dir);
	bool needs_slash = dir_len > 0 && dump_dir[dir_len - 1] != '/';
	size_t path_len = dir_len + (needs_slash ? 1 : 0) + strlen(PARAM_JSON_RELATIVE);
	char *path = malloc(path_len + 1);
	if (path == NULL) {
		return RAIJU_PARAM_IO_ERROR;
	}
	snprintf(path, path_len + 1, "%s%s%s", dump_dir, needs_slash ? "/" : "", PARAM_JSON_RELATIVE);
	FILE *stream = fopen(path, "rb");
	free(path);
	if (stream == NULL) {
		return RAIJU_PARAM_IO_ERROR;
	}
	char *buffer = malloc(RAIJU_PARAM_JSON_MAX_BYTES + 1);
	if (buffer == NULL) {
		fclose(stream);
		return RAIJU_PARAM_IO_ERROR;
	}
	size_t length = fread(buffer, 1, RAIJU_PARAM_JSON_MAX_BYTES + 1, stream);
	int read_failed = ferror(stream);
	fclose(stream);
	raiju_param_status status;
	if (read_failed) {
		status = RAIJU_PARAM_IO_ERROR;
	} else if (length > RAIJU_PARAM_JSON_MAX_BYTES) {
		status = RAIJU_PARAM_TOO_LARGE;
	} else {
		status = raiju_title_id_from_param_json(buffer, length, out);
	}
	free(buffer);
	return status;
}

bool raiju_title_id_from_name(const char *file_name, char out[RAIJU_TITLE_ID_LEN + 1]) {
	if (file_name == NULL || out == NULL) {
		return false;
	}
	for (const char *p = file_name; *p != '\0'; p++) {
		if (strncmp(p, "PPSA", 4) != 0 && strncmp(p, "CUSA", 4) != 0) {
			continue;
		}
		bool digits = true;
		for (int i = 4; i < RAIJU_TITLE_ID_LEN && digits; i++) {
			digits = is_digit(p[i]);
		}
		if (digits) {
			memcpy(out, p, RAIJU_TITLE_ID_LEN);
			out[RAIJU_TITLE_ID_LEN] = '\0';
			return true;
		}
	}
	return false;
}

raiju_title_resolution raiju_resolve_title_id(const char *param_title_id, const char *file_name) {
	raiju_title_resolution resolution;
	memset(&resolution, 0, sizeof resolution);
	bool has_param = raiju_title_id_valid(param_title_id);
	bool has_name = raiju_title_id_from_name(file_name, resolution.name_title_id);
	if (has_param) {
		memcpy(resolution.title_id, param_title_id, RAIJU_TITLE_ID_LEN + 1);
		resolution.source = RAIJU_TITLE_SOURCE_PARAM_JSON;
		resolution.names_disagree = has_name && strcmp(resolution.title_id, resolution.name_title_id) != 0;
	} else if (has_name) {
		memcpy(resolution.title_id, resolution.name_title_id, RAIJU_TITLE_ID_LEN + 1);
		resolution.source = RAIJU_TITLE_SOURCE_FILE_NAME;
	}
	return resolution;
}

const char *raiju_param_status_name(raiju_param_status status) {
	switch (status) {
	case RAIJU_PARAM_OK:
		return "ok";
	case RAIJU_PARAM_MALFORMED:
		return "malformed";
	case RAIJU_PARAM_NO_TITLE_ID:
		return "no_title_id";
	case RAIJU_PARAM_BAD_TITLE_ID:
		return "bad_title_id";
	case RAIJU_PARAM_TOO_LARGE:
		return "too_large";
	case RAIJU_PARAM_IO_ERROR:
		return "io_error";
	default:
		return "unknown";
	}
}
