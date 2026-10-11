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
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_STAGING "/data/raiju/staging"
#define RAIJU_INSTALL_ROOT "/data/homebrew"
#define RAIJU_CHUNK_BYTES (256u * 1024u)
#define RAIJU_REPORT_BYTES (64u * 1024u * 1024u)
#define RAIJU_MOUNT_ATTEMPTS 5
#define RAIJU_MOUNT_SECONDS 15

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

#ifndef RAIJU_FETCH_HOST
#define RAIJU_FETCH_HOST "192.168.100.74"
#endif

#ifndef RAIJU_FETCH_PORT
#define RAIJU_FETCH_PORT 8000
#endif

#ifndef RAIJU_FETCH_NAME
#define RAIJU_FETCH_NAME "PPSA29629.exfat"
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

/** Monotonic seconds, for timing the transfer. */
static double monotonic_seconds(void) {
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0.0;
	}

	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/** Connects to an address on the local network, returning the socket or -1. */
static int connect_to(const char *host, int port) {
	struct sockaddr_in address;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		printf("fetch: socket: %s\n", strerror(errno));
		return -1;
	}
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((uint16_t)port);
	address.sin_addr.s_addr = inet_addr(host);
	if (address.sin_addr.s_addr == INADDR_NONE) {
		printf("fetch: %s is not an address\n", host);
		close(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		printf("fetch: connect to %s:%d: %s\n", host, port, strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

/** Downloads the image over plain HTTP into the staging directory, reporting progress. */
static int fetch_image(const char *out_path, uint64_t *bytes_out) {
	char request[512];
	char *buffer = malloc(RAIJU_CHUNK_BYTES);
	char *body;
	ssize_t got;
	size_t header_length;
	double started;
	double elapsed;
	uint64_t total = 0;
	uint64_t reported = 0;
	int fd;
	int connection;

	if (buffer == NULL) {
		printf("fetch: out of memory for a %u byte buffer\n", RAIJU_CHUNK_BYTES);
		return -1;
	}
	connection = connect_to(RAIJU_FETCH_HOST, RAIJU_FETCH_PORT);
	if (connection < 0) {
		free(buffer);
		return -1;
	}
	if (snprintf(request, sizeof(request),
		     "GET /%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
		     RAIJU_FETCH_NAME, RAIJU_FETCH_HOST) < 0) {
		free(buffer);
		close(connection);
		return -1;
	}
	if (write(connection, request, strlen(request)) != (ssize_t)strlen(request)) {
		printf("fetch: request: %s\n", strerror(errno));
		free(buffer);
		close(connection);
		return -1;
	}
	printf("fetch: GET http://%s:%d/%s\n", RAIJU_FETCH_HOST, RAIJU_FETCH_PORT,
	       RAIJU_FETCH_NAME);

	fd = open(out_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) {
		printf("fetch: cannot create %s: %s\n", out_path, strerror(errno));
		free(buffer);
		close(connection);
		return -1;
	}

	got = read(connection, buffer, RAIJU_CHUNK_BYTES);
	if (got <= 0) {
		printf("fetch: no response: %s\n", strerror(errno));
		free(buffer);
		close(connection);
		close(fd);
		return -1;
	}
	body = strstr(buffer, "\r\n\r\n");
	if (body == NULL) {
		printf("fetch: no header terminator in %zd bytes\n", got);
		free(buffer);
		close(connection);
		close(fd);
		return -1;
	}
	body += 4;
	header_length = (size_t)(body - buffer);
	printf("fetch: %.*s\n", (int)(header_length - 4), buffer);

	started = monotonic_seconds();
	if (write(fd, body, (size_t)got - header_length) != (ssize_t)((size_t)got - header_length)) {
		printf("fetch: write: %s\n", strerror(errno));
		free(buffer);
		close(connection);
		close(fd);
		return -1;
	}
	total += (uint64_t)got - header_length;

	while ((got = read(connection, buffer, RAIJU_CHUNK_BYTES)) > 0) {
		if (write(fd, buffer, (size_t)got) != got) {
			printf("fetch: write: %s\n", strerror(errno));
			free(buffer);
			close(connection);
			close(fd);
			return -1;
		}
		total += (uint64_t)got;
		if (total - reported >= RAIJU_REPORT_BYTES) {
			reported = total;
			printf("fetch: %llu MiB\n", (unsigned long long)(total >> 20));
		}
	}
	elapsed = monotonic_seconds() - started;
	fsync(fd);
	close(fd);
	close(connection);
	free(buffer);

	printf("fetch: %llu bytes in %.1f s, %.0f MB/s\n",
	       (unsigned long long)total, elapsed, elapsed > 0.0 ? (double)total / elapsed / 1e6 : 0.0);
	*bytes_out = total;

	return 0;
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

/** Waits through ShadowMountPlus's own scan interval, reporting whether a mount appeared. */
static int wait_for_mount(const char *label, const char *needle, int attempts) {
	for (int i = 0; i < attempts; i++) {
		int found = mount_matching(label, needle);

		if (found > 0) {
			printf("%s: %s mounted within %d s\n", label, needle,
			       i * RAIJU_MOUNT_SECONDS);
			return 1;
		}
		printf("%s: %s not mounted yet, waiting %d s\n", label, needle,
		       RAIJU_MOUNT_SECONDS);
		sleep(RAIJU_MOUNT_SECONDS);
	}
	printf("%s: %s did not mount within %d s\n", label, needle,
	       attempts * RAIJU_MOUNT_SECONDS);

	return 0;
}

/**
 * Installs one real game image the way Raiju's pipeline will: it fetches the image from a plain
 * HTTP server on the local network into `/data/raiju/staging`, flushes it, renames it into
 * `/data/homebrew/<TITLE_ID>.exfat` in one step, and then watches the mount table through
 * ShadowMountPlus's own scan interval to see whether it mounts. An existing install is reported
 * rather than overwritten, and nothing is removed at the end, because the image is a real dump.
 */
int main(void) {
	char staged[256];
	char installed[256];
	struct stat info;
	uint64_t bytes = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("raiju install spike %s\n", RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());

	if (snprintf(staged, sizeof(staged), "%s/%s", RAIJU_STAGING, RAIJU_FETCH_NAME) < 0 ||
	    snprintf(installed, sizeof(installed), "%s/%s", RAIJU_INSTALL_ROOT, RAIJU_FETCH_NAME) < 0) {
		printf("paths: too long\n");
		payload_exit(1);
		return 1;
	}

	if (stat(installed, &info) == 0) {
		printf("install: %s is already there (%llu bytes), not overwriting\n",
		       installed, (unsigned long long)info.st_size);
		wait_for_mount("existing", "PPSA29629", RAIJU_MOUNT_ATTEMPTS);
		payload_exit(0);
		return 0;
	}

	if (fetch_image(staged, &bytes) != 0) {
		unlink(staged);
		payload_exit(1);
		return 1;
	}
	if (rename(staged, installed) != 0) {
		printf("install: rename into %s failed: %s\n", RAIJU_INSTALL_ROOT,
		       strerror(errno));
		payload_exit(1);
		return 1;
	}
	printf("install: renamed %llu bytes to %s\n", (unsigned long long)bytes,
	       installed);

	wait_for_mount("installed", "PPSA29629", RAIJU_MOUNT_ATTEMPTS);

	notify("Raiju", "the game image is installed");
	printf("exit\n");
	payload_exit(0);

	return 0;
}
