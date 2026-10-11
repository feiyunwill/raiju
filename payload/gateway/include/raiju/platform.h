#ifndef RAIJU_PLATFORM_H
#define RAIJU_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/**
 * Free and total bytes of the filesystem holding a path, in bytes.
 */
typedef struct {
	uint64_t total_bytes;
	uint64_t free_bytes;
} raiju_disk_usage;

/**
 * The console- and host-specific calls the gateway needs. Every PS5-only
 * call lives here so host tests substitute fakes; the PS5 implementation is
 * added with the payload build.
 */
typedef struct {
	int (*notify)(void *ctx, const char *message);
	int (*escalate)(void *ctx);
	int (*disk_usage)(void *ctx, const char *path, raiju_disk_usage *out);
	int (*interface_ip)(void *ctx, char *buffer, size_t capacity);
	int (*launch_engine)(void *ctx, const char *executable, const char *options_file);
	int (*random_bytes)(void *ctx, void *buffer, size_t length);
	void *ctx;
} raiju_platform;

/**
 * The host implementation used by tests and by a desktop build: it writes
 * notifications to stderr, treats escalation as a no-op, reads disk usage
 * through statvfs, finds the first non-loopback IPv4 address, launches the
 * engine with fork/exec and reads randomness from /dev/urandom.
 */
const raiju_platform *raiju_platform_host(void);

#endif
