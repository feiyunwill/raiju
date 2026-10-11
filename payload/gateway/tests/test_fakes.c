#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "fake_platform.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fake_notify(void *ctx, const char *message) {
	raiju_fake_platform *fake = ctx;
	snprintf(fake->notification, sizeof fake->notification, "%s", message);
	return 0;
}

static int fake_escalate(void *ctx) {
	(void)ctx;
	return 0;
}

static int fake_disk_usage(void *ctx, const char *path, raiju_disk_usage *out) {
	(void)path;
	raiju_fake_platform *fake = ctx;
	out->total_bytes = fake->total_bytes;
	out->free_bytes = fake->free_bytes;
	return 0;
}

static int fake_interface_ip(void *ctx, char *buffer, size_t capacity) {
	(void)ctx;
	snprintf(buffer, capacity, "10.0.0.5");
	return 0;
}

static int fake_launch_engine(void *ctx, const char *executable, const char *options_file) {
	raiju_fake_platform *fake = ctx;
	snprintf(fake->launched_executable, sizeof fake->launched_executable, "%s", executable);
	snprintf(fake->launched_options, sizeof fake->launched_options, "%s", options_file);
	fake->launch_count++;
	return 0;
}

static int fake_random_bytes(void *ctx, void *buffer, size_t length) {
	raiju_fake_platform *fake = ctx;
	unsigned char *out = buffer;
	for (size_t i = 0; i < length; i++) {
		out[i] = (unsigned char)(fake->counter * 31 + i * 7 + 1);
	}
	fake->counter++;
	return 0;
}

const raiju_platform *raiju_fake_platform_init(raiju_fake_platform *fake) {
	memset(fake, 0, sizeof *fake);
	fake->total_bytes = 1000000000ull;
	fake->free_bytes = 500000000ull;
	fake->base.notify = fake_notify;
	fake->base.escalate = fake_escalate;
	fake->base.disk_usage = fake_disk_usage;
	fake->base.interface_ip = fake_interface_ip;
	fake->base.launch_engine = fake_launch_engine;
	fake->base.random_bytes = fake_random_bytes;
	fake->base.ctx = fake;
	return &fake->base;
}

int raiju_test_tmpdir(char *out, size_t capacity, const char *name) {
	char cwd[1024];
	if (getcwd(cwd, sizeof cwd) == NULL) {
		return -1;
	}
	char path[2048];
	if (snprintf(path, sizeof path, "%s/build/test-tmp/%s", cwd, name) >= (int)sizeof path) {
		return -1;
	}
	umask(0);
	for (char *cursor = path + 1; *cursor != '\0'; cursor++) {
		if (*cursor != '/') {
			continue;
		}
		*cursor = '\0';
		mkdir(path, 0755);
		*cursor = '/';
	}
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		return -1;
	}
	snprintf(out, capacity, "%s", path);
	return 0;
}

void raiju_test_rmtree(const char *path) {
	DIR *directory = opendir(path);
	if (directory != NULL) {
		struct dirent *entry;
		while ((entry = readdir(directory)) != NULL) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}
			char child[2048];
			snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
			struct stat info;
			if (lstat(child, &info) != 0) {
				continue;
			}
			if (S_ISDIR(info.st_mode)) {
				raiju_test_rmtree(child);
			} else {
				unlink(child);
			}
		}
		closedir(directory);
	}
	rmdir(path);
}
