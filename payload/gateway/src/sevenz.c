#include "raiju/sevenz.h"

#include <stdlib.h>
#include <string.h>

#ifdef RAIJU_HAVE_LZMA
#include <lzma.h>
#endif

#define SEVENZ_CODEC_COPY 0x00ull
#define SEVENZ_CODEC_DELTA 0x03ull
#define SEVENZ_CODEC_LZMA1 0x030101ull
#define SEVENZ_CODEC_LZMA2 0x21ull
#define SEVENZ_CODEC_BCJ_X86 0x03030103ull
#define SEVENZ_CODEC_BCJ_POWERPC 0x03030104ull
#define SEVENZ_CODEC_BCJ_IA64 0x03030105ull
#define SEVENZ_CODEC_BCJ_ARM 0x03030106ull
#define SEVENZ_CODEC_BCJ_ARMTHUMB 0x03030107ull
#define SEVENZ_CODEC_BCJ_SPARC 0x03030108ull

#ifdef RAIJU_HAVE_LZMA
/**
 * Maps a 7z codec id to the liblzma filter that decodes it. BCJ2 needs four
 * input streams and has no single-filter equivalent, so it is unsupported.
 */
static bool filter_for_codec(uint64_t codec, lzma_vli *out) {
	switch (codec) {
	case SEVENZ_CODEC_LZMA1:
		*out = LZMA_FILTER_LZMA1;
		return true;
	case SEVENZ_CODEC_LZMA2:
		*out = LZMA_FILTER_LZMA2;
		return true;
	case SEVENZ_CODEC_DELTA:
		*out = LZMA_FILTER_DELTA;
		return true;
	case SEVENZ_CODEC_BCJ_X86:
		*out = LZMA_FILTER_X86;
		return true;
	case SEVENZ_CODEC_BCJ_POWERPC:
		*out = LZMA_FILTER_POWERPC;
		return true;
	case SEVENZ_CODEC_BCJ_IA64:
		*out = LZMA_FILTER_IA64;
		return true;
	case SEVENZ_CODEC_BCJ_ARM:
		*out = LZMA_FILTER_ARM;
		return true;
	case SEVENZ_CODEC_BCJ_ARMTHUMB:
		*out = LZMA_FILTER_ARMTHUMB;
		return true;
	case SEVENZ_CODEC_BCJ_SPARC:
		*out = LZMA_FILTER_SPARC;
		return true;
	default:
		return false;
	}
}

/**
 * Releases the options liblzma allocated for the filters built so far.
 */
static void free_filter_options(lzma_filter *filters, size_t count) {
	for (size_t i = 0; i < count; i++) {
		free(filters[i].options);
		filters[i].options = NULL;
	}
}

/**
 * Builds the liblzma filter chain for a folder. 7z stores a folder's coders in
 * encoding order, so the decoder chain runs from the last coder back to the
 * first, which is the order liblzma expects. A 7z LZMA1 stream carries no
 * end-of-payload marker, so it is decoded as LZMA1EXT with the folder's
 * unpacked size when liblzma is new enough to take it.
 */
static bool build_filters(const raiju_sevenz_folder *folder, lzma_filter *filters) {
	for (size_t i = 0; i < folder->coder_count; i++) {
		const raiju_sevenz_coder *coder = &folder->coders[folder->coder_count - 1 - i];
		lzma_vli id = LZMA_VLI_UNKNOWN;
		if (!filter_for_codec(coder->codec, &id)) {
			free_filter_options(filters, i);
			return false;
		}
		filters[i].id = id;
		filters[i].options = NULL;
		if (coder->properties_length > 0 &&
		    lzma_properties_decode(&filters[i], NULL, coder->properties, coder->properties_length) != LZMA_OK) {
			free_filter_options(filters, i);
			return false;
		}
#if LZMA_VERSION >= 50040000
		if (id == LZMA_FILTER_LZMA1) {
			lzma_options_lzma *options = filters[i].options;
			if (options == NULL) {
				free_filter_options(filters, i + 1);
				return false;
			}
			options->ext_flags = LZMA_LZMA1EXT_ALLOW_EOPM;
			options->ext_size_low = (uint32_t)(folder->unpacked_size & 0xFFFFFFFFu);
			options->ext_size_high = (uint32_t)(folder->unpacked_size >> 32);
			filters[i].id = LZMA_FILTER_LZMA1EXT;
		}
#endif
	}
	filters[folder->coder_count].id = LZMA_VLI_UNKNOWN;
	filters[folder->coder_count].options = NULL;
	return true;
}
#endif

bool raiju_sevenz_decoding_available(void) {
#ifdef RAIJU_HAVE_LZMA
	return true;
#else
	return false;
#endif
}

bool raiju_sevenz_decode(const raiju_sevenz_folder *folder, const unsigned char *packed, size_t packed_length,
                         unsigned char *out, size_t out_capacity, size_t *out_length) {
	if (folder == NULL || out_length == NULL || folder->coder_count == 0 ||
	    folder->unpacked_size > (uint64_t)out_capacity) {
		return false;
	}
	if (folder->coder_count == 1 && folder->coders[0].codec == SEVENZ_CODEC_COPY) {
		if (folder->unpacked_size != (uint64_t)packed_length) {
			return false;
		}
		memcpy(out, packed, packed_length);
		*out_length = packed_length;
		return true;
	}
#ifdef RAIJU_HAVE_LZMA
	lzma_filter filters[RAIJU_SEVENZ_MAX_CODERS + 1];
	if (!build_filters(folder, filters)) {
		return false;
	}
	size_t input_position = 0;
	size_t output_position = 0;
	lzma_ret result = lzma_raw_buffer_decode(filters, NULL, packed, &input_position, packed_length, out,
	                                         &output_position, out_capacity);
	free_filter_options(filters, folder->coder_count);
	if (result != LZMA_OK || output_position != (size_t)folder->unpacked_size) {
		return false;
	}
	*out_length = output_position;
	return true;
#else
	(void)packed;
	(void)packed_length;
	(void)out;
	(void)out_capacity;
	return false;
#endif
}
