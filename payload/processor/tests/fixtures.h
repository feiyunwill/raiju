#ifndef RAIJU_FIXTURES_H
#define RAIJU_FIXTURES_H

#include <stddef.h>
#include <stdint.h>

#define FIXTURE_EXFAT_BOOT_SECTORS 12u

/**
 * Builds a zeroed buffer of image_len bytes holding a valid exFAT boot region
 * (main boot sector, empty extended sectors and a correct checksum sector).
 * The caller frees the buffer.
 */
uint8_t *fixture_exfat(size_t image_len, unsigned sector_shift, unsigned cluster_shift, uint64_t volume_sectors);

/**
 * Recomputes and rewrites the boot checksum sector of an exFAT fixture after
 * its boot region has been edited.
 */
void fixture_exfat_reseal(uint8_t *image, unsigned sector_shift);

/**
 * Builds a zeroed buffer of image_len bytes (at least the UFS2 probe size)
 * holding a UFS2 superblock at offset 65536. The caller frees the buffer.
 */
uint8_t *fixture_ufs2(size_t image_len, uint32_t block_size, uint32_t fragment_size, uint64_t fragments);

/**
 * Same as fixture_ufs2 but places the superblock at location and records
 * sblockloc in its fs_sblockloc field, so a backup copy can be simulated by
 * passing a sblockloc that differs from location.
 */
void fixture_put_ufs2_superblock(uint8_t *image, size_t location, uint64_t sblockloc, uint32_t block_size,
                                 uint32_t fragment_size, uint64_t fragments);

/**
 * Writes a little-endian 32-bit value at p.
 */
void fixture_put_le32(uint8_t *p, uint32_t value);

/**
 * Writes a little-endian 64-bit value at p.
 */
void fixture_put_le64(uint8_t *p, uint64_t value);

/**
 * Creates a fresh temporary directory and returns its path, or NULL. The
 * caller frees the string.
 */
char *fixture_temp_dir(void);

/**
 * Joins a directory and a relative name into a newly allocated path.
 */
char *fixture_path(const char *dir, const char *name);

/**
 * Writes bytes to a new file at path. Returns 0 on success.
 */
int fixture_write(const char *path, const void *data, size_t length);

/**
 * Creates a directory at path. Returns 0 on success.
 */
int fixture_mkdir(const char *path);

/**
 * Removes a file or an empty directory at path.
 */
void fixture_remove(const char *path);

#endif
