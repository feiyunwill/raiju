#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_SPIKE_DIR RAIJU_DATA_ROOT "/spike"
#define RAIJU_EXTRACT_ROOT RAIJU_DATA_ROOT "/spike-extract"
#define RAIJU_BLOCK_BYTES (1024ull * 1024ull)
#define RAIJU_PROGRESS_BYTES (2ull * 1024ull * 1024ull * 1024ull)
#define RAIJU_FREE_RESERVE_BYTES (512ull * 1024ull * 1024ull)

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

#ifndef RAIJU_FETCH_HOST
#define RAIJU_FETCH_HOST "192.168.100.74"
#endif

#ifndef RAIJU_FETCH_PORT
#define RAIJU_FETCH_PORT 8000
#endif

#ifndef RAIJU_SAMPLE_NAME
#define RAIJU_SAMPLE_NAME "sample.7z"
#endif

#ifndef RAIJU_ARCHIVE_PATH
#define RAIJU_ARCHIVE_PATH RAIJU_SPIKE_DIR "/" RAIJU_SAMPLE_NAME
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
	fflush(stdout);
}

/** Seconds of wall clock since started, from a monotonic source. */
static double elapsed_seconds(const struct timespec *started) {
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0.0;
	}

	return (double)(now.tv_sec - started->tv_sec) +
	       (double)(now.tv_nsec - started->tv_nsec) / 1000000000.0;
}

/** Free bytes on the filesystem holding path, or -1 when it cannot be read. */
static int64_t free_bytes(const char *path) {
	struct statvfs info;

	if (statvfs(path, &info) != 0) {
		return -1;
	}

	return (int64_t)info.f_bavail * (int64_t)info.f_frsize;
}

/** Removes a directory tree, never following a symlink out of it. */
static void remove_tree(const char *path) {
	DIR *directory = opendir(path);

	if (!directory) {
		return;
	}

	struct dirent *entry;
	while ((entry = readdir(directory))) {
		char child[1024];
		struct stat info;

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
			continue;
		}
		if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
		    (int)sizeof(child)) {
			continue;
		}
		if (lstat(child, &info) != 0) {
			continue;
		}
		if (S_ISDIR(info.st_mode)) {
			remove_tree(child);
		} else {
			unlink(child);
		}
	}
	closedir(directory);
	rmdir(path);
}

/** Creates every component of a path, creating directories on the way. */
static int make_directories(const char *path) {
	char buffer[1024];

	if (snprintf(buffer, sizeof(buffer), "%s", path) >= (int)sizeof(buffer)) {
		return -1;
	}

	for (char *cursor = buffer + 1; *cursor; cursor++) {
		if (*cursor != '/') {
			continue;
		}
		*cursor = '\0';
		mkdir(buffer, 0755);
		*cursor = '/';
	}

	if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
		return -1;
	}

	return 0;
}

/** Opens a TCP connection to a host and port, or -1 when it cannot. */
static int connect_to(const char *host, int port) {
	struct addrinfo hints;
	struct addrinfo *addresses = NULL;
	struct addrinfo *address;
	char service[16];
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	snprintf(service, sizeof(service), "%d", port);
	if (getaddrinfo(host, service, &hints, &addresses) != 0) {
		return -1;
	}

	for (address = addresses; address; address = address->ai_next) {
		fd = socket(address->ai_family, address->ai_socktype,
			    address->ai_protocol);
		if (fd < 0) {
			continue;
		}
		if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
			break;
		}
		close(fd);
		fd = -1;
	}

	freeaddrinfo(addresses);

	return fd;
}

/** Writes a whole buffer to a descriptor, or -1 when it cannot finish. */
static int write_all(int fd, const void *buffer, size_t length) {
	const char *bytes = buffer;
	size_t done = 0;

	while (done < length) {
		ssize_t put = write(fd, bytes + done, length - done);

		if (put < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		done += (size_t)put;
	}

	return 0;
}

/**
 * Fetches the sample from the host's plain HTTP server into out_path, so the
 * console needs nothing mounted to get the archive under test, and prints the
 * rate it arrived at.
 */
static int download(const char *out_path) {
	struct timespec started;
	char request[512];
	char *buffer;
	uint64_t received = 0;
	uint64_t next_progress = RAIJU_PROGRESS_BYTES;
	bool in_body = false;
	int fd;
	FILE *out;
	int result = -1;

	fd = connect_to(RAIJU_FETCH_HOST, RAIJU_FETCH_PORT);
	if (fd < 0) {
		printf("download: cannot reach %s:%d\n", RAIJU_FETCH_HOST,
		       RAIJU_FETCH_PORT);
		return -1;
	}

	buffer = malloc((size_t)RAIJU_BLOCK_BYTES + 1);
	out = fopen(out_path, "wb");
	if (buffer == NULL || out == NULL) {
		printf("download: cannot write %s\n", out_path);
		if (out != NULL) {
			fclose(out);
		}
		free(buffer);
		close(fd);
		return -1;
	}

	snprintf(request, sizeof(request),
		 "GET /%s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: raiju-spike\r\n"
		 "Accept: */*\r\nConnection: close\r\n\r\n",
		 RAIJU_SAMPLE_NAME, RAIJU_FETCH_HOST, RAIJU_FETCH_PORT);
	if (write_all(fd, request, strlen(request)) != 0) {
		printf("download: request failed\n");
		goto done;
	}

	printf("download: http://%s:%d/%s\n", RAIJU_FETCH_HOST, RAIJU_FETCH_PORT,
	       RAIJU_SAMPLE_NAME);
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &started);

	for (;;) {
		ssize_t got;

		if (received >= next_progress) {
			double seconds = elapsed_seconds(&started);

			printf("progress: %.1f GB downloaded, %.1f s, %.2f MB/s\n",
			       (double)received / 1e9, seconds,
			       seconds > 0 ? (double)received / 1e6 / seconds : 0.0);
			fflush(stdout);
			next_progress += RAIJU_PROGRESS_BYTES;
		}

		got = read(fd, buffer, (size_t)RAIJU_BLOCK_BYTES);

		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			printf("download: read failed: %s\n", strerror(errno));
			goto done;
		}
		if (got == 0) {
			break;
		}

		if (!in_body) {
			size_t have = (size_t)got;
			char *body;

			buffer[have] = '\0';
			body = strstr(buffer, "\r\n\r\n");
			if (body == NULL) {
				continue;
			}
			if (strncmp(buffer, "HTTP/1.1 200", 12) != 0 &&
			    strncmp(buffer, "HTTP/1.0 200", 12) != 0) {
				printf("download: %s\n", buffer);
				goto done;
			}
			in_body = true;
			body += 4;
			have = (size_t)(buffer + have - body);
			if (write_all(fileno(out), body, have) != 0) {
				printf("download: write failed\n");
				goto done;
			}
			received += (uint64_t)have;
			continue;
		}

		if (write_all(fileno(out), buffer, (size_t)got) != 0) {
			printf("download: write failed\n");
			goto done;
		}
		received += (uint64_t)got;
	}

	{
		double seconds = elapsed_seconds(&started);

		printf("downloaded: %llu bytes in %.2f s, %.2f MB/s\n",
		       (unsigned long long)received, seconds,
		       seconds > 0 ? (double)received / 1e6 / seconds : 0.0);
	}
	result = received > 0 ? 0 : -1;

done:
	fclose(out);
	free(buffer);
	close(fd);

	if (result != 0) {
		unlink(out_path);
	}

	return result;
}

/**
 * Whether an archive entry may be written: relative, without a backslash, and
 * without a component that walks up. Anything else is refused rather than
 * sanitised, matching what the pipeline does.
 */
static bool entry_path_is_safe(const char *path) {
	const char *component;

	if (path[0] == '/' || path[0] == '\0') {
		return false;
	}
	if (strchr(path, '\\') != NULL) {
		return false;
	}

	component = path;
	while (*component) {
		const char *end = strchr(component, '/');
		size_t length = end ? (size_t)(end - component) : strlen(component);

		if (length == 2 && component[0] == '.' && component[1] == '.') {
			return false;
		}
		if (end == NULL) {
			break;
		}
		component = end + 1;
	}

	return true;
}

/** Prints the filter chain and format libarchive picked for this archive. */
static void print_decoders(struct archive *reader) {
	printf("format: %s\n", archive_format_name(reader));
	printf("filters:");
	for (int i = 0; i < archive_filter_count(reader); i++) {
		printf(" %s", archive_filter_name(reader, i));
	}
	printf("\n");
	fflush(stdout);
}

/**
 * Extracts one archive into RAIJU_EXTRACT_ROOT and prints how many bytes were
 * written per second, which is the figure the ADR needs.
 */
static int extract(const char *archive_path) {
	struct timespec started;
	struct archive *reader;
	struct archive_entry *entry;
	char target[1024];
	char parent[1024];
	uint64_t written = 0;
	uint64_t entries = 0;
	uint64_t next_progress = RAIJU_PROGRESS_BYTES;
	int status = 1;
	bool described = false;
	int code;

	printf("archive: %s\n", archive_path);
	printf("target: %s\n", RAIJU_EXTRACT_ROOT);
	printf("free before: %lld bytes\n",
	       (long long)free_bytes(RAIJU_DATA_ROOT));

	remove_tree(RAIJU_EXTRACT_ROOT);
	if (make_directories(RAIJU_EXTRACT_ROOT) != 0) {
		printf("extract: cannot create %s\n", RAIJU_EXTRACT_ROOT);
		return 1;
	}

	reader = archive_read_new();
	archive_read_support_filter_all(reader);
	archive_read_support_format_all(reader);

	clock_gettime(CLOCK_MONOTONIC, &started);
	if (archive_read_open_filename(reader, archive_path,
				       (size_t)RAIJU_BLOCK_BYTES) != ARCHIVE_OK) {
		printf("open: %s\n", archive_error_string(reader));
		archive_read_free(reader);
		return 1;
	}

	while ((code = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
		const char *path = archive_entry_pathname(entry);
		const void *block;
		size_t size;
		int64_t offset;
		int data_code;
		int fd;

		if (!described) {
			print_decoders(reader);
			described = true;
		}
		if (path == NULL || !entry_path_is_safe(path)) {
			printf("refused: %s\n", path ? path : "(no name)");
			goto done;
		}
		if (archive_entry_filetype(entry) != AE_IFREG &&
		    archive_entry_filetype(entry) != AE_IFDIR) {
			printf("refused: %s (not a regular file)\n", path);
			goto done;
		}
		if (snprintf(target, sizeof(target), "%s/%s", RAIJU_EXTRACT_ROOT,
			     path) >= (int)sizeof(target)) {
			printf("refused: %s (path too long)\n", path);
			goto done;
		}

		if (archive_entry_filetype(entry) == AE_IFDIR) {
			make_directories(target);
			entries++;
			continue;
		}

		if (snprintf(parent, sizeof(parent), "%s", target) <
		    (int)sizeof(parent)) {
			char *slash = strrchr(parent, '/');

			if (slash != NULL) {
				*slash = '\0';
				make_directories(parent);
			}
		}

		fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			printf("write: %s: %s\n", target, strerror(errno));
			goto done;
		}

		while ((data_code = archive_read_data_block(reader, &block, &size,
							    &offset)) == ARCHIVE_OK) {
			if (write_all(fd, block, size) != 0) {
				printf("write: %s: %s\n", target,
				       strerror(errno));
				close(fd);
				goto done;
			}
			written += (uint64_t)size;
		}
		close(fd);

		if (data_code != ARCHIVE_EOF) {
			printf("read: %s: %s\n", path,
			       archive_error_string(reader));
			goto done;
		}
		entries++;

		if (written >= next_progress) {
			double seconds = elapsed_seconds(&started);
			int64_t now_free = free_bytes(RAIJU_DATA_ROOT);

			printf("progress: %.1f GB written, %.1f s, %.1f MB/s, %lld free\n",
			       (double)written / 1e9, seconds,
			       seconds > 0 ? (double)written / 1e6 / seconds : 0.0,
			       (long long)now_free);
			fflush(stdout);
			next_progress += RAIJU_PROGRESS_BYTES;

			if (now_free >= 0 &&
			    (uint64_t)now_free < RAIJU_FREE_RESERVE_BYTES) {
				printf("stopped: %s is nearly full\n",
				       RAIJU_DATA_ROOT);
				goto done;
			}
		}
	}

	if (code != ARCHIVE_EOF) {
		printf("read: %s\n", archive_error_string(reader));
		goto done;
	}
	status = 0;

done:
	if (!described) {
		print_decoders(reader);
	}

	{
		double seconds = elapsed_seconds(&started);

		printf("entries: %llu\n", (unsigned long long)entries);
		printf("written: %llu bytes\n", (unsigned long long)written);
		printf("seconds: %.2f\n", seconds);
		printf("rate: %.2f MB/s\n",
		       seconds > 0 ? (double)written / 1e6 / seconds : 0.0);
		printf("free after: %lld bytes\n",
		       (long long)free_bytes(RAIJU_DATA_ROOT));
		printf("status: %s\n", status == 0 ? "ok" : "failed");
	}
	fflush(stdout);

	archive_read_close(reader);
	archive_read_free(reader);

	return status;
}

#ifdef RAIJU_BEFORE_ONLY

/** Reports free space and whether the sample is staged, extracting nothing. */
static int before_only(const char *archive_path) {
	struct stat info;

	printf("mode: before only, nothing is downloaded or extracted\n");
	if (stat(archive_path, &info) == 0) {
		printf("staged: %s, %lld bytes\n", archive_path,
		       (long long)info.st_size);
	} else {
		printf("staged: %s is absent\n", archive_path);
	}
	printf("free: %lld bytes\n", (long long)free_bytes(RAIJU_DATA_ROOT));
	fflush(stdout);

	return 0;
}

#endif

/**
 * Fetches one archive onto the console, then extracts it to measure how fast
 * libarchive writes the format out on the console's own storage.
 */
int main(void) {
	int status = 0;

	printf("raiju libarchive spike %s, firmware %08x\n", RAIJU_VERSION,
	       kernel_get_fw_version());
	printf("libarchive: %s\n", archive_version_details());
	printf("sample: %s\n", RAIJU_SAMPLE_NAME);
	fflush(stdout);

	make_directories(RAIJU_SPIKE_DIR);

#ifdef RAIJU_BEFORE_ONLY
	status = before_only(RAIJU_ARCHIVE_PATH);
	printf("status: %s\n", status == 0 ? "ok" : "failed");
	fflush(stdout);
	notify("Raiju libarchive", "before-only run finished");
	payload_exit(status);

	return status;
#else
	char title[64];
	char detail[128];

	{
		struct stat info;

		if (stat(RAIJU_ARCHIVE_PATH, &info) == 0) {
			printf("cached: %s, %lld bytes\n", RAIJU_ARCHIVE_PATH,
			       (long long)info.st_size);
		} else if (download(RAIJU_ARCHIVE_PATH) != 0) {
			printf("status: failed\n");
			fflush(stdout);
			notify("Raiju libarchive", "download failed");
			payload_exit(1);

			return 1;
		}
	}

	status = extract(RAIJU_ARCHIVE_PATH);

	snprintf(title, sizeof(title), "Raiju libarchive %s", RAIJU_VERSION);
	snprintf(detail, sizeof(detail), "%s extract %s", RAIJU_SAMPLE_NAME,
		 status == 0 ? "ok" : "failed");
	notify(title, detail);

	remove_tree(RAIJU_EXTRACT_ROOT);

	payload_exit(status);

	return status;
#endif
}
