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
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#include "image_builder.h"

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_STAGING RAIJU_DATA_ROOT "/staging"
#define RAIJU_INSTALL_ROOT "/data/homebrew"
#define RAIJU_SCAN_CONFIG "/data/shadowmount/config.ini"
#define RAIJU_SMP_PORT 10101
#define RAIJU_TITLE_ID "PPSA99999"
#define RAIJU_DEPTH_ID "PPSA99998"
#define RAIJU_DEPTH_DIR RAIJU_INSTALL_ROOT "/raiju-depth-probe"
#define RAIJU_IMAGE_NAME RAIJU_TITLE_ID ".exfat"
#define RAIJU_DEPTH_NAME RAIJU_DEPTH_ID ".exfat"
#define RAIJU_STAGED_IMAGE RAIJU_STAGING "/" RAIJU_IMAGE_NAME
#define RAIJU_INSTALLED_IMAGE RAIJU_INSTALL_ROOT "/" RAIJU_IMAGE_NAME
#define RAIJU_DEPTH_IMAGE RAIJU_DEPTH_DIR "/" RAIJU_DEPTH_NAME
#define RAIJU_CLUSTER_SHIFT 7
#define RAIJU_CLUSTER_COUNT 64
#define RAIJU_EXFAT_ENTRY 32
#define RAIJU_WAIT_ATTEMPTS 4
#define RAIJU_WAIT_SECONDS 15

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

/** Sends one HTTP request to whatever answers the ShadowMountPlus port and reads the reply. */
static int smp_request(const char *method, const char *path, const char *body,
		       char *out, size_t out_size) {
	struct sockaddr_in address;
	struct timeval timeout = {5, 0};
	char request[512];
	size_t length;
	size_t total = 0;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		snprintf(out, out_size, "socket: %s", strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(RAIJU_SMP_PORT);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		snprintf(out, out_size, "connect: %s", strerror(errno));
		close(fd);
		return -1;
	}

	length = (size_t)snprintf(request, sizeof(request),
				  "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
				  "Content-Length: %zu\r\n\r\n%s",
				  method, path, body != NULL ? strlen(body) : 0,
				  body != NULL ? body : "");
	if (write(fd, request, length) != (ssize_t)length) {
		snprintf(out, out_size, "write: %s", strerror(errno));
		close(fd);
		return -1;
	}

	while (total + 1 < out_size) {
		ssize_t got = read(fd, out + total, out_size - 1 - total);

		if (got <= 0) {
			break;
		}
		total += (size_t)got;
	}
	out[total] = '\0';
	close(fd);

	return 0;
}

/** Prints the status line and body of a response, with the body on one line. */
static void print_response(const char *label, const char *response) {
	const char *body = strstr(response, "\r\n\r\n");
	char flat[512];
	size_t written = 0;

	if (body != NULL) {
		body += 4;
	} else {
		body = response;
	}
	for (const char *at = body; *at != '\0' && written + 1 < sizeof(flat); at++) {
		if (*at != '\n' && *at != '\r') {
			flat[written++] = *at;
		}
	}
	flat[written] = '\0';
	printf("%s: %s\n", label, flat);
}

/** Prints the scan settings ShadowMountPlus reads, and which paths it is told to mount. */
static void report_scan_config(void) {
	char line[256];
	FILE *config = fopen(RAIJU_SCAN_CONFIG, "r");

	if (config == NULL) {
		printf("config: %s is absent (%s)\n", RAIJU_SCAN_CONFIG,
		       strerror(errno));
		return;
	}
	while (fgets(line, sizeof(line), config) != NULL) {
		if (strstr(line, "scan") == NULL && strstr(line, "backport") == NULL) {
			continue;
		}
		line[strcspn(line, "\r\n")] = '\0';
		printf("config: %s\n", line);
	}
	fclose(config);
}

/** Builds an exFAT game image with 64 KiB clusters, the size ShadowMountPlus recommends. */
static builder_exfat *build_game_image(const char *json) {
	size_t json_length = strlen(json);
	builder_exfat *volume = builder_exfat_create(RAIJU_CLUSTER_SHIFT, RAIJU_CLUSTER_COUNT);
	uint32_t root;
	uint32_t eboot;
	uint32_t sce_sys;
	uint32_t sce_clusters[2];
	uint32_t icon;
	uint32_t clusters;
	uint32_t first = 0;
	uint32_t previous = 0;
	size_t offset;
	size_t sce_offset;
	size_t param_offset;
	builder_exfat_entry eboot_entry;
	builder_exfat_entry dir_entry;
	builder_exfat_entry icon_entry;
	builder_exfat_entry param_entry;

	if (volume == NULL) {
		return NULL;
	}
	root = volume->root_cluster;

	eboot = builder_exfat_alloc(volume, 1, true);
	if (eboot == 0) {
		builder_exfat_free(volume);
		return NULL;
	}
	memcpy(builder_exfat_cluster(volume, eboot), "\177ELF", 4);
	eboot_entry = (builder_exfat_entry){"eboot.bin", 0x20, BUILDER_EXFAT_ALLOCATED, eboot, 4, 4};
	offset = builder_exfat_write_entry(volume, &root, 1, 2 * RAIJU_EXFAT_ENTRY, &eboot_entry);

	sce_sys = builder_exfat_alloc(volume, 2, false);
	sce_clusters[0] = sce_sys;
	sce_clusters[1] = sce_sys + 1;
	dir_entry = (builder_exfat_entry){"sce_sys", BUILDER_EXFAT_ATTR_DIRECTORY, BUILDER_EXFAT_NO_FAT_CHAIN,
	                                  sce_sys, 2u * volume->cluster_size, 2u * volume->cluster_size};
	builder_exfat_write_entry(volume, &root, 1, offset, &dir_entry);

	icon = builder_exfat_alloc(volume, 1, true);
	icon_entry = (builder_exfat_entry){"icon0.png", 0x20, BUILDER_EXFAT_ALLOCATED, icon, 8, 8};
	sce_offset = builder_exfat_write_entry(volume, sce_clusters, 2, 0, &icon_entry);
	param_offset = volume->cluster_size - 2 * RAIJU_EXFAT_ENTRY;
	for (; sce_offset < param_offset; sce_offset += RAIJU_EXFAT_ENTRY) {
		builder_exfat_cluster(volume, sce_sys)[sce_offset] = 0x05;
	}

	clusters = (uint32_t)((json_length + volume->cluster_size - 1) / volume->cluster_size);
	for (uint32_t i = 0; i < clusters; i++) {
		uint32_t cluster = builder_exfat_alloc(volume, 1, false);
		size_t start = (size_t)i * volume->cluster_size;
		size_t chunk = json_length - start < volume->cluster_size ? json_length - start : volume->cluster_size;

		builder_exfat_alloc(volume, 1, false);
		if (cluster == 0) {
			builder_exfat_free(volume);
			return NULL;
		}
		memcpy(builder_exfat_cluster(volume, cluster), json + start, chunk);
		if (previous != 0) {
			builder_exfat_set_fat(volume, previous, cluster);
		} else {
			first = cluster;
		}
		previous = cluster;
	}
	if (previous != 0) {
		builder_exfat_set_fat(volume, previous, BUILDER_EXFAT_END_OF_CHAIN);
	}
	param_entry = (builder_exfat_entry){"param.json", 0x20, BUILDER_EXFAT_ALLOCATED, first,
	                                    json_length, json_length};
	builder_exfat_write_entry(volume, sce_clusters, 2, param_offset, &param_entry);

	return volume;
}

/** Writes the in-memory image to a path, flushing it before returning. */
static int write_image(const builder_exfat *volume, const char *path) {
	ssize_t written;
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);

	if (fd < 0) {
		printf("image: cannot create %s: %s\n", path, strerror(errno));
		return -1;
	}
	written = write(fd, volume->data, volume->size);
	if (written != (ssize_t)volume->size) {
		printf("image: wrote %zd of %zu bytes: %s\n", written, volume->size,
		       strerror(errno));
		close(fd);
		return -1;
	}
	fsync(fd);
	close(fd);

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
			       i * RAIJU_WAIT_SECONDS);
			return 1;
		}
		printf("%s: %s not mounted yet, waiting %d s\n", label, needle,
		       RAIJU_WAIT_SECONDS);
		sleep(RAIJU_WAIT_SECONDS);
	}
	printf("%s: %s did not mount within %d s\n", label, needle,
	       attempts * RAIJU_WAIT_SECONDS);

	return 0;
}

/**
 * Measures the ShadowMountPlus half of the atomic install without depending on any API: the config
 * it reads is printed, an image built in memory with the recommended 64 KiB clusters is staged
 * under `/data/raiju` and renamed into the install root, and the mount table is watched through
 * ShadowMountPlus's own scan interval for the title. A second image inside a subfolder tests
 * `scan_depth=1`. Both carry title IDs PPSA99999 and PPSA99998 so they cannot be mistaken for a
 * real dump, and both are removed again so the console is left as it was found.
 */
int main(void) {
	static const char param_json[] =
		"{\"titleId\":\"" RAIJU_TITLE_ID "\",\"localizedParameters\":"
		"{\"en-US\":{\"titleName\":\"Raiju spike\"}}}";
	char response[4096];
	builder_exfat *volume;
	uint64_t size_bytes;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("raiju smp spike %s\n", RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());
	report_scan_config();

	if (smp_request("GET", "/api/v1/images", NULL, response, sizeof(response)) == 0) {
		print_response("port 10101", response);
	} else {
		printf("port 10101: %s\n", response);
	}

	if (stat(RAIJU_INSTALL_ROOT, &(struct stat){0}) != 0) {
		printf("install root %s is absent (%s)\n", RAIJU_INSTALL_ROOT,
		       strerror(errno));
		payload_exit(1);
		return 1;
	}

	volume = build_game_image(param_json);
	if (volume == NULL) {
		printf("image: the builder returned nothing\n");
		payload_exit(1);
		return 1;
	}
	size_bytes = (uint64_t)volume->size;
	printf("image: %llu bytes, %u byte clusters\n",
	       (unsigned long long)size_bytes, volume->cluster_size);

	unlink(RAIJU_STAGED_IMAGE);
	unlink(RAIJU_INSTALLED_IMAGE);
	if (write_image(volume, RAIJU_STAGED_IMAGE) != 0) {
		builder_exfat_free(volume);
		payload_exit(1);
		return 1;
	}
	printf("image: staged at %s\n", RAIJU_STAGED_IMAGE);
	if (rename(RAIJU_STAGED_IMAGE, RAIJU_INSTALLED_IMAGE) != 0) {
		printf("image: rename into the install root failed: %s\n",
		       strerror(errno));
		builder_exfat_free(volume);
		payload_exit(1);
		return 1;
	}
	printf("image: renamed to %s\n", RAIJU_INSTALLED_IMAGE);

	wait_for_mount("installed", RAIJU_TITLE_ID, RAIJU_WAIT_ATTEMPTS);

	if (mkdir(RAIJU_DEPTH_DIR, 0755) != 0 && errno != EEXIST) {
		printf("depth: cannot create %s: %s\n", RAIJU_DEPTH_DIR,
		       strerror(errno));
	} else if (write_image(volume, RAIJU_DEPTH_IMAGE) != 0) {
		printf("depth: image not written\n");
	} else {
		printf("depth: image written to %s\n", RAIJU_DEPTH_IMAGE);
		wait_for_mount("subfolder", RAIJU_DEPTH_ID, RAIJU_WAIT_ATTEMPTS);
	}

	unlink(RAIJU_INSTALLED_IMAGE);
	unlink(RAIJU_DEPTH_IMAGE);
	rmdir(RAIJU_DEPTH_DIR);
	printf("cleanup: probe images removed\n");
	if (wait_for_mount("cleanup", RAIJU_TITLE_ID, 2) == 1) {
		printf("cleanup: the title is still mounted, ShadowMountPlus still holds it\n");
	} else {
		printf("cleanup: no mount for the title, the console is as it was found\n");
	}

	builder_exfat_free(volume);
	notify("Raiju spike", "the ShadowMountPlus probe finished");
	printf("exit\n");
	payload_exit(0);

	return 0;
}
