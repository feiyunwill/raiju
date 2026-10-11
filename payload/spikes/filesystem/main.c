#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_STAGING RAIJU_DATA_ROOT "/staging"
#define RAIJU_INSTALL_ROOT "/data/homebrew"
#define RAIJU_SMP_PORT 10101
#define RAIJU_WRITE_BYTES (1024ull * 1024ull * 1024ull)
#define RAIJU_RENAME_BYTES (50ull * 1024ull * 1024ull * 1024ull)
#define RAIJU_CHUNK_BYTES (1024ull * 1024ull)
#define RAIJU_ALLOC_PROBE RAIJU_STAGING "/raiju-fallocate-probe.bin"
#define RAIJU_WRITE_PROBE RAIJU_STAGING "/raiju-write-probe.bin"
#define RAIJU_RENAME_PROBE RAIJU_STAGING "/raiju-rename-probe.bin"
#define RAIJU_RENAME_TARGET RAIJU_INSTALL_ROOT "/raiju-rename-probe.bin"

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

int sceNotificationSend(int userId, bool isLogged, const char *payload);

static const char toast_tmpl[] =
	"{"
	"  \"rawData\": {"
	"    \"viewTemplateType\": \"InteractiveToastTemplateB\","
	"    \"channelType\": \"Downloads\","
	"    \"useCaseId\": \"IDC\","
	"    \"toastOverwriteType\": \"No\","
	"    \"isImmediate\": true,"
	"    \"priority\": 100,"
	"    \"viewData\": {"
	"      \"icon\": {\"type\": \"Predefined\", \"parameters\": {\"icon\": \"download\"}},"
	"      \"message\": {\"body\": \"%s\"},"
	"      \"subMessage\": {\"body\": \"%s\"}"
	"    },"
	"    \"platformViews\": {"
	"      \"previewDisabled\": {"
	"        \"viewData\": {"
	"          \"icon\": {\"type\": \"Predefined\", \"parameters\": {\"icon\": \"download\"}},"
	"          \"message\": {\"body\": \"%s\"}"
	"        }"
	"      }"
	"    }"
	"  },"
	"  \"localNotificationId\": \"1\""
	"}";

/** Formats a byte count the way the UI does, e.g. `612.4 GB`. */
static void human_size(uint64_t bytes, char *out, size_t len) {
	static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
	double value = (double)bytes;
	size_t unit = 0;

	while (value >= 1000.0 && unit < 4) {
		value /= 1000.0;
		unit++;
	}
	snprintf(out, len, "%.1f %s", value, units[unit]);
}

/** Monotonic seconds, for timing the probes. */
static double monotonic_seconds(void) {
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0.0;
	}

	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/** Free and total bytes of a filesystem, zero when it cannot be read. */
static uint64_t space_of(const char *path, uint64_t *total) {
	struct statvfs fs;
	uint64_t block;

	if (statvfs(path, &fs) != 0) {
		return 0;
	}
	block = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
	if (total) {
		*total = (uint64_t)fs.f_blocks * block;
	}

	return (uint64_t)fs.f_bavail * block;
}

/** Prints how much space is left, so a probe that leaks a file is obvious. */
static void report_space(const char *label) {
	char free_text[32];
	char total_text[32];
	uint64_t total = 0;
	uint64_t free_bytes = space_of("/data", &total);

	human_size(free_bytes, free_text, sizeof(free_text));
	human_size(total, total_text, sizeof(total_text));
	printf("%s: %s free of %s\n", label, free_text, total_text);
}

/** Raises a toast and reports what the notification service answered. */
static void notify(const char *title, const char *detail) {
	char payload[2048];
	int result;

	if (snprintf(payload, sizeof(payload), toast_tmpl, title, detail, title) < 0) {
		return;
	}
	result = sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true,
				     payload);
	printf("notify: %d\n", result);
}

/** Says whether two paths live on one filesystem, which is what makes a rename atomic. */
static int same_filesystem(const char *left, const char *right) {
	struct stat left_stat;
	struct stat right_stat;

	if (stat(left, &left_stat) != 0 || stat(right, &right_stat) != 0) {
		return -1;
	}

	return left_stat.st_dev == right_stat.st_dev ? 1 : 0;
}

/** Writes `bytes` of a repeating pattern in chunks, the way a download lands. */
static int write_blocks(int fd, uint64_t bytes) {
	size_t chunk = RAIJU_CHUNK_BYTES;
	uint8_t *buffer = malloc(chunk);
	uint64_t done = 0;

	if (buffer == NULL) {
		printf("write: out of memory for a %zu byte buffer\n", chunk);
		return -1;
	}
	memset(buffer, 0x5a, chunk);

	while (done < bytes) {
		uint64_t remaining = bytes - done;
		size_t want = remaining < chunk ? (size_t)remaining : chunk;

		if (write(fd, buffer, want) != (ssize_t)want) {
			printf("write: %s after %llu bytes\n", strerror(errno),
			       (unsigned long long)done);
			free(buffer);
			return -1;
		}
		done += want;
	}
	free(buffer);

	return 0;
}

/** Reports whether `posix_fallocate` works on the data filesystem, and what it costs. */
static int probe_fallocate(void) {
	struct stat info;
	double started;
	double elapsed;
	int rc;
	int fd;

	unlink(RAIJU_ALLOC_PROBE);
	fd = open(RAIJU_ALLOC_PROBE, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) {
		printf("fallocate: cannot create %s: %s\n", RAIJU_ALLOC_PROBE,
		       strerror(errno));
		return 1;
	}

	started = monotonic_seconds();
	rc = posix_fallocate(fd, 0, (off_t)RAIJU_WRITE_BYTES);
	elapsed = monotonic_seconds() - started;

	if (rc != 0) {
		printf("fallocate: 1 GiB failed with %d (%s)\n", rc, strerror(rc));
		close(fd);
		unlink(RAIJU_ALLOC_PROBE);
		return 1;
	}

	if (fstat(fd, &info) != 0) {
		printf("fallocate: fstat failed: %s\n", strerror(errno));
		close(fd);
		unlink(RAIJU_ALLOC_PROBE);
		return 1;
	}
	printf("fallocate: 1 GiB in %.2f s, size %llu, blocks %llu (%.2f GiB allocated)\n",
	       elapsed, (unsigned long long)info.st_size,
	       (unsigned long long)info.st_blocks,
	       (double)info.st_blocks * 512.0 / 1073741824.0);
	close(fd);
	unlink(RAIJU_ALLOC_PROBE);

	return 0;
}

/** Times writing one size twice: into a fresh file, then into a preallocated one. */
static int probe_write_speed(const char *mode, bool preallocate) {
	double started;
	double elapsed;
	int fd;

	unlink(RAIJU_WRITE_PROBE);
	fd = open(RAIJU_WRITE_PROBE, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) {
		printf("write %s: cannot create %s: %s\n", mode, RAIJU_WRITE_PROBE,
		       strerror(errno));
		return 1;
	}
	if (preallocate && posix_fallocate(fd, 0, (off_t)RAIJU_WRITE_BYTES) != 0) {
		printf("write %s: falling back to ftruncate\n", mode);
		if (ftruncate(fd, (off_t)RAIJU_WRITE_BYTES) != 0) {
			printf("write %s: ftruncate: %s\n", mode, strerror(errno));
			close(fd);
			unlink(RAIJU_WRITE_PROBE);
			return 1;
		}
		lseek(fd, 0, SEEK_SET);
	}

	started = monotonic_seconds();
	if (write_blocks(fd, RAIJU_WRITE_BYTES) != 0) {
		close(fd);
		unlink(RAIJU_WRITE_PROBE);
		return 1;
	}
	if (fsync(fd) != 0) {
		printf("write %s: fsync: %s\n", mode, strerror(errno));
		close(fd);
		unlink(RAIJU_WRITE_PROBE);
		return 1;
	}
	elapsed = monotonic_seconds() - started;
	close(fd);
	unlink(RAIJU_WRITE_PROBE);

	printf("write %s: 1 GiB in %.2f s, %.0f MB/s (file-allocation=%s)\n", mode,
	       elapsed, 1000.0 / elapsed, preallocate ? "falloc" : "none");

	return 0;
}

/** Install-root, same-filesystem and 50 GiB rename checks, which is how a dump is installed. */
static int probe_rename(void) {
	struct stat info;
	uint64_t before;
	char size_text[32];
	double started;
	double elapsed;
	int fd;
	int rc;
	int same;

	if (stat(RAIJU_INSTALL_ROOT, &info) != 0) {
		printf("rename: %s is absent (%s), skipping the install-root probes\n",
		       RAIJU_INSTALL_ROOT, strerror(errno));
		return 1;
	}

	same = same_filesystem(RAIJU_STAGING, RAIJU_INSTALL_ROOT);
	printf("rename: %s and %s on one filesystem: %s\n", RAIJU_STAGING,
	       RAIJU_INSTALL_ROOT,
	       same == 1 ? "yes" : same == 0 ? "no" : "unknown");

	unlink(RAIJU_RENAME_PROBE);
	fd = open(RAIJU_RENAME_PROBE, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) {
		printf("rename: cannot create %s: %s\n", RAIJU_RENAME_PROBE,
		       strerror(errno));
		return 1;
	}
	before = space_of("/data", NULL);
	started = monotonic_seconds();
	rc = posix_fallocate(fd, 0, (off_t)RAIJU_RENAME_BYTES);
	elapsed = monotonic_seconds() - started;
	if (rc != 0) {
		printf("rename: fallocate of 50 GiB failed with %d (%s), truncating instead\n",
		       rc, strerror(rc));
		if (ftruncate(fd, (off_t)RAIJU_RENAME_BYTES) != 0) {
			printf("rename: ftruncate: %s\n", strerror(errno));
			close(fd);
			unlink(RAIJU_RENAME_PROBE);
			return 1;
		}
	} else {
		printf("rename: 50 GiB allocated in %.2f s\n", elapsed);
	}
	if (fstat(fd, &info) != 0) {
		printf("rename: fstat: %s\n", strerror(errno));
		close(fd);
		unlink(RAIJU_RENAME_PROBE);
		return 1;
	}
	human_size((uint64_t)info.st_size, size_text, sizeof(size_text));
	printf("rename: probe file is %s, %llu bytes in %llu blocks\n", size_text,
	       (unsigned long long)info.st_size,
	       (unsigned long long)info.st_blocks);
	close(fd);

	started = monotonic_seconds();
	if (rename(RAIJU_RENAME_PROBE, RAIJU_RENAME_TARGET) != 0) {
		printf("rename: into the install root failed: %s\n", strerror(errno));
		unlink(RAIJU_RENAME_PROBE);
		return 1;
	}
	elapsed = monotonic_seconds() - started;
	printf("rename: 50 GiB into %s took %.4f s\n", RAIJU_INSTALL_ROOT, elapsed);

	started = monotonic_seconds();
	if (rename(RAIJU_RENAME_TARGET, RAIJU_RENAME_PROBE) != 0) {
		printf("rename: back out of the install root failed: %s\n",
		       strerror(errno));
		unlink(RAIJU_RENAME_TARGET);
		return 1;
	}
	printf("rename: back out took %.4f s\n", monotonic_seconds() - started);

	if (unlink(RAIJU_RENAME_PROBE) != 0) {
		printf("rename: unlink: %s\n", strerror(errno));
		return 1;
	}
	printf("rename: probe removed, %llu bytes freed\n",
	       (unsigned long long)(space_of("/data", NULL) - before));

	return 0;
}

/** Says whether something is listening on the ShadowMountPlus API port. */
static int probe_smp(void) {
	struct sockaddr_in address;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		printf("smp: socket: %s\n", strerror(errno));
		return 1;
	}
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(RAIJU_SMP_PORT);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		printf("smp: 127.0.0.1:%d refuses connections (%s)\n", RAIJU_SMP_PORT,
		       strerror(errno));
		close(fd);
		return 1;
	}
	printf("smp: 127.0.0.1:%d accepts connections\n", RAIJU_SMP_PORT);
	close(fd);

	return 0;
}

/**
 * Measures what the atomic install pipeline assumes about the filesystem: that `posix_fallocate`
 * works and what it really allocates, what preallocation costs against writing directly (aria2's
 * `file-allocation=falloc` against `none`), whether the staging directory and the install root sit
 * on one filesystem so a 50 GiB `rename()` is a metadata move, and whether ShadowMountPlus is
 * listening. Reads and writes stay inside `/data/raiju`, and the one file it puts at the install
 * root is named so that ShadowMountPlus ignores it and is removed again before the run ends.
 */
int main(void) {
	char free_text[32];
	char total_text[32];
	char title[64];
	char detail[128];
	uint64_t total = 0;
	uint64_t free_bytes;

	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("raiju filesystem spike %s\n", RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());
	report_space("space");

	probe_fallocate();
	probe_write_speed("none", false);
	probe_write_speed("falloc", true);
	probe_rename();
	probe_smp();
	report_space("space after probes");

	free_bytes = space_of("/data", &total);
	human_size(free_bytes, free_text, sizeof(free_text));
	human_size(total, total_text, sizeof(total_text));
	snprintf(title, sizeof(title), "Raiju %s", RAIJU_VERSION);
	snprintf(detail, sizeof(detail), "filesystem probe done, %s free of %s",
		 free_text, total_text);
	notify(title, detail);

	printf("exit\n");
	payload_exit(0);

	return 0;
}
