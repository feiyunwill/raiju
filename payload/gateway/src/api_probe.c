#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_internal.h"
#include "raiju/classify.h"
#include "raiju/platform.h"
#include "raiju/space.h"

#define PROBE_HEAD_MAX (4u * 1024u * 1024u)
#define PROBE_INDEX_MAX (4u * 1024u * 1024u)
#define PROBE_FOLDERS_MAX 4096u
#define ZIP_EOCD_WINDOW 65558u
#define SEVENZ_MAGIC 0xAFBC7A37u
#define ZIP_EOCD_SIGNATURE 0x06054b50u
#define ZIP_ZIP64_LOCATOR 0x07064b50u
#define ZIP_ZIP64_EOCD 0x06064b50u
#define ZIP_CENTRAL_SIGNATURE 0x02014b50u

static uint16_t read16(const unsigned char *bytes) {
	return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t read32(const unsigned char *bytes) {
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read64(const unsigned char *bytes) {
	return (uint64_t)read32(bytes) | ((uint64_t)read32(bytes + 4) << 32);
}

/**
 * Reads a range of the remote file through the fetch seam. Returns a newly
 * allocated NUL-terminated buffer the caller frees, or NULL.
 */
static char *range_slice(raiju_gateway *gateway, const char *url, uint64_t start, uint64_t end, size_t max_bytes,
                         size_t *out_length) {
	char range[64];
	snprintf(range, sizeof range, "bytes=%llu-%llu", (unsigned long long)start, (unsigned long long)end);
	raiju_fetch_fn fetch = raiju_api_fetch_fn(gateway);
	raiju_fetch_response response;
	if (fetch(raiju_api_fetch_context(gateway), RAIJU_FETCH_GET, url, range, max_bytes, RAIJU_FETCH_DEFAULT_TIMEOUT_MS,
	          &response) != 0) {
		return NULL;
	}
	if (response.status != 200 && response.status != 206) {
		raiju_fetch_response_free(&response);
		return NULL;
	}
	char *body = response.body;
	*out_length = response.body_length;
	response.body = NULL;
	raiju_fetch_response_free(&response);
	return body;
}

static bool zip64_directory_size(raiju_gateway *gateway, const char *url, uint64_t locator_offset, uint64_t *out_size,
                                 uint64_t *out_entries) {
	if (locator_offset < 20) {
		return false;
	}
	size_t length = 0;
	char *locator = range_slice(gateway, url, locator_offset - 20, locator_offset - 1, 64, &length);
	if (locator == NULL || length < 20 || read32((unsigned char *)locator) != ZIP_ZIP64_LOCATOR) {
		free(locator);
		return false;
	}
	uint64_t eocd_offset = read64((unsigned char *)locator + 8);
	free(locator);
	char *eocd = range_slice(gateway, url, eocd_offset, eocd_offset + 55, 128, &length);
	if (eocd == NULL || length < 56 || read32((unsigned char *)eocd) != ZIP_ZIP64_EOCD) {
		free(eocd);
		return false;
	}
	*out_entries = read64((unsigned char *)eocd + 32);
	*out_size = read64((unsigned char *)eocd + 40);
	free(eocd);
	return true;
}

/**
 * Sums the uncompressed sizes in a zip's central directory, read with range
 * requests, honouring zip64 when the 32-bit fields are saturated.
 */
static bool zip_uncompressed_size(raiju_gateway *gateway, const char *url, uint64_t total, uint64_t *out) {
	if (total < 22) {
		return false;
	}
	uint64_t tail = total < ZIP_EOCD_WINDOW ? total : ZIP_EOCD_WINDOW;
	size_t length = 0;
	char *window = range_slice(gateway, url, total - tail, total - 1, ZIP_EOCD_WINDOW + 64, &length);
	if (window == NULL) {
		return false;
	}
	const unsigned char *bytes = (const unsigned char *)window;
	long eocd = -1;
	if (length >= 22) {
		for (long i = (long)length - 22; i >= 0; i--) {
			if (read32(bytes + i) == ZIP_EOCD_SIGNATURE) {
				eocd = i;
				break;
			}
		}
	}
	if (eocd < 0) {
		free(window);
		return false;
	}
	uint64_t entries = read16(bytes + eocd + 10);
	uint64_t cd_size = read32(bytes + eocd + 12);
	uint64_t cd_offset = read32(bytes + eocd + 16);
	free(window);
	if (entries == 0xFFFF || cd_offset == 0xFFFFFFFF) {
		if (!zip64_directory_size(gateway, url, total - tail + (uint64_t)eocd, &cd_size, &entries)) {
			return false;
		}
	}
	if (cd_size == 0 || cd_size > PROBE_INDEX_MAX || entries == 0) {
		return false;
	}
	char *central = range_slice(gateway, url, cd_offset, cd_offset + cd_size - 1, PROBE_INDEX_MAX + 64, &length);
	if (central == NULL) {
		return false;
	}
	const unsigned char *cd = (const unsigned char *)central;
	uint64_t position = 0;
	uint64_t total_uncompressed = 0;
	uint64_t seen = 0;
	while (position + 46 <= length && seen < entries) {
		if (read32(cd + position) != ZIP_CENTRAL_SIGNATURE) {
			break;
		}
		uint64_t uncompressed = read32(cd + position + 24);
		uint16_t name_length = read16(cd + position + 28);
		uint16_t extra_length = read16(cd + position + 30);
		uint16_t comment_length = read16(cd + position + 32);
		uint64_t next = position + 46 + name_length + extra_length + comment_length;
		if (next > length) {
			free(central);
			return false;
		}
		if (uncompressed == 0xFFFFFFFF) {
			uint64_t extra = position + 46 + name_length;
			uint64_t extra_end = extra + extra_length;
			while (extra + 4 <= extra_end) {
				uint16_t id = read16(cd + extra);
				uint16_t size = read16(cd + extra + 2);
				if (extra + 4 + size > extra_end) {
					break;
				}
				if (id == 0x0001 && size >= 8) {
					uncompressed = read64(cd + extra + 4);
					break;
				}
				extra += 4 + size;
			}
		}
		total_uncompressed += uncompressed;
		position = next;
		seen++;
	}
	free(central);
	if (seen == 0) {
		return false;
	}
	*out = total_uncompressed;
	return true;
}

static bool read_number(const unsigned char *bytes, size_t length, size_t *position, uint64_t *out) {
	if (*position >= length) {
		return false;
	}
	unsigned char first = bytes[(*position)++];
	unsigned char mask = 0x80;
	uint64_t value = 0;
	for (int i = 0; i < 8; i++) {
		if ((first & mask) == 0) {
			value |= (uint64_t)(first & (mask - 1)) << (i * 8);
			*out = value;
			return true;
		}
		if (*position >= length) {
			return false;
		}
		value |= (uint64_t)bytes[(*position)++] << (i * 8);
		mask >>= 1;
	}
	*out = value;
	return true;
}

/**
 * Parses a 7z "StreamsInfo" far enough to sum each folder's unpacked size,
 * which is the archive's total uncompressed size. Returns false on any
 * malformed field.
 */
static bool sevenz_streams_size(const unsigned char *bytes, size_t length, size_t *position, uint64_t *out) {
	uint64_t id;
	if (!read_number(bytes, length, position, &id)) {
		return false;
	}
	if (id == 0x06) {
		uint64_t skip;
		uint64_t count;
		if (!read_number(bytes, length, position, &skip) || !read_number(bytes, length, position, &count)) {
			return false;
		}
		for (;;) {
			if (!read_number(bytes, length, position, &id)) {
				return false;
			}
			if (id == 0x00) {
				break;
			}
			if (id == 0x09) {
				for (uint64_t i = 0; i < count; i++) {
					if (!read_number(bytes, length, position, &skip)) {
						return false;
					}
				}
			} else if (id == 0x0A) {
				if (*position >= length) {
					return false;
				}
				unsigned char all_defined = bytes[(*position)++];
				if (!all_defined) {
					size_t bits = ((size_t)count + 7) / 8;
					if (*position + bits > length) {
						return false;
					}
					*position += bits;
				}
				if (*position + (size_t)count * 4 > length) {
					return false;
				}
				*position += (size_t)count * 4;
			} else {
				return false;
			}
		}
		if (!read_number(bytes, length, position, &id)) {
			return false;
		}
	}
	if (id != 0x07 || *position >= length) {
		return false;
	}
	uint64_t folder_marker;
	uint64_t folders;
	if (!read_number(bytes, length, position, &folder_marker) || folder_marker != 0x0B ||
	    !read_number(bytes, length, position, &folders) || *position >= length || folders == 0 ||
	    folders > PROBE_FOLDERS_MAX) {
		return false;
	}
	(*position)++;
	uint64_t *out_streams = calloc((size_t)folders, sizeof *out_streams);
	if (out_streams == NULL) {
		return false;
	}
	bool ok = true;
	for (uint64_t f = 0; ok && f < folders; f++) {
		uint64_t coders;
		if (!read_number(bytes, length, position, &coders) || coders == 0 || coders > 4) {
			ok = false;
			break;
		}
		uint64_t total_in = 0;
		uint64_t total_out = 0;
		for (uint64_t c = 0; ok && c < coders; c++) {
			if (*position >= length) {
				ok = false;
				break;
			}
			unsigned char flags = bytes[(*position)++];
			uint64_t in = 1;
			uint64_t out = 1;
			if (flags & 0x10) {
				if (!read_number(bytes, length, position, &in) || !read_number(bytes, length, position, &out)) {
					ok = false;
					break;
				}
			}
			if (*position + (flags & 0x0F) > length) {
				ok = false;
				break;
			}
			*position += flags & 0x0F;
			total_in += in;
			total_out += out;
		}
		if (!ok || total_out == 0) {
			ok = false;
			break;
		}
		uint64_t bind_pairs = total_out - 1;
		for (uint64_t i = 0; ok && i < bind_pairs; i++) {
			uint64_t index;
			if (!read_number(bytes, length, position, &index) || !read_number(bytes, length, position, &index)) {
				ok = false;
			}
		}
		uint64_t packed = total_in - bind_pairs;
		if (packed > 1) {
			for (uint64_t i = 0; ok && i < packed; i++) {
				uint64_t index;
				if (!read_number(bytes, length, position, &index)) {
					ok = false;
				}
			}
		}
		out_streams[f] = total_out;
	}
	uint64_t size_id;
	if (ok && (!read_number(bytes, length, position, &size_id) || size_id != 0x0C)) {
		ok = false;
	}
	uint64_t total_uncompressed = 0;
	for (uint64_t f = 0; ok && f < folders; f++) {
		uint64_t last = 0;
		for (uint64_t s = 0; s < out_streams[f]; s++) {
			if (!read_number(bytes, length, position, &last)) {
				ok = false;
				break;
			}
		}
		total_uncompressed += last;
	}
	free(out_streams);
	if (!ok) {
		return false;
	}
	*out = total_uncompressed;
	return true;
}

static bool sevenz_uncompressed_size(raiju_gateway *gateway, const char *url, uint64_t total, uint64_t *out) {
	if (total < 32) {
		return false;
	}
	size_t length = 0;
	char *start = range_slice(gateway, url, 0, 31, 64, &length);
	if (start == NULL || length < 32 || read32((unsigned char *)start) != SEVENZ_MAGIC) {
		free(start);
		return false;
	}
	uint64_t next_offset = read64((unsigned char *)start + 12);
	uint64_t next_size = read64((unsigned char *)start + 20);
	free(start);
	if (next_size == 0 || next_size > PROBE_INDEX_MAX) {
		return false;
	}
	uint64_t next_start = 32 + next_offset;
	char *header = range_slice(gateway, url, next_start, next_start + next_size - 1, PROBE_INDEX_MAX + 64, &length);
	if (header == NULL) {
		return false;
	}
	const unsigned char *bytes = (const unsigned char *)header;
	size_t position = 0;
	bool ok = length > 0 && bytes[position++] == 0x01;
	if (ok && position < length && bytes[position] == 0x02) {
		position++;
		uint64_t properties;
		ok = read_number(bytes, length, &position, &properties);
		for (uint64_t i = 0; ok && i < properties; i++) {
			uint64_t type;
			uint64_t size;
			if (!read_number(bytes, length, &position, &type) || !read_number(bytes, length, &position, &size) ||
			    position + size > length) {
				ok = false;
				break;
			}
			position += size;
		}
	}
	uint64_t total_uncompressed = 0;
	if (ok && position < length && bytes[position] == 0x04) {
		position++;
		ok = sevenz_streams_size(bytes, length, &position, &total_uncompressed);
	}
	free(header);
	if (!ok) {
		return false;
	}
	*out = total_uncompressed;
	return true;
}

static const char *uri_basename(const char *uri, char *out, size_t capacity) {
	const char *slash = strrchr(uri, '/');
	const char *name = slash != NULL ? slash + 1 : uri;
	size_t length = strcspn(name, "?#");
	if (length >= capacity) {
		length = capacity - 1;
	}
	memcpy(out, name, length);
	out[length] = '\0';
	return out;
}

static int probe_free_space(const char *path, uint64_t *free_bytes, void *context) {
	raiju_api *api = context;
	const raiju_platform *platform = api->gateway->config.platform;
	raiju_disk_usage usage;
	if (platform->disk_usage(platform->ctx, path, &usage) != 0) {
		return -1;
	}
	*free_bytes = usage.free_bytes;
	return 0;
}

/**
 * Runs one probe and builds its response. The caller holds probe_lock.
 */
static int probe_run(const raiju_http_request *request, raiju_http_response *response, raiju_api *api) {
	raiju_gateway *gateway = api->gateway;
	char uri[2048];
	if (!raiju_api_json_string((const char *)request->body, request->body_length, "uri", uri, sizeof uri)) {
		raiju_response_error(response, 400, "uri is required");
		return 400;
	}
	raiju_fetch_fn fetch = raiju_api_fetch_fn(gateway);
	raiju_fetch_response head;
	uint64_t size = 0;
	if (fetch(raiju_api_fetch_context(gateway), RAIJU_FETCH_HEAD, uri, NULL, 0, RAIJU_FETCH_DEFAULT_TIMEOUT_MS, &head) !=
	    0) {
		raiju_response_error(response, 502, "unreachable");
		return 502;
	}
	if (head.content_length != RAIJU_FETCH_NO_LENGTH) {
		size = head.content_length;
	}
	raiju_fetch_response_free(&head);
	if (size == 0) {
		raiju_fetch_response probe;
		if (fetch(raiju_api_fetch_context(gateway), RAIJU_FETCH_GET, uri, "bytes=0-0", 4096,
		          RAIJU_FETCH_DEFAULT_TIMEOUT_MS, &probe) == 0) {
			if (probe.content_length != RAIJU_FETCH_NO_LENGTH) {
				size = probe.content_length;
			}
			raiju_fetch_response_free(&probe);
		}
	}
	char name[RAIJU_JOB_NAME_MAX];
	uri_basename(uri, name, sizeof name);
	size_t head_length = 0;
	char *head_bytes = range_slice(gateway, uri, 0, RAIJU_PROBE_SIZE - 1, PROBE_HEAD_MAX, &head_length);
	raiju_classification classification;
	memset(&classification, 0, sizeof classification);
	if (head_bytes != NULL) {
		classification = raiju_classify((const uint8_t *)head_bytes, head_length, name);
	}
	uint64_t uncompressed = 0;
	bool known = false;
	if (head_bytes != NULL && classification.category == RAIJU_CATEGORY_ARCHIVE) {
		if (classification.kind == RAIJU_KIND_ZIP) {
			known = zip_uncompressed_size(gateway, uri, size, &uncompressed);
		} else if (classification.kind == RAIJU_KIND_7Z) {
			known = sevenz_uncompressed_size(gateway, uri, size, &uncompressed);
		}
	}
	free(head_bytes);
	if (!known) {
		uncompressed = 0;
	}
	raiju_space_need need = raiju_space_needed(classification.category, size, uncompressed);
	raiju_space_report space = raiju_space_check(need, gateway->install_root, probe_free_space, api);
	bool fits = space.status != RAIJU_SPACE_NOT_ENOUGH;
	const char *category = classification.category == RAIJU_CATEGORY_ARCHIVE ? "archive"
	                       : classification.category == RAIJU_CATEGORY_IMAGE ? "image"
	                                                                        : "other";
	raiju_strbuf document;
	raiju_strbuf_init(&document);
	raiju_strbuf_append(&document, "{\"size\":");
	raiju_strbuf_append_uint(&document, (unsigned long long)size);
	raiju_strbuf_append(&document, ",\"kind\":");
	raiju_strbuf_append_json_string(&document, raiju_kind_name(classification.kind));
	raiju_strbuf_append(&document, ",\"category\":");
	raiju_strbuf_append_json_string(&document, category);
	raiju_strbuf_append(&document, ",\"uncompressedSize\":");
	raiju_strbuf_append_uint(&document, (unsigned long long)uncompressed);
	raiju_strbuf_append(&document, ",\"uncompressedKnown\":");
	raiju_strbuf_append(&document, known ? "true" : "false");
	raiju_strbuf_append(&document, ",\"estimated\":");
	raiju_strbuf_append(&document, space.estimated ? "true" : "false");
	raiju_strbuf_append(&document, ",\"fits\":");
	raiju_strbuf_append(&document, fits ? "true" : "false");
	raiju_strbuf_append(&document, ",\"requiredBytes\":");
	raiju_strbuf_append_uint(&document, (unsigned long long)space.required_bytes);
	raiju_strbuf_append(&document, ",\"freeBytes\":");
	raiju_strbuf_append_uint(&document, (unsigned long long)space.free_bytes);
	raiju_strbuf_append_char(&document, '}');
	raiju_response_json(response, 200, raiju_strbuf_cstr(&document));
	raiju_strbuf_free(&document);
	return 200;
}

/**
 * Answers POST /api/probe, refusing with 503 probe_busy while another probe
 * is in flight so a client cannot make the console scan the LAN in parallel.
 */
static int handle_probe(const raiju_http_request *request, raiju_http_response *response, void *context) {
	raiju_api *api = context;
	if (pthread_mutex_trylock(&api->probe_lock) != 0) {
		raiju_response_error(response, 503, "probe_busy");
		return 503;
	}
	int status = probe_run(request, response, api);
	pthread_mutex_unlock(&api->probe_lock);
	return status;
}

bool raiju_api_add_probe_route(raiju_gateway *gateway, raiju_api *api) {
	raiju_route route = {"POST", "/api/probe", handle_probe, api, true};
	return raiju_gateway_add_route(gateway, &route);
}
