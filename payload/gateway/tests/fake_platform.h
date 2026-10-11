#ifndef RAIJU_FAKE_PLATFORM_H
#define RAIJU_FAKE_PLATFORM_H

#include <stddef.h>

#include "raiju/platform.h"

/**
 * A deterministic platform for host tests: a counter RNG, a captured
 * notification, fixed disk usage, a fixed interface address and a recorded
 * engine launch.
 */
typedef struct {
	raiju_platform base;
	unsigned counter;
	char notification[256];
	char launched_executable[128];
	char launched_options[512];
	int launch_count;
	uint64_t total_bytes;
	uint64_t free_bytes;
	char firmware[32];
} raiju_fake_platform;

/**
 * Initialises a fake platform and returns its base interface.
 */
const raiju_platform *raiju_fake_platform_init(raiju_fake_platform *fake);

/**
 * Creates a fresh temporary directory under build/test-tmp and writes its
 * path into out. Returns false on failure.
 */
int raiju_test_tmpdir(char *out, size_t capacity, const char *name);

/**
 * Recursively removes a directory tree the tests created. Symlinks are
 * removed, never followed, so a link planted by a test cannot reach outside
 * the tree.
 */
void raiju_test_rmtree(const char *path);

#endif
