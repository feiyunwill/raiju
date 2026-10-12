#define _POSIX_C_SOURCE 200809L

#include "platform_ps5.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>

#include <ps5/kernel.h>

#include "engine_ps5.h"

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_CA_PATH RAIJU_DATA_ROOT "/ca.pem"
#define RAIJU_NOTIFY_MESSAGE_MAX 512

int sceNotificationSend(int userId, bool isLogged, const char *payload);

static const char toast_template[] =
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
	"      \"subMessage\": {\"body\": \"Raiju\"}"
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

/**
 * Copies text into out with the characters JSON strings reserve escaped, so a
 * message carrying a job's display name cannot break out of the toast
 * document. Returns false when the escaped text does not fit.
 */
static bool json_escape(const char *text, char *out, size_t capacity) {
	size_t written = 0;

	for (const char *cursor = text; *cursor != '\0'; cursor++) {
		unsigned char byte = (unsigned char)*cursor;
		char codepoint[8];
		const char *escape = NULL;
		size_t length;

		switch (byte) {
			case '"':
				escape = "\\\"";
				break;
			case '\\':
				escape = "\\\\";
				break;
			case '\n':
				escape = "\\n";
				break;
			case '\r':
				escape = "\\r";
				break;
			case '\t':
				escape = "\\t";
				break;
			default:
				if (byte < 0x20) {
					snprintf(codepoint, sizeof codepoint, "\\u%04x", byte);
					escape = codepoint;
				}
				break;
		}
		if (escape == NULL) {
			if (written + 2 > capacity) {
				return false;
			}
			out[written++] = (char)byte;
			continue;
		}
		length = strlen(escape);
		if (written + length + 1 > capacity) {
			return false;
		}
		memcpy(out + written, escape, length);
		written += length;
	}
	out[written] = '\0';
	return true;
}

/** Raises one toast and reports the notification service's result. */
static int ps5_notify(void *ctx, const char *message) {
	char text[RAIJU_NOTIFY_MESSAGE_MAX];
	char escaped[RAIJU_NOTIFY_MESSAGE_MAX * 2];
	char payload[RAIJU_NOTIFY_MESSAGE_MAX * 4];
	int result;

	(void)ctx;
	snprintf(text, sizeof text, "%s", message);
	if (!json_escape(text, escaped, sizeof escaped)) {
		return -1;
	}
	if (snprintf(payload, sizeof payload, toast_template, escaped, escaped) < 0) {
		return -1;
	}
	result = sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true, payload);
	printf("notify: %d for %s\n", result, text);
	fflush(stdout);
	return result == 0 ? 0 : -1;
}

/** Runs in the context the loader grants and escalates nothing (ADR 0006). */
static int ps5_escalate(void *ctx) {
	(void)ctx;
	return 0;
}

/** Reports the filesystem holding path through the console's statfs. */
static int ps5_disk_usage(void *ctx, const char *path, raiju_disk_usage *out) {
	struct statfs info;

	(void)ctx;
	if (statfs(path, &info) != 0) {
		return -1;
	}
	out->total_bytes = (uint64_t)info.f_blocks * (uint64_t)info.f_bsize;
	out->free_bytes = (uint64_t)info.f_bavail * (uint64_t)info.f_bsize;
	return 0;
}

/** Reports the console's LAN address, skipping loopback and link-local ones. */
static int ps5_interface_ip(void *ctx, char *buffer, size_t capacity) {
	struct ifaddrs *addresses = NULL;
	int found = -1;

	(void)ctx;
	if (capacity > 0) {
		buffer[0] = '\0';
	}
	if (getifaddrs(&addresses) != 0) {
		return -1;
	}
	for (struct ifaddrs *entry = addresses; entry != NULL; entry = entry->ifa_next) {
		const struct sockaddr_in *address;
		uint32_t host;

		if (entry->ifa_addr == NULL || entry->ifa_addr->sa_family != AF_INET) {
			continue;
		}
		address = (const struct sockaddr_in *)entry->ifa_addr;
		host = ntohl(address->sin_addr.s_addr);
		if ((host >> 24) == 127 || (host >> 16) == 0xA9FE) {
			continue;
		}
		if (inet_ntop(AF_INET, &address->sin_addr, buffer, capacity) == NULL) {
			continue;
		}
		found = 0;
		break;
	}
	freeifaddrs(addresses);
	if (found != 0) {
		snprintf(buffer, capacity, "127.0.0.1");
	}
	return 0;
}

/** Reports the kernel's firmware version, e.g. `13.60` for 0x13600007. */
static int ps5_firmware(void *ctx, char *buffer, size_t capacity) {
	uint32_t version = kernel_get_fw_version();

	(void)ctx;
	snprintf(buffer, capacity, "%x.%02x", (version >> 24) & 0xFF, (version >> 16) & 0xFF);
	return 0;
}

/** Starts the embedded engine with the options file; on this platform the engine is us. */
static int ps5_launch_engine(void *ctx, const char *executable, const char *options_file) {
	(void)ctx;
	(void)executable;
	return raiju_engine_start(options_file);
}

/** Fills the buffer from the libc's CSPRNG, which the console seeds from the kernel. */
static int ps5_random_bytes(void *ctx, void *buffer, size_t length) {
	(void)ctx;
	arc4random_buf(buffer, length);
	return 0;
}

/** Names the CA bundle the console build installs at the data root. */
static int ps5_ca_bundle_path(void *ctx, char *buffer, size_t capacity) {
	(void)ctx;
	if (snprintf(buffer, capacity, "%s", RAIJU_CA_PATH) >= (int)capacity) {
		if (capacity > 0) {
			buffer[0] = '\0';
		}
		return -1;
	}
	return 0;
}

const raiju_platform *raiju_platform_ps5(void) {
	static const raiju_platform console = {
	    ps5_notify,   ps5_escalate,      ps5_disk_usage,    ps5_interface_ip,
	    ps5_firmware, ps5_launch_engine, ps5_random_bytes,  ps5_ca_bundle_path, NULL,
	};
	return &console;
}
