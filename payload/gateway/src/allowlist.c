#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/allowlist.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *default_roots[] = {
    "/data/raiju",
    "/data/pkg",
    "/data/homebrew",
    "/data/etaHEN/games",
};

raiju_allowlist raiju_allowlist_default(void) {
	raiju_allowlist list;
	memset(&list, 0, sizeof list);
	for (size_t i = 0; i < sizeof default_roots / sizeof default_roots[0]; i++) {
		raiju_allowlist_add(&list, default_roots[i]);
	}
	return list;
}

bool raiju_allowlist_add(raiju_allowlist *list, const char *root) {
	if (list->count >= RAIJU_ALLOWLIST_MAX_ROOTS) {
		return false;
	}
	list->roots[list->count++] = root;
	return true;
}

static bool has_dotdot_component(const char *path) {
	const char *cursor = path;
	while (*cursor != '\0') {
		while (*cursor == '/') {
			cursor++;
		}
		const char *start = cursor;
		while (*cursor != '\0' && *cursor != '/') {
			cursor++;
		}
		size_t length = (size_t)(cursor - start);
		if (length == 2 && start[0] == '.' && start[1] == '.') {
			return true;
		}
	}
	return false;
}

static bool is_inside(const char *root, const char *resolved) {
	size_t length = strlen(root);
	if (strncmp(root, resolved, length) != 0) {
		return false;
	}
	return resolved[length] == '\0' || resolved[length] == '/';
}

raiju_allow_status raiju_allowlist_check(const raiju_allowlist *list, const char *path, char *resolved,
                                         size_t resolved_capacity) {
	if (path == NULL || path[0] != '/') {
		return RAIJU_ALLOW_NOT_ABSOLUTE;
	}
	if (has_dotdot_component(path)) {
		return RAIJU_ALLOW_HAS_DOTDOT;
	}
	char candidate[PATH_MAX];
	if (realpath(path, candidate) == NULL) {
		return RAIJU_ALLOW_RESOLVE_FAILED;
	}
	for (size_t i = 0; i < list->count; i++) {
		char root[PATH_MAX];
		if (realpath(list->roots[i], root) == NULL) {
			continue;
		}
		if (is_inside(root, candidate)) {
			if (resolved != NULL) {
				size_t length = strlen(candidate);
				if (length + 1 > resolved_capacity) {
					return RAIJU_ALLOW_RESOLVE_FAILED;
				}
				memcpy(resolved, candidate, length + 1);
			}
			return RAIJU_ALLOW_OK;
		}
	}
	return RAIJU_ALLOW_OUTSIDE_ROOTS;
}

const char *raiju_allow_status_name(raiju_allow_status status) {
	switch (status) {
	case RAIJU_ALLOW_OK:
		return "ok";
	case RAIJU_ALLOW_NOT_ABSOLUTE:
		return "not_absolute";
	case RAIJU_ALLOW_HAS_DOTDOT:
		return "has_dotdot";
	case RAIJU_ALLOW_RESOLVE_FAILED:
		return "resolve_failed";
	case RAIJU_ALLOW_OUTSIDE_ROOTS:
		return "outside_roots";
	}
	return "unknown";
}
