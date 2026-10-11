#ifndef RAIJU_ALLOWLIST_H
#define RAIJU_ALLOWLIST_H

#include <stdbool.h>
#include <stddef.h>

#define RAIJU_ALLOWLIST_MAX_ROOTS 8

typedef enum {
	RAIJU_ALLOW_OK = 0,
	RAIJU_ALLOW_NOT_ABSOLUTE,
	RAIJU_ALLOW_HAS_DOTDOT,
	RAIJU_ALLOW_RESOLVE_FAILED,
	RAIJU_ALLOW_OUTSIDE_ROOTS
} raiju_allow_status;

/**
 * The set of directory roots a client path may resolve inside. Roots are
 * stored as given; checking resolves both sides through realpath.
 */
typedef struct {
	const char *roots[RAIJU_ALLOWLIST_MAX_ROOTS];
	size_t count;
} raiju_allowlist;

/**
 * Builds an allowlist from the ARCHITECTURE.md Security roots.
 */
raiju_allowlist raiju_allowlist_default(void);

/**
 * Appends a root. Returns false when the table is full.
 */
bool raiju_allowlist_add(raiju_allowlist *list, const char *root);

/**
 * Checks that path is absolute, free of ".." components, resolves through
 * realpath, and lands inside one of the list's roots (also resolved). On
 * success, and when resolved is not NULL, it receives the resolved absolute
 * path. Paths that do not exist fail with RAIJU_ALLOW_RESOLVE_FAILED.
 */
raiju_allow_status raiju_allowlist_check(const raiju_allowlist *list, const char *path, char *resolved,
                                         size_t resolved_capacity);

/**
 * Returns a stable lowercase name for an allow status.
 */
const char *raiju_allow_status_name(raiju_allow_status status);

#endif
