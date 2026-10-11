#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "fixtures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void fixture_put_le32(uint8_t *p, uint32_t value) {
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

void fixture_put_le64(uint8_t *p, uint64_t value) {
	fixture_put_le32(p, (uint32_t)value);
	fixture_put_le32(p + 4, (uint32_t)(value >> 32));
}

void fixture_exfat_reseal(uint8_t *image, unsigned sector_shift) {
	size_t sector_size = (size_t)1 << sector_shift;
	size_t covered = 11 * sector_size;
	uint32_t checksum = 0;
	for (size_t i = 0; i < covered; i++) {
		if (i == 106 || i == 107 || i == 112) {
			continue;
		}
		checksum = ((checksum & 1u) << 31) + (checksum >> 1) + image[i];
	}
	for (size_t i = 0; i < sector_size; i += 4) {
		fixture_put_le32(image + covered + i, checksum);
	}
}

uint8_t *fixture_exfat(size_t image_len, unsigned sector_shift, unsigned cluster_shift, uint64_t volume_sectors) {
	size_t sector_size = (size_t)1 << sector_shift;
	if (image_len < FIXTURE_EXFAT_BOOT_SECTORS * sector_size) {
		return NULL;
	}
	uint8_t *image = calloc(1, image_len);
	if (image == NULL) {
		return NULL;
	}
	image[0] = 0xEB;
	image[1] = 0x76;
	image[2] = 0x90;
	memcpy(image + 3, "EXFAT   ", 8);
	fixture_put_le64(image + 72, volume_sectors);
	fixture_put_le32(image + 80, 24);
	fixture_put_le32(image + 84, 8);
	fixture_put_le32(image + 88, 32);
	fixture_put_le32(image + 92, 16);
	fixture_put_le32(image + 96, 4);
	fixture_put_le32(image + 100, 0x1234ABCDu);
	image[104] = 0x00;
	image[105] = 0x01;
	image[108] = (uint8_t)sector_shift;
	image[109] = (uint8_t)cluster_shift;
	image[110] = 1;
	image[111] = 0x80;
	image[510] = 0x55;
	image[511] = 0xAA;
	for (size_t sector = 1; sector <= 8; sector++) {
		image[sector * sector_size + sector_size - 2] = 0x55;
		image[sector * sector_size + sector_size - 1] = 0xAA;
	}
	fixture_exfat_reseal(image, sector_shift);
	return image;
}

uint8_t *fixture_ufs2(size_t image_len, uint32_t block_size, uint32_t fragment_size, uint64_t fragments) {
	if (image_len < 65536 + 1376) {
		return NULL;
	}
	uint8_t *image = calloc(1, image_len);
	if (image == NULL) {
		return NULL;
	}
	fixture_put_ufs2_superblock(image, 65536, 65536, block_size, fragment_size, fragments);
	return image;
}

void fixture_put_ufs2_superblock(uint8_t *image, size_t location, uint64_t sblockloc, uint32_t block_size,
                                 uint32_t fragment_size, uint64_t fragments) {
	uint8_t *superblock = image + location;
	fixture_put_le32(superblock + 48, block_size);
	fixture_put_le32(superblock + 52, fragment_size);
	fixture_put_le32(superblock + 56, fragment_size ? block_size / fragment_size : 0);
	fixture_put_le64(superblock + 1000, sblockloc);
	fixture_put_le64(superblock + 1080, fragments);
	fixture_put_le32(superblock + 1372, 0x19540119u);
}

char *fixture_temp_dir(void) {
	const char *base = getenv("TMPDIR");
	if (base == NULL || base[0] == '\0') {
		base = "/tmp";
	}
	char *path = fixture_path(base, "raiju-test-XXXXXX");
	if (path != NULL && mkdtemp(path) == NULL) {
		free(path);
		return NULL;
	}
	return path;
}

char *fixture_path(const char *dir, const char *name) {
	size_t dir_len = strlen(dir);
	int needs_slash = dir_len > 0 && dir[dir_len - 1] != '/';
	size_t length = dir_len + (size_t)needs_slash + strlen(name) + 1;
	char *path = malloc(length);
	if (path != NULL) {
		snprintf(path, length, "%s%s%s", dir, needs_slash ? "/" : "", name);
	}
	return path;
}

int fixture_write(const char *path, const void *data, size_t length) {
	FILE *stream = fopen(path, "wb");
	if (stream == NULL) {
		return -1;
	}
	size_t written = length ? fwrite(data, 1, length, stream) : 0;
	int closed = fclose(stream);
	return (written == length && closed == 0) ? 0 : -1;
}

int fixture_mkdir(const char *path) {
	return mkdir(path, 0700);
}

void fixture_remove(const char *path) {
	if (unlink(path) != 0) {
		rmdir(path);
	}
}
