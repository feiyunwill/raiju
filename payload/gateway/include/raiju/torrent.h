#ifndef RAIJU_TORRENT_H
#define RAIJU_TORRENT_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Reads the name a torrent's own info dictionary carries, from the base64
 * contents a client uploads to POST /api/jobs. Returns false when the text is
 * not valid base64, when the bencoded structure is not a dictionary holding an
 * "info" dictionary with a non-empty "name" string, or when that name does not
 * fit out_name (including its terminator). The name is copied as the bytes the
 * torrent carries, so the caller still decides whether it may name a job.
 */
bool raiju_torrent_name_from_base64(const char *text, size_t length, char *out_name, size_t capacity);

#endif
