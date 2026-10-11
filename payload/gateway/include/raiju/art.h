#ifndef RAIJU_ART_H
#define RAIJU_ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAIJU_ART_TITLE_ID_LEN 9
#define RAIJU_ART_TTL_SECONDS 86400

/**
 * Artwork for one title, ported from ui/src/artwork/parse.ts. name, background
 * and logo are NULL when the page omits them; icon is always present on a
 * result. source is "orbispatches" for CUSA (PS4) and "prosperopatches" for
 * PPSA (PS5).
 */
typedef struct {
	char title_id[RAIJU_ART_TITLE_ID_LEN + 1];
	char *name;
	char *icon;
	char *background;
	char *logo;
	const char *source;
} raiju_art;

/**
 * Writes the OrbisPatches (CUSA) or ProsperoPatches (PPSA) page URL for a
 * title ID into out. Returns false when the ID is not (CUSA|PPSA)[0-9]{5}.
 */
bool raiju_art_page_url(const char *title_id, char *out, size_t capacity);

/**
 * Returns whether text is a well-formed artwork title ID:
 * (CUSA|PPSA)[0-9]{5}.
 */
bool raiju_art_title_id_valid(const char *text);

/**
 * Parses a title page into artwork, following parse.ts exactly: only images
 * under that title's own CDN folder are accepted, the first per kind wins,
 * icon0 is required (else NULL), HTML entities are decoded and the site
 * suffix is stripped from the name. Returns a newly allocated result the
 * caller frees with raiju_art_free, or NULL when the page has no icon or the
 * title ID is malformed.
 */
raiju_art *raiju_art_parse(const char *title_id, const char *html, size_t html_length);

/**
 * Serialises artwork as the /api/art JSON document into a newly allocated
 * NUL-terminated buffer the caller frees. Absent name, background and logo
 * become JSON null. Returns NULL on allocation failure.
 */
char *raiju_art_to_json(const raiju_art *art, size_t *out_length);

/**
 * Releases artwork and its strings.
 */
void raiju_art_free(raiju_art *art);

/**
 * Reads the cached JSON for a title under dir, honouring the 24h TTL by file
 * modification time. Returns a newly allocated NUL-terminated buffer the
 * caller frees, or NULL when there is no fresh entry.
 */
char *raiju_art_cache_read(const char *dir, const char *title_id, uint64_t now, size_t *out_length);

/**
 * Writes the JSON for a title under dir atomically, creating dir when
 * missing. Returns false on any failure.
 */
bool raiju_art_cache_write(const char *dir, const char *title_id, const char *json, size_t length);

#endif
