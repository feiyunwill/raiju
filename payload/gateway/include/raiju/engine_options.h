#ifndef RAIJU_ENGINE_OPTIONS_H
#define RAIJU_ENGINE_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raiju/fsutil.h"
#include "raiju/platform.h"

#define RAIJU_ENGINE_RPC_PORT 6800
#define RAIJU_ENGINE_SECRET_BYTES 24
#define RAIJU_ENGINE_SECRET_HEX (RAIJU_ENGINE_SECRET_BYTES * 2)
#define RAIJU_ENGINE_SESSION_FILE "aria2.session"
#define RAIJU_ENGINE_OPTIONS_FILE "aria2.conf"

/**
 * The inputs the engine's options file and RPC client are built from. The
 * paths are absolute and rooted at the data directory the supervisor made.
 */
typedef struct {
	char downloads_dir[RAIJU_PATH_MAX];
	char session_dir[RAIJU_PATH_MAX];
	char secret[RAIJU_ENGINE_SECRET_HEX + 1];
	uint16_t rpc_port;
} raiju_engine_config;

/**
 * Builds an engine configuration rooted at data_dir: the downloads and
 * session directories, the RPC port and the secret. The secret is loaded
 * from secret_path when it exists, otherwise a fresh one is generated from
 * the platform RNG hook and persisted atomically there. Returns false when a
 * path is too long, the RNG fails or the secret can't be written.
 */
bool raiju_engine_config_init(raiju_engine_config *config, const raiju_platform *platform, const char *data_dir,
                              const char *secret_path);

/**
 * Renders the aria2 options file (the ARCHITECTURE.md tuning table plus the
 * RPC, session and download settings) into a newly allocated NUL-terminated
 * buffer the caller frees. Returns NULL when out of memory.
 */
char *raiju_engine_options_render(const raiju_engine_config *config);

/**
 * Writes the rendered options to options_path atomically. Returns false on
 * any failure.
 */
bool raiju_engine_options_write(const raiju_engine_config *config, const char *options_path);

#endif
