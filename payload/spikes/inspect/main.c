#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define RAIJU_SCAN_CONFIG "/data/shadowmount/config.ini"
#define RAIJU_SHADOWMOUNT_DIR "/data/shadowmount"
#define RAIJU_MNT_DIR "/mnt"
#define RAIJU_INSTALL_ROOT "/data/homebrew"
#define RAIJU_PROCESS_LIST_MIB_COUNT 4
#define RAIJU_PROCESS_NAME_OFFSET 447
#define RAIJU_PROCESS_PID_OFFSET 72
#define RAIJU_TAIL_BYTES 8192

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

/** Prints every mounted filesystem, so a mount made by a payload is visible. */
static void report_mounts(void) {
	struct statfs *mounts;
	int count = getmntinfo(&mounts, MNT_NOWAIT);

	if (count <= 0) {
		printf("mounts: getmntinfo: %s\n", strerror(errno));
		return;
	}
	for (int i = 0; i < count; i++) {
		printf("mount: %s on %s, type %s\n", mounts[i].f_mntfromname,
		       mounts[i].f_mntonname, mounts[i].f_fstypename);
	}
}

/** Prints the kernel process list, so a payload that should be running is visible. */
static void report_processes(void) {
	int mib[RAIJU_PROCESS_LIST_MIB_COUNT] = {1, 14, 8, 0};
	pid_t mine = getpid();
	size_t buffer_size;
	uint8_t *buffer;

	if (sysctl(mib, RAIJU_PROCESS_LIST_MIB_COUNT, NULL, &buffer_size, NULL, 0) != 0) {
		printf("processes: sysctl: %s\n", strerror(errno));
		return;
	}
	buffer = malloc(buffer_size);
	if (buffer == NULL) {
		printf("processes: out of memory for %zu bytes\n", buffer_size);
		return;
	}
	if (sysctl(mib, RAIJU_PROCESS_LIST_MIB_COUNT, buffer, &buffer_size, NULL, 0) != 0) {
		printf("processes: sysctl: %s\n", strerror(errno));
		free(buffer);
		return;
	}

	for (uint8_t *entry = buffer; entry < buffer + buffer_size;) {
		int struct_size = *(int *)entry;
		pid_t pid = *(pid_t *)&entry[RAIJU_PROCESS_PID_OFFSET];
		const char *name = (const char *)&entry[RAIJU_PROCESS_NAME_OFFSET];

		if (struct_size <= 0) {
			break;
		}
		entry += struct_size;
		if (pid == mine || name[0] == '\0') {
			continue;
		}
		printf("process: %d %s\n", (int)pid, name);
	}
	free(buffer);
}

/** Prints the scan settings and the scan roots a mounted ShadowMountPlus would be using. */
static void report_scan_config(void) {
	char line[256];
	FILE *config = fopen(RAIJU_SCAN_CONFIG, "r");

	if (config == NULL) {
		printf("config: %s is absent (%s)\n", RAIJU_SCAN_CONFIG,
		       strerror(errno));
		return;
	}
	while (fgets(line, sizeof(line), config) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		printf("config: %s\n", line);
	}
	fclose(config);
}

/** Prints the entries of a directory, so files a payload keeps on disk are visible. */
static void report_directory(const char *path) {
	DIR *directory = opendir(path);
	struct dirent *entry;

	if (directory == NULL) {
		printf("dir %s: %s\n", path, strerror(errno));
		return;
	}
	while ((entry = readdir(directory)) != NULL) {
		printf("dir %s: %s\n", path, entry->d_name);
	}
	closedir(directory);
}

/** Prints the last bytes of a file, so a log that explains a decision is visible. */
static void report_tail(const char *path) {
	char buffer[RAIJU_TAIL_BYTES];
	size_t got;
	FILE *file = fopen(path, "rb");

	if (file == NULL) {
		printf("tail %s: %s\n", path, strerror(errno));
		return;
	}
	if (fseek(file, -(long)sizeof(buffer), SEEK_END) != 0 && fseek(file, 0, SEEK_SET) != 0) {
		printf("tail %s: seek: %s\n", path, strerror(errno));
		fclose(file);
		return;
	}
	printf("tail %s:\n", path);
	got = fread(buffer, 1, sizeof(buffer), file);
	if (got > 0) {
		fwrite(buffer, 1, got, stdout);
	}
	if (got == 0 || buffer[got - 1] != '\n') {
		printf("\n");
	}
	fclose(file);
}

/** Prints the regular files of a directory and the tail of each, one level deep. */
static void report_directory_tails(const char *path) {
	DIR *directory = opendir(path);
	struct dirent *entry;

	if (directory == NULL) {
		printf("tails %s: %s\n", path, strerror(errno));
		return;
	}
	while ((entry = readdir(directory)) != NULL) {
		char child[512];
		struct stat info;

		if (entry->d_name[0] == '.') {
			continue;
		}
		if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child)) {
			continue;
		}
		if (stat(child, &info) != 0) {
			continue;
		}
		if (S_ISDIR(info.st_mode)) {
			report_directory(child);
			report_directory_tails(child);
		} else if (S_ISREG(info.st_mode)) {
			report_tail(child);
		}
	}
	closedir(directory);
}

/** Prints what is under the install root, so a scan that did nothing is visible. */
static void report_install_root(void) {
	DIR *directory = opendir(RAIJU_INSTALL_ROOT);
	struct dirent *entry;

	if (directory == NULL) {
		printf("install root: %s is absent (%s)\n", RAIJU_INSTALL_ROOT,
		       strerror(errno));
		return;
	}
	while ((entry = readdir(directory)) != NULL) {
		printf("install root: %s\n", entry->d_name);
	}
	closedir(directory);
}

/**
 * Reports what a console is actually running, mounting and logging: the mount table, the kernel
 * process list, the ShadowMountPlus configuration, everything it keeps under
 * `/data/shadowmount` with the tail of each file, the contents of `/mnt`, and the install root. It
 * reads only, so it can answer "why was that image not picked up?" without taking anything from the
 * kernel or writing anything to disk.
 */
int main(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("raiju inspect spike %s\n", RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());
	report_mounts();
	report_processes();
	report_scan_config();
	report_directory(RAIJU_SHADOWMOUNT_DIR);
	report_directory_tails(RAIJU_SHADOWMOUNT_DIR);
	report_directory(RAIJU_MNT_DIR);
	report_install_root();
	printf("exit\n");
	payload_exit(0);

	return 0;
}
