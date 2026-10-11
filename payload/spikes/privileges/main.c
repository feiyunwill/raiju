#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_HOLD_SECONDS 15
#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_STAGING_FILE RAIJU_DATA_ROOT "/staging/privileges.probe"

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

static const char *data_dirs[] = {
	RAIJU_DATA_ROOT,
	RAIJU_DATA_ROOT "/downloads",
	RAIJU_DATA_ROOT "/staging",
	RAIJU_DATA_ROOT "/trash",
	RAIJU_DATA_ROOT "/session",
	RAIJU_DATA_ROOT "/www",
};

/** The credentials a process holds, kept so that everything taken can be given back. */
struct credentials {
	uint64_t authid;
	uint8_t caps[16];
	uid_t uid;
	uid_t ruid;
	uid_t svuid;
	gid_t rgid;
	gid_t svgid;
};

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
	fflush(stdout);
}

/** Creates one directory, saying whether it was made or was already there. */
static int ensure_dir(const char *path) {
	if (mkdir(path, 0755) == 0) {
		printf("mkdir %s: created\n", path);
		return 0;
	}
	if (errno == EEXIST) {
		printf("mkdir %s: already there\n", path);
		return 0;
	}
	printf("mkdir %s: %s\n", path, strerror(errno));
	return 1;
}

/** Reads the process's credentials into `creds`. */
static void credentials_read(pid_t pid, struct credentials *creds) {
	memset(creds, 0, sizeof(*creds));
	creds->authid = kernel_get_ucred_authid(pid);
	kernel_get_ucred_caps(pid, creds->caps);
	creds->uid = kernel_get_ucred_uid(pid);
	creds->ruid = kernel_get_ucred_ruid(pid);
	creds->svuid = kernel_get_ucred_svuid(pid);
	creds->rgid = kernel_get_ucred_rgid(pid);
	creds->svgid = kernel_get_ucred_svgid(pid);
}

/** Writes `creds` back onto the process, so a measurement leaves nothing behind. */
static void credentials_restore(pid_t pid, const struct credentials *creds) {
	kernel_set_ucred_authid(pid, creds->authid);
	kernel_set_ucred_caps(pid, creds->caps);
	kernel_set_ucred_uid(pid, creds->uid);
	kernel_set_ucred_ruid(pid, creds->ruid);
	kernel_set_ucred_svuid(pid, creds->svuid);
	kernel_set_ucred_rgid(pid, creds->rgid);
	kernel_set_ucred_svgid(pid, creds->svgid);
}

/** Prints the credentials a run is judged on, and whether they match a set already read. */
static void credentials_print(const char *label, pid_t pid,
			      const struct credentials *expected) {
	struct credentials now;

	credentials_read(pid, &now);
	printf("%s: uid %u, euid %u, ruid %u, svuid %u, rgid %u, svgid %u\n", label,
	       (unsigned)getuid(), (unsigned)geteuid(), (unsigned)now.ruid,
	       (unsigned)now.svuid, (unsigned)now.rgid, (unsigned)now.svgid);
	printf("%s: authid 0x%llx, caps %02x%02x..%02x%02x\n", label,
	       (unsigned long long)now.authid, now.caps[0], now.caps[1],
	       now.caps[14], now.caps[15]);
	if (expected != NULL) {
		printf("%s: reverted %s\n", label,
		       now.authid == expected->authid && now.uid == expected->uid &&
				       now.ruid == expected->ruid &&
				       now.svuid == expected->svuid &&
				       now.rgid == expected->rgid &&
				       now.svgid == expected->svgid &&
				       memcmp(now.caps, expected->caps, sizeof(now.caps)) == 0
			       ? "yes"
			       : "no");
	}
}

/** Creates every directory the supervisor owns, reporting the ones that fail. */
static int probe_dirs(void) {
	size_t count = sizeof(data_dirs) / sizeof(*data_dirs);
	int failures = 0;

	for (size_t i = 0; i < count; i++) {
		failures += ensure_dir(data_dirs[i]);
	}

	return failures;
}

/** Writes, flushes and renames a staging file, the way the pipeline commits a download. */
static int probe_write(void) {
	static const char body[] = "raiju privileges probe\n";
	const char *final_path = RAIJU_STAGING_FILE ".done";
	ssize_t written;
	int fd;

	fd = open(RAIJU_STAGING_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) {
		printf("write %s: %s\n", RAIJU_STAGING_FILE, strerror(errno));
		return 1;
	}
	written = write(fd, body, sizeof(body) - 1);
	if (written != (ssize_t)(sizeof(body) - 1)) {
		printf("write %s: %s\n", RAIJU_STAGING_FILE, strerror(errno));
		close(fd);
		return 1;
	}
	if (fsync(fd) != 0) {
		printf("fsync %s: %s\n", RAIJU_STAGING_FILE, strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	if (rename(RAIJU_STAGING_FILE, final_path) != 0) {
		printf("rename %s: %s\n", final_path, strerror(errno));
		return 1;
	}
	if (unlink(final_path) != 0) {
		printf("unlink %s: %s\n", final_path, strerror(errno));
		return 1;
	}
	printf("write %s: written, flushed, renamed, removed\n", RAIJU_STAGING_FILE);

	return 0;
}

/** Reports free and total space on the filesystem holding the data root. */
static int probe_space(void) {
	char free_text[32];
	char total_text[32];
	uint64_t total = 0;
	uint64_t free_bytes = space_of("/data", &total);

	if (free_bytes == 0) {
		printf("space /data: unreadable (%s)\n", strerror(errno));
		return 1;
	}
	human_size(free_bytes, free_text, sizeof(free_text));
	human_size(total, total_text, sizeof(total_text));
	printf("space /data: %s free of %s\n", free_text, total_text);

	return 0;
}

/** Runs every operation Raiju needs and returns how many of them failed. */
static int probe_all(void) {
	int failures = 0;

	failures += probe_dirs();
	failures += probe_write();
	failures += probe_space();

	return failures;
}

/**
 * Measures the smallest set of privileges Raiju actually needs. The loader's own context is tried
 * first, and nothing is taken from the kernel unless that context fails: the process's credentials
 * are read before any change, the narrowest candidate step is tried and re-probed, and everything
 * is put back and verified afterwards. No jail is borrowed — a payload runs inside a hijacked
 * system process, so kernel state owned by another process is not ours to install, and ending
 * through `payload_exit()` is the only exit that does not run the hijacked process's C++ teardown.
 */
int main(void) {
	char free_text[32];
	char total_text[32];
	char title[64];
	char detail[128];
	struct credentials saved;
	uint64_t total = 0;
	uint64_t free_bytes;
	pid_t pid = getpid();
	int failures;
	int lock;

	printf("raiju privileges spike %s\n", RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());

	printf("loader context:\n");
	failures = probe_all();

	if (failures == 0) {
		printf("escalation: not needed, the loader's context does everything Raiju asks\n");
	} else {
		printf("escalation: %d failed, measuring the narrowest step\n", failures);
		credentials_read(pid, &saved);
		credentials_print("before", pid, NULL);

		if (kernel_set_ucred_uid(pid, 0) == 0) {
			printf("uid: set to 0\n");
			printf("uid 0 context:\n");
			if (probe_all() == 0) {
				printf("escalation: uid 0 alone is enough\n");
			} else {
				printf("escalation: uid 0 alone is not enough, taking credentials too\n");
				kernel_set_ucred_ruid(pid, 0);
				kernel_set_ucred_svuid(pid, 0);
				kernel_set_ucred_rgid(pid, 0);
				kernel_set_ucred_svgid(pid, 0);
				kernel_set_ucred_authid(pid, 0x4800000000010003ull);
				printf("escalated context:\n");
				if (probe_all() == 0) {
					printf("escalation: the full set is enough\n");
				} else {
					printf("escalation: still failing, the loader's context is the problem\n");
				}
			}
		} else {
			printf("uid: could not be set (%s)\n", strerror(errno));
		}

		credentials_restore(pid, &saved);
		credentials_print("after", pid, &saved);
	}

	lock = open(RAIJU_DATA_ROOT "/session/raiju.lock", O_CREAT | O_RDWR, 0644);
	if (lock < 0) {
		printf("lock: %s\n", strerror(errno));
		payload_exit(1);
		return 1;
	}
	if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
		printf("lock: another instance holds it (%s)\n", strerror(errno));
		notify("Raiju is already running", "the second launch exited");
		close(lock);
		payload_exit(0);
		return 0;
	}
	printf("lock: taken\n");

	free_bytes = space_of("/data", &total);
	human_size(free_bytes, free_text, sizeof(free_text));
	human_size(total, total_text, sizeof(total_text));
	snprintf(title, sizeof(title), "Raiju %s", RAIJU_VERSION);
	snprintf(detail, sizeof(detail), "%s free of %s on /data", free_text,
		 total_text);
	notify(title, detail);

	printf("hold: keeping the lock for %d s\n", RAIJU_HOLD_SECONDS);
	sleep(RAIJU_HOLD_SECONDS);
	close(lock);
	printf("exit\n");
	fflush(stdout);

	payload_exit(0);
	return 0;
}
