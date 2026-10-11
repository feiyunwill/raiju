#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/art.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "raiju/fsutil.h"
#include "raiju/strbuf.h"

typedef struct {
	const char *start;
	const char *limit;
} html_span;

static bool valid_title_id(const char *text) {
	if (text == NULL || strlen(text) != 9) {
		return false;
	}
	if (strncmp(text, "CUSA", 4) != 0 && strncmp(text, "PPSA", 4) != 0) {
		return false;
	}
	for (int i = 4; i < 9; i++) {
		if (text[i] < '0' || text[i] > '9') {
			return false;
		}
	}
	return true;
}

bool raiju_art_title_id_valid(const char *text) {
	return valid_title_id(text);
}

static const char *source_for(const char *title_id) {
	return strncmp(title_id, "CUSA", 4) == 0 ? "orbispatches" : "prosperopatches";
}

bool raiju_art_page_url(const char *title_id, char *out, size_t capacity) {
	if (!valid_title_id(title_id)) {
		return false;
	}
	int written = snprintf(out, capacity, "https://%s.com/%s", source_for(title_id), title_id);
	return written >= 0 && (size_t)written < capacity;
}

static bool starts_with(const char *text, const char *limit, const char *needle) {
	size_t length = strlen(needle);
	return (size_t)(limit - text) >= length && memcmp(text, needle, length) == 0;
}

static const char *ci_find(const char *text, const char *limit, const char *needle) {
	size_t length = strlen(needle);
	for (const char *cursor = text; cursor + length <= limit; cursor++) {
		if (strncasecmp(cursor, needle, length) == 0) {
			return cursor;
		}
	}
	return NULL;
}

static bool is_hex_lower(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/**
 * Attempts the parse.ts image pattern at text: an image under this title's
 * own CDN folder. On a match returns true, sets kind to the captured kind and
 * end past the match. Mirrors the exact (case-sensitive) pattern.
 */
static bool match_image(const char *text, const char *limit, const char *title_id, const char *source,
                        const char **kind, const char **end) {
	const char *cursor = text;
	if (!starts_with(cursor, limit, "https://cdn.")) {
		return false;
	}
	cursor += strlen("https://cdn.");
	if (!starts_with(cursor, limit, source)) {
		return false;
	}
	cursor += strlen(source);
	if (!starts_with(cursor, limit, ".com/titles/")) {
		return false;
	}
	cursor += strlen(".com/titles/");
	if (!starts_with(cursor, limit, title_id)) {
		return false;
	}
	cursor += strlen(title_id);
	if (cursor >= limit || *cursor != '_') {
		return false;
	}
	cursor++;
	const char *hex_start = cursor;
	while (cursor < limit && is_hex_lower(*cursor)) {
		cursor++;
	}
	size_t hex_length = (size_t)(cursor - hex_start);
	if (hex_length < 16 || hex_length > 128) {
		return false;
	}
	if (cursor >= limit || *cursor != '/') {
		return false;
	}
	cursor++;
	const char *which;
	if (starts_with(cursor, limit, "icon0")) {
		which = "icon0";
		cursor += 5;
	} else if (starts_with(cursor, limit, "pic0")) {
		which = "pic0";
		cursor += 4;
	} else if (starts_with(cursor, limit, "logo")) {
		which = "logo";
		cursor += 4;
	} else {
		return false;
	}
	if (cursor >= limit || *cursor != '.') {
		return false;
	}
	cursor++;
	if (starts_with(cursor, limit, "webp")) {
		cursor += 4;
	} else if (starts_with(cursor, limit, "png")) {
		cursor += 3;
	} else if (starts_with(cursor, limit, "jpg")) {
		cursor += 3;
	} else {
		return false;
	}
	*kind = which;
	*end = cursor;
	return true;
}

static bool find_og_title(const char *html, const char *limit, html_span *out) {
	const char *cursor = html;
	while ((cursor = ci_find(cursor, limit, "<meta")) != NULL) {
		const char *tag_end = memchr(cursor, '>', (size_t)(limit - cursor));
		if (tag_end == NULL) {
			return false;
		}
		const char *property = ci_find(cursor, tag_end, "property=\"og:title\"");
		if (property != NULL) {
			const char *content = ci_find(property, tag_end, "content=\"");
			if (content != NULL) {
				const char *value = content + strlen("content=\"");
				const char *value_end = memchr(value, '"', (size_t)(tag_end - value));
				if (value_end != NULL) {
					out->start = value;
					out->limit = value_end;
					return true;
				}
			}
		}
		cursor = tag_end + 1;
	}
	return false;
}

static bool find_title_tag(const char *html, const char *limit, html_span *out) {
	const char *open = ci_find(html, limit, "<title>");
	if (open == NULL) {
		return false;
	}
	const char *value = open + strlen("<title>");
	const char *close = ci_find(value, limit, "</title>");
	if (close == NULL) {
		return false;
	}
	out->start = value;
	out->limit = close;
	return true;
}

static void append_utf8(raiju_strbuf *out, unsigned long codepoint) {
	unsigned char buffer[4];
	size_t length;
	if (codepoint > 0x10FFFF) {
		return;
	}
	if (codepoint < 0x80) {
		buffer[0] = (unsigned char)codepoint;
		length = 1;
	} else if (codepoint < 0x800) {
		buffer[0] = (unsigned char)(0xC0 | (codepoint >> 6));
		buffer[1] = (unsigned char)(0x80 | (codepoint & 0x3F));
		length = 2;
	} else if (codepoint < 0x10000) {
		buffer[0] = (unsigned char)(0xE0 | (codepoint >> 12));
		buffer[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
		buffer[2] = (unsigned char)(0x80 | (codepoint & 0x3F));
		length = 3;
	} else {
		buffer[0] = (unsigned char)(0xF0 | (codepoint >> 18));
		buffer[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
		buffer[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
		buffer[3] = (unsigned char)(0x80 | (codepoint & 0x3F));
		length = 4;
	}
	raiju_strbuf_append_bytes(out, buffer, length);
}

static int decimal_digit(char c) {
	return c >= '0' && c <= '9' ? c - '0' : -1;
}

static int hex_digit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static bool match_entity(const char *cursor, const char *limit, const char *entity) {
	return starts_with(cursor, limit, entity);
}

static void decode_entities(const char *text, size_t length, raiju_strbuf *out) {
	const char *cursor = text;
	const char *limit = text + length;
	while (cursor < limit) {
		if (*cursor == '&' && cursor + 1 < limit && cursor[1] == '#') {
			const char *digits = cursor + 2;
			bool hex = digits < limit && (*digits == 'x' || *digits == 'X');
			const char *number = hex ? digits + 1 : digits;
			unsigned long codepoint = 0;
			size_t count = 0;
			while (number < limit) {
				int value = hex ? hex_digit(*number) : decimal_digit(*number);
				if (value < 0) {
					break;
				}
				codepoint = codepoint * (hex ? 16u : 10u) + (unsigned long)value;
				number++;
				count++;
			}
			if (count > 0 && number < limit && *number == ';') {
				append_utf8(out, codepoint);
				cursor = number + 1;
				continue;
			}
		}
		if (match_entity(cursor, limit, "&quot;")) {
			raiju_strbuf_append_char(out, '"');
			cursor += 6;
		} else if (match_entity(cursor, limit, "&#39;")) {
			raiju_strbuf_append_char(out, '\'');
			cursor += 5;
		} else if (match_entity(cursor, limit, "&apos;")) {
			raiju_strbuf_append_char(out, '\'');
			cursor += 6;
		} else if (match_entity(cursor, limit, "&lt;")) {
			raiju_strbuf_append_char(out, '<');
			cursor += 4;
		} else if (match_entity(cursor, limit, "&gt;")) {
			raiju_strbuf_append_char(out, '>');
			cursor += 4;
		} else if (match_entity(cursor, limit, "&amp;")) {
			raiju_strbuf_append_char(out, '&');
			cursor += 5;
		} else {
			raiju_strbuf_append_char(out, *cursor);
			cursor++;
		}
	}
}

static void strip_name_prefix(char *name, const char *title_id) {
	size_t title_length = strlen(title_id);
	if (strncmp(name, title_id, title_length) != 0) {
		return;
	}
	char *cursor = name + title_length;
	while (isspace((unsigned char)*cursor)) {
		cursor++;
	}
	if (*cursor != ':') {
		return;
	}
	cursor++;
	while (isspace((unsigned char)*cursor)) {
		cursor++;
	}
	memmove(name, cursor, strlen(cursor) + 1);
}

static void strip_name_suffix(char *name) {
	char *bar = strrchr(name, '|');
	if (bar == NULL) {
		return;
	}
	char *cursor = bar + 1;
	while (isspace((unsigned char)*cursor)) {
		cursor++;
	}
	const char *site;
	if (strncasecmp(cursor, "orbispatches.com", 16) == 0) {
		site = cursor + 16;
	} else if (strncasecmp(cursor, "prosperopatches.com", 19) == 0) {
		site = cursor + 19;
	} else {
		return;
	}
	while (isspace((unsigned char)*site)) {
		site++;
	}
	if (*site != '\0') {
		return;
	}
	char *end = bar;
	while (end > name && isspace((unsigned char)end[-1])) {
		end--;
	}
	*end = '\0';
}

static char *clean_name(const char *text, size_t length, const char *title_id) {
	raiju_strbuf decoded;
	raiju_strbuf_init(&decoded);
	decode_entities(text, length, &decoded);
	if (decoded.failed) {
		raiju_strbuf_free(&decoded);
		return NULL;
	}
	char *name = strdup(raiju_strbuf_cstr(&decoded));
	raiju_strbuf_free(&decoded);
	if (name == NULL) {
		return NULL;
	}
	strip_name_prefix(name, title_id);
	strip_name_suffix(name);
	char *start = name;
	while (isspace((unsigned char)*start)) {
		start++;
	}
	if (start != name) {
		memmove(name, start, strlen(start) + 1);
	}
	size_t trimmed = strlen(name);
	while (trimmed > 0 && isspace((unsigned char)name[trimmed - 1])) {
		name[--trimmed] = '\0';
	}
	if (name[0] == '\0') {
		free(name);
		return NULL;
	}
	return name;
}

static char *copy_span(const char *start, const char *end) {
	size_t length = (size_t)(end - start);
	char *copy = malloc(length + 1);
	if (copy == NULL) {
		return NULL;
	}
	memcpy(copy, start, length);
	copy[length] = '\0';
	return copy;
}

raiju_art *raiju_art_parse(const char *title_id, const char *html, size_t html_length) {
	if (!valid_title_id(title_id)) {
		return NULL;
	}
	const char *limit = html + html_length;
	const char *source = source_for(title_id);
	char *icon = NULL;
	char *background = NULL;
	char *logo = NULL;
	for (const char *cursor = html; cursor + strlen("https://cdn.") <= limit; cursor++) {
		if (!starts_with(cursor, limit, "https://cdn.")) {
			continue;
		}
		const char *kind;
		const char *end;
		if (!match_image(cursor, limit, title_id, source, &kind, &end)) {
			continue;
		}
		if (strcmp(kind, "icon0") == 0 && icon == NULL) {
			icon = copy_span(cursor, end);
		} else if (strcmp(kind, "pic0") == 0 && background == NULL) {
			background = copy_span(cursor, end);
		} else if (strcmp(kind, "logo") == 0 && logo == NULL) {
			logo = copy_span(cursor, end);
		}
	}
	if (icon == NULL) {
		free(background);
		free(logo);
		return NULL;
	}
	html_span title;
	char *name = NULL;
	if (find_og_title(html, limit, &title) || find_title_tag(html, limit, &title)) {
		name = clean_name(title.start, (size_t)(title.limit - title.start), title_id);
	}
	raiju_art *art = calloc(1, sizeof *art);
	if (art == NULL) {
		free(icon);
		free(background);
		free(logo);
		free(name);
		return NULL;
	}
	snprintf(art->title_id, sizeof art->title_id, "%s", title_id);
	art->icon = icon;
	art->background = background;
	art->logo = logo;
	art->name = name;
	art->source = source;
	return art;
}

static void append_nullable(raiju_strbuf *out, const char *value) {
	if (value == NULL) {
		raiju_strbuf_append(out, "null");
	} else {
		raiju_strbuf_append_json_string(out, value);
	}
}

char *raiju_art_to_json(const raiju_art *art, size_t *out_length) {
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	raiju_strbuf_append(&document, "{\"titleId\":");
	raiju_strbuf_append_json_string(&document, art->title_id);
	raiju_strbuf_append(&document, ",\"name\":");
	append_nullable(&document, art->name);
	raiju_strbuf_append(&document, ",\"icon\":");
	raiju_strbuf_append_json_string(&document, art->icon);
	raiju_strbuf_append(&document, ",\"background\":");
	append_nullable(&document, art->background);
	raiju_strbuf_append(&document, ",\"logo\":");
	append_nullable(&document, art->logo);
	raiju_strbuf_append(&document, ",\"source\":");
	raiju_strbuf_append_json_string(&document, art->source);
	raiju_strbuf_append_char(&document, '}');
	char *result = document.failed ? NULL : strdup(raiju_strbuf_cstr(&document));
	if (result != NULL && out_length != NULL) {
		*out_length = document.length;
	}
	raiju_strbuf_free(&document);
	return result;
}

void raiju_art_free(raiju_art *art) {
	if (art == NULL) {
		return;
	}
	free(art->name);
	free(art->icon);
	free(art->background);
	free(art->logo);
	free(art);
}

static bool cache_path(const char *dir, const char *title_id, char *out, size_t capacity) {
	int written = snprintf(out, capacity, "%s/%s.json", dir, title_id);
	return written >= 0 && (size_t)written < capacity;
}

char *raiju_art_cache_read(const char *dir, const char *title_id, uint64_t now, size_t *out_length) {
	if (!valid_title_id(title_id)) {
		return NULL;
	}
	char path[RAIJU_PATH_MAX];
	if (!cache_path(dir, title_id, path, sizeof path)) {
		return NULL;
	}
	struct stat info;
	if (stat(path, &info) != 0) {
		return NULL;
	}
	if (now >= (uint64_t)info.st_mtime && now - (uint64_t)info.st_mtime >= RAIJU_ART_TTL_SECONDS) {
		return NULL;
	}
	size_t length = 0;
	char *document = raiju_read_file_ex(path, 1u * 1024u * 1024u, true, &length);
	if (document != NULL && out_length != NULL) {
		*out_length = length;
	}
	return document;
}

bool raiju_art_cache_write(const char *dir, const char *title_id, const char *json, size_t length) {
	if (!valid_title_id(title_id) || !raiju_mkdir_p(dir)) {
		return false;
	}
	char path[RAIJU_PATH_MAX];
	if (!cache_path(dir, title_id, path, sizeof path)) {
		return false;
	}
	return raiju_atomic_write(path, json, length);
}
