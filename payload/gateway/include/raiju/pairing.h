#ifndef RAIJU_PAIRING_H
#define RAIJU_PAIRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raiju/fsutil.h"
#include "raiju/platform.h"

#define RAIJU_PAIR_CODE_DIGITS 4
#define RAIJU_PAIR_TOKEN_BYTES 32
#define RAIJU_PAIR_TOKEN_HEX (RAIJU_PAIR_TOKEN_BYTES * 2)
#define RAIJU_DEVICE_ID_MAX 32
#define RAIJU_DEVICE_LABEL_MAX 64
#define RAIJU_DEVICES_MAX 64
#define RAIJU_PAIR_FREE_ATTEMPTS 3
#define RAIJU_PAIR_BASE_LOCK_SECONDS 2
#define RAIJU_PAIR_MAX_LOCK_SECONDS 300

typedef enum {
	RAIJU_PAIR_OK = 0,
	RAIJU_PAIR_WRONG_CODE,
	RAIJU_PAIR_RATE_LIMITED,
	RAIJU_PAIR_STORE_ERROR,
	RAIJU_PAIR_FULL,
	RAIJU_PAIR_NOT_FOUND,
	RAIJU_PAIR_INVALID_LABEL
} raiju_pair_status;

/**
 * One paired device: its stable ID, the bearer token it presents, a label
 * and the pairing time.
 */
typedef struct {
	char id[RAIJU_DEVICE_ID_MAX];
	char token[RAIJU_PAIR_TOKEN_HEX + 1];
	char label[RAIJU_DEVICE_LABEL_MAX];
	uint64_t paired_at;
} raiju_device;

/**
 * The pairing state: the current code, the failed-attempt counter and the
 * instant the next attempt is allowed, plus the device store and the path it
 * is persisted to.
 */
typedef struct {
	const raiju_platform *platform;
	char code[RAIJU_PAIR_CODE_DIGITS + 1];
	unsigned failed_attempts;
	uint64_t lock_until;
	raiju_device devices[RAIJU_DEVICES_MAX];
	size_t device_count;
	char devices_path[RAIJU_PATH_MAX];
} raiju_pairing;

/**
 * Prepares the pairing state from the store at devices_path, generating a
 * fresh code. A missing store starts empty.
 */
bool raiju_pairing_init(raiju_pairing *pairing, const raiju_platform *platform, const char *devices_path);

/**
 * Generates a new 4-digit code from the platform RNG hook.
 */
bool raiju_pairing_new_code(raiju_pairing *pairing);

/**
 * Compares code against the current code in constant time and enforces the
 * backoff. now is the current time in seconds. On a correct code the lockout
 * state is cleared; on a wrong one the attempt counter grows and, past the
 * free attempts, the next attempt is pushed out by an exponential backoff.
 */
raiju_pair_status raiju_pairing_verify(raiju_pairing *pairing, const char *code, uint64_t now);

/**
 * Issues a token for a newly paired device and persists the store. When
 * label is NULL or empty, "device" is used. A label that is too long or
 * carries quotes, backslashes or control characters is refused with
 * RAIJU_PAIR_INVALID_LABEL, because labels are echoed into JSON documents.
 */
raiju_pair_status raiju_pairing_issue(raiju_pairing *pairing, const char *label, raiju_device *out);

/**
 * Finds the device whose token matches, in constant time, or NULL.
 */
const raiju_device *raiju_pairing_find(const raiju_pairing *pairing, const char *token);

/**
 * Removes the device with the given ID and persists the store.
 */
raiju_pair_status raiju_pairing_revoke(raiju_pairing *pairing, const char *id);

/**
 * Persists the device store atomically.
 */
bool raiju_pairing_save(const raiju_pairing *pairing);

/**
 * Returns a stable lowercase name for a pair status.
 */
const char *raiju_pair_status_name(raiju_pair_status status);

#endif
