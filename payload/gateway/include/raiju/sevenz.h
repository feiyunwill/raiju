#ifndef RAIJU_SEVENZ_H
#define RAIJU_SEVENZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAIJU_SEVENZ_MAX_CODERS 4
#define RAIJU_SEVENZ_CODEC_ID_MAX 8
#define RAIJU_SEVENZ_PROPERTIES_MAX 16

/**
 * One coder of a 7z folder: its codec id, its property bytes and the packed
 * and unpacked sizes the header declares.
 */
typedef struct {
	uint64_t codec;
	unsigned char properties[RAIJU_SEVENZ_PROPERTIES_MAX];
	size_t properties_length;
} raiju_sevenz_coder;

/**
 * The folder an encoded 7z header is packed into: its coder chain and the
 * folder's unpacked size.
 */
typedef struct {
	raiju_sevenz_coder coders[RAIJU_SEVENZ_MAX_CODERS];
	size_t coder_count;
	uint64_t unpacked_size;
} raiju_sevenz_folder;

/**
 * Whether this build can decode an encoded 7z header, which needs liblzma.
 */
bool raiju_sevenz_decoding_available(void);

/**
 * Decodes one packed stream into out, following the folder's coder chain in
 * the order the format stores them. Returns false when a codec is not
 * supported, the stream does not decode, or the result does not fit, so the
 * caller reports the size as unknown instead of guessing.
 */
bool raiju_sevenz_decode(const raiju_sevenz_folder *folder, const unsigned char *packed, size_t packed_length,
                         unsigned char *out, size_t out_capacity, size_t *out_length);

#endif
