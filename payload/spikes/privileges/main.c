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
#include <sys/sysctl.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_HOLD_SECONDS 15
#define RAIJU_DATA_ROOT "/data/raiju"

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

static const char *jail_sources[] = {
	"SceSysCore",
	"init",
	"SceShellCore",
	"SceShellUI",
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

/** Finds a process by name in the kernel process list, or -1. */
static pid_t find_pid(const char *name) {
	int mib[4] = {1, 14, 8, 0};
	pid_t mypid = getpid();
	pid_t pid = -1;
	size_t buf_size;
	uint8_t *buf;

	if (sysctl(mib, 4, 0, &buf_size, 0, 0)) {
		return -1;
	}
	if (!(buf = malloc(buf_size))) {
		return -1;
	}
	if (sysctl(mib, 4, buf, &buf_size, 0, 0)) {
		free(buf);
		return -1;
	}

	for (uint8_t *ptr = buf; ptr < (buf + buf_size);) {
		int struct_size = *(int *)ptr;
		pid_t entry_pid = *(pid_t *)&ptr[72];
		char *entry_name = (char *)&ptr[447];

		if (struct_size <= 0) {
			break;
		}
		ptr += struct_size;
		if (!strcmp(name, entry_name) && entry_pid != mypid) {
			pid = entry_pid;
		}
	}

	free(buf);

	return pid;
}

/** Borrows the jail directory of a system process, which sits outside the app sandbox. */
static intptr_t borrowed_jaildir(pid_t *from_pid) {
	size_t count = sizeof(jail_sources) / sizeof(*jail_sources);

	for (size_t i = 0; i < count; i++) {
		pid_t pid = find_pid(jail_sources[i]);
		intptr_t jail;

		if (pid <= 0) {
			continue;
		}
		jail = kernel_get_proc_jaildir(pid);
		if (jail) {
			*from_pid = pid;
			return jail;
		}
	}

	return 0;
}

/** Creates one directory, saying whether it was made or was already there. */
static void ensure_dir(const char *path) {
	if (mkdir(path, 0755) == 0) {
		printf("mkdir %s: created\n", path);
		return;
	}
	printf("mkdir %s: %s\n", path,
	       errno == EEXIST ? "already there" : strerror(errno));
}

/** Takes privileged credentials and a jail outside the app sandbox, printing both sides. */
static void escalate(void) {
	pid_t pid = getpid();
	pid_t source = -1;
	uint8_t caps[16];
	uint8_t attrs[32];
	uint8_t qaflags[16];
	intptr_t jail;
	int result;

	memset(caps, 0xff, sizeof(caps));
	kernel_get_ucred_attrs(pid, attrs);
	kernel_get_qaflags(qaflags);

	printf("before: uid %d, euid %d, jaildir %p\n", getuid(), geteuid(),
	       (void *)kernel_get_proc_jaildir(pid));
	printf("before: authid 0x%llx\n",
	       (unsigned long long)kernel_get_ucred_authid(pid));
	printf("before: attrs %02x%02x..%02x, qaflags %02x%02x\n", attrs[0], attrs[1],
	       attrs[31], qaflags[0], qaflags[15]);

	result = kernel_set_ucred_authid(pid, 0x4800000000010003ull);
	printf("authid: set %d\n", result);
	result = kernel_set_ucred_caps(pid, caps);
	printf("caps: set %d\n", result);
	result = kernel_set_ucred_uid(pid, 0);
	printf("uid: set %d\n", result);
	result = kernel_set_ucred_ruid(pid, 0);
	printf("ruid: set %d\n", result);
	result = kernel_set_ucred_svuid(pid, 0);
	printf("svuid: set %d\n", result);
	result = kernel_set_ucred_rgid(pid, 0);
	printf("rgid: set %d\n", result);
	result = kernel_set_ucred_svgid(pid, 0);
	printf("svgid: set %d\n", result);

	jail = borrowed_jaildir(&source);
	if (jail == 0) {
		printf("jaildir: no system process to borrow from\n");
		return;
	}
	result = kernel_set_proc_jaildir(pid, jail);
	printf("jaildir: %p borrowed from pid %d, set %d\n", (void *)jail,
	       (int)source, result);

	printf("after: uid %d, euid %d, jaildir %p\n", getuid(), geteuid(),
	       (void *)kernel_get_proc_jaildir(pid));
	printf("after: authid 0x%llx\n",
	       (unsigned long long)kernel_get_ucred_authid(pid));
}

/**
 * Measures what a payload gets on a real console: the credentials and jail it starts with, what
 * escalation buys it, whether `/data/raiju` can be created, what `statvfs` says about `/data`, and
 * whether a second launch is turned away while the first holds the lock. Ends through
 * `payload_exit()`, because returning from `main` runs the hijacked process's teardown and dies in
 * the C++ runtime.
 */
int main(void) {
	char free_text[32];
	char total_text[32];
	char title[64];
	char detail[128];
	uint64_t total = 0;
	uint64_t free_bytes;
	int lock;

	printf("raiju privileges spike %s\n", RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());

	ensure_dir(RAIJU_DATA_ROOT);

	escalate();

	ensure_dir(RAIJU_DATA_ROOT "/downloads");
	ensure_dir(RAIJU_DATA_ROOT "/staging");
	ensure_dir(RAIJU_DATA_ROOT "/session");

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

	free_bytes = space_of("/data", &total);
	human_size(free_bytes, free_text, sizeof(free_text));
	human_size(total, total_text, sizeof(total_text));
	printf("space: %s free of %s on /data\n", free_text, total_text);

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
