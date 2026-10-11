#include "raiju/auth.h"

#include <stdio.h>
#include <string.h>

const char *raiju_auth_cookie_name(void) {
	return "raiju_device";
}

static bool copy_token(const char *start, size_t length, char *out, size_t capacity) {
	if (length == 0 || length >= capacity) {
		return false;
	}
	memcpy(out, start, length);
	out[length] = '\0';
	return true;
}

static bool cookie_token(const char *cookie, char *out, size_t capacity) {
	const char *expected = raiju_auth_cookie_name();
	size_t expected_length = strlen(expected);
	const char *cursor = cookie;
	while (*cursor != '\0') {
		while (*cursor == ' ' || *cursor == ';') {
			cursor++;
		}
		const char *name = cursor;
		while (*cursor != '\0' && *cursor != '=' && *cursor != ';') {
			cursor++;
		}
		size_t name_length = (size_t)(cursor - name);
		if (*cursor == '=' && name_length == expected_length && strncmp(name, expected, expected_length) == 0) {
			cursor++;
			const char *value = cursor;
			while (*cursor != '\0' && *cursor != ';') {
				cursor++;
			}
			return copy_token(value, (size_t)(cursor - value), out, capacity);
		}
		while (*cursor != '\0' && *cursor != ';') {
			cursor++;
		}
	}
	return false;
}

bool raiju_auth_token(const raiju_http_request *request, char *out, size_t capacity) {
	if (request->cookie != NULL && cookie_token(request->cookie, out, capacity)) {
		return true;
	}
	if (request->authorization != NULL) {
		const char *value = request->authorization;
		const char *prefix = "Bearer ";
		size_t prefix_length = strlen(prefix);
		if (strncmp(value, prefix, prefix_length) == 0) {
			value += prefix_length;
		}
		while (*value == ' ') {
			value++;
		}
		size_t length = strlen(value);
		while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '\r' || value[length - 1] == '\n')) {
			length--;
		}
		return copy_token(value, length, out, capacity);
	}
	return false;
}

void raiju_auth_set_cookie(const char *token, char *out, size_t capacity) {
	snprintf(out, capacity, "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=31536000", raiju_auth_cookie_name(),
	         token);
}
