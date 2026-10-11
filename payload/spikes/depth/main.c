#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_INSTALL_ROOT "/data/homebrew"
#define RAIJU_SUBDIR RAIJU_INSTALL_ROOT "/raiju-depth-probe"
#define RAIJU_NAME "PPSA29629.exfat"
#define RAIJU_TITLE "PPSA29629"
#define RAIJU_INSTALLED RAIJU_INSTALL_ROOT "/" RAIJU_NAME
#define RAIJU_NESTED RAIJU_SUBDIR "/" RAIJU_NAME
#define RAIJU_DEBUG_LOG "/data/shadowmount/debug.log"
#define RAIJU_WAIT_SECONDS 15
#define RAIJU_PHASE_SECONDS 35
#define RAIJU_LOG_BYTES 4096

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

/** Prints every mount whose path or source mentions the title, and how many there were. */
static int mount_matching(const char *label, const char *needle) {
	struct statfs *mounts;
	int found = 0;
	int count = getmntinfo(&mounts, MNT_NOWAIT);

	if (count <= 0) {
		printf("%s: getmntinfo: %s\n", label, strerror(errno));
		return -1;
	}
	for (int i = 0; i < count; i++) {
		if (strstr(mounts[i].f_mntonname, needle) == NULL &&
		    strstr(mounts[i].f_mntfromname, needle) == NULL) {
			continue;
		}
		printf("%s: mounted at %s from %s, type %s\n", label,
		       mounts[i].f_mntonname, mounts[i].f_mntfromname,
		       mounts[i].f_fstypename);
		found++;
	}

	return found;
}

/** Watches the mount table across a whole number of ShadowMountPlus scan intervals. */
static int watch_mount(const char *label, int seconds) {
	int found = mount_matching(label, RAIJU_TITLE);

	for (int elapsed = 0; elapsed + RAIJU_WAIT_SECONDS <= seconds; elapsed += RAIJU_WAIT_SECONDS) {
		printf("%s: %s after %d s\n", label, found > 0 ? "mounted" : "not mounted",
		       elapsed);
		sleep(RAIJU_WAIT_SECONDS);
		found = mount_matching(label, RAIJU_TITLE);
	}
	printf("%s: %s after %d s\n", label, found > 0 ? "mounted" : "not mounted",
	       seconds);

	return found;
}

/** Prints the printable tail of a log, so ShadowMountPlus's own account is visible. */
static void report_log_tail(const char *path) {
	char buffer[RAIJU_LOG_BYTES];
	size_t kept = 0;
	size_t got;
	FILE *file = fopen(path, "rb");

	if (file == NULL) {
		printf("log %s: %s\n", path, strerror(errno));
		return;
	}
	if (fseek(file, -(long)sizeof(buffer), SEEK_END) != 0 && fseek(file, 0, SEEK_SET) != 0) {
		printf("log %s: seek: %s\n", path, strerror(errno));
		fclose(file);
		return;
	}
	got = fread(buffer, 1, sizeof(buffer), file);
	fclose(file);

	printf("log %s:\n", path);
	for (size_t i = 0; i < got; i++) {
		char character = buffer[i];

		if (character == '\n' || character == '\t' || (character >= 32 && character < 127)) {
			putchar(character);
			kept++;
		}
	}
	if (kept == 0 || buffer[got > 0 ? got - 1 : 0] != '\n') {
		printf("\n");
	}
}

/**
 * Measures the last open question of the install pipeline: whether an image one level down is
 * picked up. It moves the real image from the install root into a subfolder, watches the mount
 * table across two of ShadowMountPlus's own scan intervals and prints that period's log, then puts
 * the image back exactly where it was and watches again. The file never leaves `/data`, so both
 * moves are instant renames, and the run ends with the image installed as it started.
 */
int main(void) {
	struct stat info;
	int nested_mounted;
	int restored_mounted;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("raiju depth spike %s\n", RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());

	if (stat(RAIJU_INSTALLED, &info) != 0) {
		printf("depth: %s is not installed (%s), nothing to move\n",
		       RAIJU_INSTALLED, strerror(errno));
		payload_exit(1);
		return 1;
	}
	printf("depth: %s is %llu bytes\n", RAIJU_INSTALLED,
	       (unsigned long long)info.st_size);
	watch_mount("before", RAIJU_PHASE_SECONDS);

	if (mkdir(RAIJU_SUBDIR, 0755) != 0 && errno != EEXIST) {
		printf("depth: cannot create %s: %s\n", RAIJU_SUBDIR, strerror(errno));
		payload_exit(1);
		return 1;
	}
	if (rename(RAIJU_INSTALLED, RAIJU_NESTED) != 0) {
		printf("depth: rename into the subfolder failed: %s\n", strerror(errno));
		payload_exit(1);
		return 1;
	}
	printf("depth: moved to %s\n", RAIJU_NESTED);
	nested_mounted = watch_mount("nested", RAIJU_PHASE_SECONDS);
	report_log_tail(RAIJU_DEBUG_LOG);

	if (rename(RAIJU_NESTED, RAIJU_INSTALLED) != 0) {
		printf("depth: rename back failed: %s\n", strerror(errno));
		payload_exit(1);
		return 1;
	}
	rmdir(RAIJU_SUBDIR);
	printf("depth: moved back to %s\n", RAIJU_INSTALLED);
	restored_mounted = watch_mount("restored", RAIJU_PHASE_SECONDS);
	report_log_tail(RAIJU_DEBUG_LOG);

	printf("depth: nested %s, restored %s\n",
	       nested_mounted > 0 ? "still mounted" : "not mounted",
	       restored_mounted > 0 ? "mounted again" : "not mounted");
	notify("Raiju", "the scan depth probe finished");
	printf("exit\n");
	payload_exit(0);

	return 0;
}
