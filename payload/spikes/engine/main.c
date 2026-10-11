#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

extern char **environ;

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_BIN_DIR RAIJU_DATA_ROOT "/bin"
#define RAIJU_ENGINE RAIJU_BIN_DIR "/aria2c"
#define RAIJU_SESSION_DIR RAIJU_DATA_ROOT "/session"
#define RAIJU_LOG RAIJU_SESSION_DIR "/aria2.log"
#define RAIJU_DOWNLOADS RAIJU_DATA_ROOT "/downloads"
#define RAIJU_RPC_PORT 6800
#define RAIJU_CHUNK_BYTES (256u * 1024u)
#define RAIJU_PROCESS_LIST_MIB_COUNT 4
#define RAIJU_PROCESS_NAME_OFFSET 447
#define RAIJU_PROCESS_PID_OFFSET 72
#define RAIJU_LOG_BYTES 4096
#define RAIJU_SETTLE_SECONDS 8

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
#define RAIJU_FETCH_NAME "aria2c"
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
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		printf("fetch: connect to %s:%d: %s\n", host, port, strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

/** Downloads the engine from a plain HTTP server on the local network into the data root. */
static int fetch_engine(const char *path, uint64_t *bytes_out) {
	char request[512];
	char *buffer = malloc(RAIJU_CHUNK_BYTES);
	char *body;
	ssize_t got;
	size_t written;
	uint64_t total = 0;
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
	snprintf(request, sizeof(request),
		 "GET /%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
		 RAIJU_FETCH_NAME, RAIJU_FETCH_HOST);
	if (write(connection, request, strlen(request)) != (ssize_t)strlen(request)) {
		printf("fetch: request: %s\n", strerror(errno));
		free(buffer);
		close(connection);
		return -1;
	}
	printf("fetch: GET http://%s:%d/%s\n", RAIJU_FETCH_HOST, RAIJU_FETCH_PORT,
	       RAIJU_FETCH_NAME);

	fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0755);
	if (fd < 0) {
		printf("fetch: cannot create %s: %s\n", path, strerror(errno));
		free(buffer);
		close(connection);
		return -1;
	}
	got = read(connection, buffer, RAIJU_CHUNK_BYTES);
	body = got > 0 ? strstr(buffer, "\r\n\r\n") : NULL;
	if (body == NULL) {
		printf("fetch: no response: %s\n", strerror(errno));
		free(buffer);
		close(connection);
		close(fd);
		return -1;
	}
	body += 4;
	printf("fetch: %.20s\n", buffer);
	written = (size_t)got - (size_t)(body - buffer);
	if (write(fd, body, written) != (ssize_t)written) {
		printf("fetch: write: %s\n", strerror(errno));
		free(buffer);
		close(connection);
		close(fd);
		return -1;
	}
	total += written;
	while ((got = read(connection, buffer, RAIJU_CHUNK_BYTES)) > 0) {
		if (write(fd, buffer, (size_t)got) != got) {
			printf("fetch: write: %s\n", strerror(errno));
			free(buffer);
			close(connection);
			close(fd);
			return -1;
		}
		total += (uint64_t)got;
	}
	fsync(fd);
	close(fd);
	close(connection);
	free(buffer);
	printf("fetch: %llu bytes\n", (unsigned long long)total);
	*bytes_out = total;

	return 0;
}

/** Prints every process whose name contains `needle`, so a spawned engine is visible. */
static int report_processes(const char *needle) {
	int mib[RAIJU_PROCESS_LIST_MIB_COUNT] = {1, 14, 8, 0};
	size_t buffer_size;
	uint8_t *buffer;
	int found = 0;

	if (sysctl(mib, RAIJU_PROCESS_LIST_MIB_COUNT, NULL, &buffer_size, NULL, 0) != 0) {
		printf("processes: sysctl: %s\n", strerror(errno));
		return -1;
	}
	buffer = malloc(buffer_size);
	if (buffer == NULL) {
		printf("processes: out of memory for %zu bytes\n", buffer_size);
		return -1;
	}
	if (sysctl(mib, RAIJU_PROCESS_LIST_MIB_COUNT, buffer, &buffer_size, NULL, 0) != 0) {
		printf("processes: sysctl: %s\n", strerror(errno));
		free(buffer);
		return -1;
	}
	for (uint8_t *entry = buffer; entry < buffer + buffer_size;) {
		int struct_size = *(int *)entry;
		pid_t pid = *(pid_t *)&entry[RAIJU_PROCESS_PID_OFFSET];
		const char *name = (const char *)&entry[RAIJU_PROCESS_NAME_OFFSET];

		if (struct_size <= 0) {
			break;
		}
		entry += struct_size;
		if (strstr(name, needle) == NULL) {
			continue;
		}
		printf("process: %d %s\n", (int)pid, name);
		found++;
	}
	free(buffer);

	return found;
}

/** Prints the printable tail of a log, so what the engine said is visible. */
static void report_log_tail(const char *path) {
	char buffer[RAIJU_LOG_BYTES];
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
		}
	}
	printf("\n");
}

/**
 * Measures whether a payload can run the engine at all, which is the first half of #5. It installs
 * the cross-compiled `aria2c` into `/data/raiju/bin`, spawns it with RPC enabled on a test-only
 * binding and logging into the session directory, then reports whether the process exists afterwards
 * and what it wrote. The engine is left running on purpose so the RPC can be driven from the
 * network; stopping it is `aria2.shutdown` over RPC.
 */
int main(void) {
	static char *const arguments[] = {
		RAIJU_ENGINE,
		"--no-conf",
		"--enable-rpc",
		"--rpc-listen-all=true",
		"--rpc-listen-port=6800",
		"--rpc-allow-origin-all=true",
		"--dir=" RAIJU_DOWNLOADS,
		"--file-allocation=none",
		"--continue=true",
		"--max-connection-per-server=16",
		"--summary-interval=10",
		"--console-log-level=notice",
		"--log-level=notice",
		NULL,
	};
	posix_spawn_file_actions_t actions;
	struct stat info;
	uint64_t bytes = 0;
	pid_t child = 0;
	int status = 0;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("raiju engine spike %s\n", RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());

	mkdir(RAIJU_DATA_ROOT, 0755);
	mkdir(RAIJU_BIN_DIR, 0755);
	mkdir(RAIJU_SESSION_DIR, 0755);
	mkdir(RAIJU_DOWNLOADS, 0755);

	if (stat(RAIJU_ENGINE, &info) != 0 || info.st_size == 0) {
		if (fetch_engine(RAIJU_ENGINE, &bytes) != 0) {
			unlink(RAIJU_ENGINE);
			payload_exit(1);
			return 1;
		}
		chmod(RAIJU_ENGINE, 0755);
	} else {
		printf("engine: %s is already installed (%llu bytes)\n", RAIJU_ENGINE,
		       (unsigned long long)info.st_size);
	}
	if (stat(RAIJU_ENGINE, &info) != 0) {
		printf("engine: %s is missing\n", RAIJU_ENGINE);
		payload_exit(1);
		return 1;
	}
	printf("engine: %llu bytes, mode %o\n", (unsigned long long)info.st_size,
	       (unsigned)(info.st_mode & 0777));

	unlink(RAIJU_LOG);
	if (posix_spawn_file_actions_init(&actions) != 0) {
		printf("spawn: file actions: %s\n", strerror(errno));
		payload_exit(1);
		return 1;
	}
	posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, RAIJU_LOG,
					 O_CREAT | O_WRONLY | O_TRUNC, 0644);
	posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

	result = posix_spawn(&child, RAIJU_ENGINE, &actions, NULL, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	if (result != 0) {
		printf("spawn: %s\n", strerror(result));
		payload_exit(1);
		return 1;
	}
	printf("spawn: aria2c is pid %d\n", (int)child);

	sleep(RAIJU_SETTLE_SECONDS);
	if (waitpid(child, &status, WNOHANG) == child) {
		printf("spawn: aria2c exited with status %d\n",
		       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	} else {
		printf("spawn: aria2c is still running\n");
	}
	printf("processes matching aria2c: %d\n", report_processes("aria2c"));
	report_log_tail(RAIJU_LOG);

	printf("engine: RPC is on port %d, listening on every interface for the test\n",
	       RAIJU_RPC_PORT);
	notify("Raiju", "the engine spike finished, aria2 RPC is up");
	printf("exit\n");
	payload_exit(0);

	return 0;
}
