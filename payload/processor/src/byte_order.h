#ifndef RAIJU_BYTE_ORDER_H
#define RAIJU_BYTE_ORDER_H

#include <stdint.h>

/**
 * Reads an unaligned little-endian 16-bit value.
 */
static inline uint16_t raiju_read_le16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

/**
 * Reads an unaligned little-endian 32-bit value.
 */
static inline uint32_t raiju_read_le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Reads an unaligned little-endian 64-bit value.
 */
static inline uint64_t raiju_read_le64(const uint8_t *p) {
	return (uint64_t)raiju_read_le32(p) | ((uint64_t)raiju_read_le32(p + 4) << 32);
}

#endif
