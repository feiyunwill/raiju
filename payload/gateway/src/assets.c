#include "raiju/assets.h"

#include <string.h>

static const char *extension_of(const char *path) {
	const char *dot = strrchr(path, '.');
	const char *slash = strrchr(path, '/');
	if (dot == NULL || (slash != NULL && dot < slash)) {
		return "";
	}
	return dot + 1;
}

static bool extension_is(const char *extension, const char *candidate) {
	return strcmp(extension, candidate) == 0;
}

const char *raiju_assets_mime(const char *path) {
	const char *extension = extension_of(path);
	if (extension_is(extension, "html")) {
		return "text/html; charset=utf-8";
	}
	if (extension_is(extension, "js") || extension_is(extension, "mjs")) {
		return "text/javascript; charset=utf-8";
	}
	if (extension_is(extension, "css")) {
		return "text/css; charset=utf-8";
	}
	if (extension_is(extension, "json")) {
		return "application/json";
	}
	if (extension_is(extension, "svg")) {
		return "image/svg+xml";
	}
	if (extension_is(extension, "png")) {
		return "image/png";
	}
	if (extension_is(extension, "jpg") || extension_is(extension, "jpeg")) {
		return "image/jpeg";
	}
	if (extension_is(extension, "webp")) {
		return "image/webp";
	}
	if (extension_is(extension, "woff2")) {
		return "font/woff2";
	}
	if (extension_is(extension, "woff")) {
		return "font/woff";
	}
	if (extension_is(extension, "ico")) {
		return "image/x-icon";
	}
	if (extension_is(extension, "txt")) {
		return "text/plain; charset=utf-8";
	}
	if (extension_is(extension, "map")) {
		return "application/json";
	}
	return "application/octet-stream";
}

const raiju_asset *raiju_assets_find(const char *path) {
	const char *lookup = path;
	if (lookup[0] == '/') {
		lookup++;
	}
	if (lookup[0] == '\0') {
		lookup = "index.html";
	}
	for (size_t i = 0; i < raiju_embedded_asset_count; i++) {
		if (strcmp(raiju_embedded_assets[i].path, lookup) == 0) {
			return &raiju_embedded_assets[i];
		}
	}
	return NULL;
}
