#define _POSIX_C_SOURCE 200809L

#include "raiju/engine_options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raiju/fsutil.h"
#include "raiju/strbuf.h"

static bool load_or_generate_secret(raiju_engine_config *config, const raiju_platform *platform,
                                    const char *secret_path) {
	size_t length = 0;
	char *existing = raiju_read_file(secret_path, 256, &length);
	if (existing != NULL) {
		size_t trimmed = 0;
		while (trimmed < length && existing[trimmed] != '\n' && existing[trimmed] != '\r') {
			trimmed++;
		}
		existing[trimmed] = '\0';
		if (trimmed == RAIJU_ENGINE_SECRET_HEX) {
			memcpy(config->secret, existing, RAIJU_ENGINE_SECRET_HEX + 1);
			free(existing);
			return true;
		}
		free(existing);
	}
	unsigned char raw[RAIJU_ENGINE_SECRET_BYTES];
	if (platform->random_bytes(platform->ctx, raw, sizeof raw) != 0) {
		return false;
	}
	raiju_hex_encode(raw, sizeof raw, config->secret, sizeof config->secret);
	return raiju_atomic_write(secret_path, config->secret, RAIJU_ENGINE_SECRET_HEX);
}

bool raiju_engine_config_init(raiju_engine_config *config, const raiju_platform *platform, const char *data_dir,
                              const char *secret_path) {
	memset(config, 0, sizeof *config);
	config->rpc_port = RAIJU_ENGINE_RPC_PORT;
	if (snprintf(config->downloads_dir, sizeof config->downloads_dir, "%s/downloads", data_dir) >=
	        (int)sizeof config->downloads_dir ||
	    snprintf(config->session_dir, sizeof config->session_dir, "%s/session", data_dir) >=
	        (int)sizeof config->session_dir) {
		return false;
	}
	return load_or_generate_secret(config, platform, secret_path);
}

char *raiju_engine_options_render(const raiju_engine_config *config) {
	raiju_strbuf buffer;
	raiju_strbuf_init(&buffer);
	raiju_strbuf_append(&buffer, "enable-rpc=true\n");
	raiju_strbuf_append(&buffer, "rpc-listen-all=false\n");
	raiju_strbuf_appendf(&buffer, "rpc-listen-port=%u\n", (unsigned)config->rpc_port);
	raiju_strbuf_appendf(&buffer, "rpc-secret=%s\n", config->secret);
	raiju_strbuf_appendf(&buffer, "dir=%s\n", config->downloads_dir);
	raiju_strbuf_appendf(&buffer, "save-session=%s/%s\n", config->session_dir, RAIJU_ENGINE_SESSION_FILE);
	raiju_strbuf_appendf(&buffer, "input-file=%s/%s\n", config->session_dir, RAIJU_ENGINE_SESSION_FILE);
	raiju_strbuf_append(&buffer, "save-session-interval=10\n");
	raiju_strbuf_append(&buffer, "force-save=true\n");
	raiju_strbuf_append(&buffer, "continue=true\n");
	raiju_strbuf_append(&buffer, "max-connection-per-server=16\n");
	raiju_strbuf_append(&buffer, "split=16\n");
	raiju_strbuf_append(&buffer, "min-split-size=10M\n");
	raiju_strbuf_append(&buffer, "max-concurrent-downloads=5\n");
	raiju_strbuf_append(&buffer, "disk-cache=64M\n");
	raiju_strbuf_append(&buffer, "file-allocation=none\n");
	raiju_strbuf_append(&buffer, "no-file-allocation-limit=64M\n");
	raiju_strbuf_append(&buffer, "enable-http-keep-alive=true\n");
	raiju_strbuf_append(&buffer, "optimize-concurrent-downloads=false\n");
	if (buffer.failed) {
		raiju_strbuf_free(&buffer);
		return NULL;
	}
	const char *text = raiju_strbuf_cstr(&buffer);
	char *result = malloc(buffer.length + 1);
	if (result != NULL) {
		memcpy(result, text, buffer.length + 1);
	}
	raiju_strbuf_free(&buffer);
	return result;
}

bool raiju_engine_options_write(const raiju_engine_config *config, const char *options_path) {
	char *rendered = raiju_engine_options_render(config);
	if (rendered == NULL) {
		return false;
	}
	bool ok = raiju_atomic_write(options_path, rendered, strlen(rendered));
	free(rendered);
	return ok;
}
