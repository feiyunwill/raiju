#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/fsutil.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_all(int fd, const void *bytes, size_t length) {
	const char *data = bytes;
	while (length > 0) {
		ssize_t written = write(fd, data, length);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			return -1;
		}
		data += written;
		length -= (size_t)written;
	}
	return 0;
}

static int sync_parent(const char *path) {
	char directory[RAIJU_PATH_MAX];
	const char *slash = strrchr(path, '/');
	if (slash == NULL) {
		snprintf(directory, sizeof directory, ".");
	} else if (slash == path) {
		snprintf(directory, sizeof directory, "/");
	} else {
		size_t length = (size_t)(slash - path);
		if (length >= sizeof directory) {
			return -1;
		}
		memcpy(directory, path, length);
		directory[length] = '\0';
	}
	int fd = open(directory, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	int status = fsync(fd);
	close(fd);
	return status;
}

bool raiju_atomic_write(const char *path, const void *bytes, size_t length) {
	if (path == NULL) {
		return false;
	}
	char temporary[RAIJU_PATH_MAX + 8];
	if ((size_t)snprintf(temporary, sizeof temporary, "%s.tmp", path) >= sizeof temporary) {
		return false;
	}
	int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		return false;
	}
	int status = write_all(fd, bytes, length);
	if (fsync(fd) != 0) {
		status = -1;
	}
	if (close(fd) != 0) {
		status = -1;
	}
	if (status != 0 || rename(temporary, path) != 0) {
		unlink(temporary);
		return false;
	}
	return sync_parent(path) == 0;
}

char *raiju_read_file(const char *path, size_t max_bytes, size_t *out_length) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return NULL;
	}
	struct stat info;
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || (uint64_t)info.st_size > max_bytes) {
		close(fd);
		return NULL;
	}
	size_t capacity = (size_t)info.st_size;
	char *buffer = malloc(capacity + 1);
	if (buffer == NULL) {
		close(fd);
		return NULL;
	}
	size_t length = 0;
	while (length < capacity) {
		ssize_t got = read(fd, buffer + length, capacity - length);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			free(buffer);
			close(fd);
			return NULL;
		}
		length += (size_t)got;
	}
	close(fd);
	buffer[length] = '\0';
	if (out_length != NULL) {
		*out_length = length;
	}
	return buffer;
}

bool raiju_mkdir_p(const char *path) {
	if (path == NULL || path[0] == '\0') {
		return false;
	}
	char buffer[RAIJU_PATH_MAX];
	if ((size_t)snprintf(buffer, sizeof buffer, "%s", path) >= sizeof buffer) {
		return false;
	}
	for (char *cursor = buffer + 1; *cursor != '\0'; cursor++) {
		if (*cursor != '/') {
			continue;
		}
		*cursor = '\0';
		if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
			return false;
		}
		*cursor = '/';
	}
	if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
		return false;
	}
	return true;
}

void raiju_hex_encode(const unsigned char *bytes, size_t length, char *buffer, size_t capacity) {
	static const char digits[] = "0123456789abcdef";
	size_t index = 0;
	for (size_t i = 0; i < length && index + 3 <= capacity; i++) {
		buffer[index++] = digits[(bytes[i] >> 4) & 0xf];
		buffer[index++] = digits[bytes[i] & 0xf];
	}
	if (capacity > 0) {
		buffer[index < capacity ? index : capacity - 1] = '\0';
	}
}
