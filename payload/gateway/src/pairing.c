#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/pairing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raiju/fsutil.h"
#include "raiju/strbuf.h"

#define DEVICES_FILE_MAX (1u * 1024u * 1024u)

static bool constant_time_equal(const char *a, const char *b) {
	size_t a_len = strlen(a);
	size_t b_len = strlen(b);
	size_t max = a_len > b_len ? a_len : b_len;
	unsigned char diff = (unsigned char)(a_len ^ b_len);
	for (size_t i = 0; i < max; i++) {
		unsigned char ca = i < a_len ? (unsigned char)a[i] : 0;
		unsigned char cb = i < b_len ? (unsigned char)b[i] : 0;
		diff |= (unsigned char)(ca ^ cb);
	}
	return diff == 0;
}

static bool valid_hex(const char *text, size_t length) {
	for (size_t i = 0; i < length; i++) {
		char c = text[i];
		bool digit = c >= '0' && c <= '9';
		bool lower = c >= 'a' && c <= 'f';
		if (!digit && !lower) {
			return false;
		}
	}
	return true;
}

bool raiju_pairing_new_code(raiju_pairing *pairing) {
	unsigned char raw[4];
	if (pairing->platform->random_bytes(pairing->platform->ctx, raw, sizeof raw) != 0) {
		return false;
	}
	uint32_t value = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
	uint32_t number = value % 10000;
	snprintf(pairing->code, sizeof pairing->code, "%04u", number);
	return true;
}

static bool label_valid(const char *label) {
	size_t length = strlen(label);
	if (length == 0 || length >= RAIJU_DEVICE_LABEL_MAX) {
		return false;
	}
	for (const unsigned char *cursor = (const unsigned char *)label; *cursor != '\0'; cursor++) {
		unsigned char value = *cursor;
		if (value < 0x20 || value == 0x7f || value == '"' || value == '\\') {
			return false;
		}
	}
	return true;
}

static void device_from_token(raiju_pairing *pairing, const char *label, raiju_device *device) {
	unsigned char raw[RAIJU_PAIR_TOKEN_BYTES];
	unsigned char id_bytes[8];
	pairing->platform->random_bytes(pairing->platform->ctx, raw, sizeof raw);
	pairing->platform->random_bytes(pairing->platform->ctx, id_bytes, sizeof id_bytes);
	raiju_hex_encode(raw, sizeof raw, device->token, sizeof device->token);
	char id_hex[RAIJU_DEVICE_ID_MAX];
	raiju_hex_encode(id_bytes, 4, id_hex, sizeof id_hex);
	snprintf(device->id, sizeof device->id, "%s", id_hex);
	if (label == NULL || label[0] == '\0') {
		snprintf(device->label, sizeof device->label, "device");
	} else {
		snprintf(device->label, sizeof device->label, "%s", label);
	}
}

static bool parse_store(raiju_pairing *pairing, const char *json) {
	const char *cursor = json;
	pairing->device_count = 0;
	const char *key = strstr(cursor, "\"devices\"");
	if (key == NULL) {
		return false;
	}
	const char *array = strchr(key, '[');
	if (array == NULL) {
		return false;
	}
	cursor = array + 1;
	while (*cursor != '\0' && *cursor != ']') {
		if (*cursor == '{') {
			if (pairing->device_count >= RAIJU_DEVICES_MAX) {
				return false;
			}
			raiju_device *device = &pairing->devices[pairing->device_count];
			memset(device, 0, sizeof *device);
			const char *end = strchr(cursor, '}');
			if (end == NULL) {
				return false;
			}
			size_t span = (size_t)(end - cursor);
			char object[512];
			if (span >= sizeof object) {
				return false;
			}
			memcpy(object, cursor, span);
			object[span] = '\0';
			const char *id = strstr(object, "\"id\":\"");
			const char *token = strstr(object, "\"token\":\"");
			if (id == NULL || token == NULL) {
				return false;
			}
			id += strlen("\"id\":\"");
			token += strlen("\"token\":\"");
			const char *id_end = strchr(id, '"');
			const char *token_end = strchr(token, '"');
			if (id_end == NULL || token_end == NULL) {
				return false;
			}
			size_t id_len = (size_t)(id_end - id);
			size_t token_len = (size_t)(token_end - token);
			if (id_len == 0 || id_len >= RAIJU_DEVICE_ID_MAX || token_len != RAIJU_PAIR_TOKEN_HEX ||
			    !valid_hex(token, token_len)) {
				return false;
			}
			memcpy(device->id, id, id_len);
			device->id[id_len] = '\0';
			memcpy(device->token, token, token_len);
			device->token[token_len] = '\0';
			const char *label = strstr(object, "\"label\":\"");
			if (label != NULL) {
				label += strlen("\"label\":\"");
				const char *label_end = strchr(label, '"');
				if (label_end != NULL) {
					size_t label_len = (size_t)(label_end - label);
					if (label_len >= RAIJU_DEVICE_LABEL_MAX) {
						return false;
					}
					memcpy(device->label, label, label_len);
					device->label[label_len] = '\0';
				}
			}
			pairing->device_count++;
			cursor = end + 1;
		} else {
			cursor++;
		}
	}
	return true;
}

bool raiju_pairing_init(raiju_pairing *pairing, const raiju_platform *platform, const char *devices_path) {
	memset(pairing, 0, sizeof *pairing);
	pairing->platform = platform;
	if (snprintf(pairing->devices_path, sizeof pairing->devices_path, "%s", devices_path) >=
	    (int)sizeof pairing->devices_path) {
		return false;
	}
	size_t length = 0;
	char *existing = raiju_read_file(devices_path, DEVICES_FILE_MAX, &length);
	if (existing != NULL) {
		parse_store(pairing, existing);
		free(existing);
	}
	return raiju_pairing_new_code(pairing);
}

raiju_pair_status raiju_pairing_verify(raiju_pairing *pairing, const char *code, uint64_t now) {
	if (now < pairing->lock_until) {
		return RAIJU_PAIR_RATE_LIMITED;
	}
	if (code != NULL && constant_time_equal(code, pairing->code)) {
		pairing->failed_attempts = 0;
		pairing->lock_until = 0;
		return RAIJU_PAIR_OK;
	}
	pairing->failed_attempts++;
	if (pairing->failed_attempts > RAIJU_PAIR_FREE_ATTEMPTS) {
		unsigned steps = pairing->failed_attempts - RAIJU_PAIR_FREE_ATTEMPTS - 1;
		uint64_t delay = RAIJU_PAIR_BASE_LOCK_SECONDS;
		for (unsigned i = 0; i < steps && delay < RAIJU_PAIR_MAX_LOCK_SECONDS; i++) {
			delay *= 2;
		}
		if (delay > RAIJU_PAIR_MAX_LOCK_SECONDS) {
			delay = RAIJU_PAIR_MAX_LOCK_SECONDS;
		}
		pairing->lock_until = now + delay;
	}
	return RAIJU_PAIR_WRONG_CODE;
}

raiju_pair_status raiju_pairing_issue(raiju_pairing *pairing, const char *label, raiju_device *out) {
	if (pairing->device_count >= RAIJU_DEVICES_MAX) {
		return RAIJU_PAIR_FULL;
	}
	if (label != NULL && label[0] != '\0' && !label_valid(label)) {
		return RAIJU_PAIR_INVALID_LABEL;
	}
	raiju_device device;
	memset(&device, 0, sizeof device);
	device_from_token(pairing, label, &device);
	device.paired_at = 0;
	pairing->devices[pairing->device_count++] = device;
	if (!raiju_pairing_save(pairing)) {
		pairing->device_count--;
		return RAIJU_PAIR_STORE_ERROR;
	}
	if (out != NULL) {
		*out = device;
	}
	return RAIJU_PAIR_OK;
}

const raiju_device *raiju_pairing_find(const raiju_pairing *pairing, const char *token) {
	if (token == NULL) {
		return NULL;
	}
	const raiju_device *found = NULL;
	for (size_t i = 0; i < pairing->device_count; i++) {
		if (constant_time_equal(pairing->devices[i].token, token)) {
			found = &pairing->devices[i];
		}
	}
	return found;
}

raiju_pair_status raiju_pairing_revoke(raiju_pairing *pairing, const char *id) {
	for (size_t i = 0; i < pairing->device_count; i++) {
		if (strcmp(pairing->devices[i].id, id) == 0) {
			for (size_t j = i + 1; j < pairing->device_count; j++) {
				pairing->devices[j - 1] = pairing->devices[j];
			}
			pairing->device_count--;
			return raiju_pairing_save(pairing) ? RAIJU_PAIR_OK : RAIJU_PAIR_STORE_ERROR;
		}
	}
	return RAIJU_PAIR_NOT_FOUND;
}

bool raiju_pairing_save(const raiju_pairing *pairing) {
	raiju_strbuf buffer;
	raiju_strbuf_init(&buffer);
	raiju_strbuf_append(&buffer, "{\"version\":1,\"devices\":[");
	for (size_t i = 0; i < pairing->device_count; i++) {
		const raiju_device *device = &pairing->devices[i];
		if (i > 0) {
			raiju_strbuf_append_char(&buffer, ',');
		}
		raiju_strbuf_append(&buffer, "{\"id\":");
		raiju_strbuf_append_json_string(&buffer, device->id);
		raiju_strbuf_append(&buffer, ",\"token\":");
		raiju_strbuf_append_json_string(&buffer, device->token);
		raiju_strbuf_append(&buffer, ",\"label\":");
		raiju_strbuf_append_json_string(&buffer, device->label);
		raiju_strbuf_append_char(&buffer, '}');
	}
	raiju_strbuf_append(&buffer, "]}");
	if (buffer.failed) {
		raiju_strbuf_free(&buffer);
		return false;
	}
	bool ok = raiju_atomic_write(pairing->devices_path, raiju_strbuf_cstr(&buffer), buffer.length);
	raiju_strbuf_free(&buffer);
	return ok;
}

const char *raiju_pair_status_name(raiju_pair_status status) {
	switch (status) {
	case RAIJU_PAIR_OK:
		return "ok";
	case RAIJU_PAIR_WRONG_CODE:
		return "wrong_code";
	case RAIJU_PAIR_RATE_LIMITED:
		return "rate_limited";
	case RAIJU_PAIR_STORE_ERROR:
		return "store_error";
	case RAIJU_PAIR_FULL:
		return "full";
	case RAIJU_PAIR_NOT_FOUND:
		return "not_found";
	case RAIJU_PAIR_INVALID_LABEL:
		return "invalid_label";
	}
	return "unknown";
}
