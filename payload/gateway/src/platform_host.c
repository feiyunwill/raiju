#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "raiju/fsutil.h"

static int host_notify(void *ctx, const char *message) {
	(void)ctx;
	fprintf(stderr, "notification: %s\n", message);
	return 0;
}

static int host_escalate(void *ctx) {
	(void)ctx;
	return 0;
}

static int host_disk_usage(void *ctx, const char *path, raiju_disk_usage *out) {
	(void)ctx;
	struct statvfs info;
	if (statvfs(path, &info) != 0) {
		return -1;
	}
	out->total_bytes = (uint64_t)info.f_blocks * info.f_frsize;
	out->free_bytes = (uint64_t)info.f_bavail * info.f_frsize;
	return 0;
}

static int host_interface_ip(void *ctx, char *buffer, size_t capacity) {
	(void)ctx;
	struct ifaddrs *addresses = NULL;
	if (getifaddrs(&addresses) != 0) {
		return -1;
	}
	int found = -1;
	for (struct ifaddrs *entry = addresses; entry != NULL; entry = entry->ifa_next) {
		if (entry->ifa_addr == NULL || entry->ifa_addr->sa_family != AF_INET) {
			continue;
		}
		const struct sockaddr_in *address = (const struct sockaddr_in *)entry->ifa_addr;
		uint32_t host = ntohl(address->sin_addr.s_addr);
		if ((host >> 24) == 127) {
			continue;
		}
		if (inet_ntop(AF_INET, &address->sin_addr, buffer, (socklen_t)capacity) == NULL) {
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

static int host_firmware(void *ctx, char *buffer, size_t capacity) {
	(void)ctx;
	snprintf(buffer, capacity, "unknown");
	return 0;
}

static int host_launch_engine(void *ctx, const char *executable, const char *options_file) {
	(void)ctx;
	pid_t pid = fork();
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		execlp(executable, executable, "--conf-path", options_file, (char *)NULL);
		_exit(127);
	}
	return 0;
}

static int host_ca_bundle_path(void *ctx, char *buffer, size_t capacity) {
	(void)ctx;
	if (capacity > 0) {
		buffer[0] = '\0';
	}
	return 0;
}

static int host_random_bytes(void *ctx, void *buffer, size_t length) {
	(void)ctx;
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	unsigned char *out = buffer;
	size_t done = 0;
	while (done < length) {
		ssize_t got = read(fd, out + done, length - done);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			close(fd);
			return -1;
		}
		done += (size_t)got;
	}
	close(fd);
	return 0;
}

const raiju_platform *raiju_platform_host(void) {
	static const raiju_platform host = {
	    host_notify,   host_escalate,      host_disk_usage,     host_interface_ip,
	    host_firmware, host_launch_engine, host_random_bytes, host_ca_bundle_path, NULL,
	};
	return &host;
}
