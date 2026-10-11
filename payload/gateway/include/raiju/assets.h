#ifndef RAIJU_ASSETS_H
#define RAIJU_ASSETS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	const char *path;
	const unsigned char *bytes;
	size_t length;
} raiju_asset;

/**
 * The generated embedded asset table and its length. When the embed
 * directory was absent at build time the table is empty and lookups fail.
 */
extern const raiju_asset raiju_embedded_assets[];
extern const size_t raiju_embedded_asset_count;

/**
 * Looks a path up in the table. A leading '/' is optional and a lookup for
 * "/" resolves to "index.html". Returns NULL when there is no entry.
 */
const raiju_asset *raiju_assets_find(const char *path);

/**
 * Returns a MIME type for a path's extension, defaulting to
 * application/octet-stream.
 */
const char *raiju_assets_mime(const char *path);

#endif
