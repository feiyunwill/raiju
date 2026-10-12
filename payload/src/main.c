#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ps5/payload.h>

#include "engine_ps5.h"
#include "platform_ps5.h"
#include "raiju/gateway.h"
#include "raiju/supervisor.h"

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_PORT 2997
#define RAIJU_ARIA2_URL "http://127.0.0.1:6800/jsonrpc"
#define RAIJU_CA_PATH RAIJU_DATA_ROOT "/ca.pem"

extern const unsigned char raiju_ca_bundle[];
extern const unsigned long raiju_ca_bundle_len;

/** Installs the CA bundle the artwork lookup and the probe verify against, once. */
static void install_ca_bundle(void) {
	size_t written = 0;
	int fd;

	if (access(RAIJU_CA_PATH, F_OK) == 0) {
		return;
	}
	fd = open(RAIJU_CA_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("ca: cannot write %s\n", RAIJU_CA_PATH);
		fflush(stdout);
		return;
	}
	while (written < (size_t)raiju_ca_bundle_len) {
		ssize_t step = write(fd, raiju_ca_bundle + written, (size_t)raiju_ca_bundle_len - written);

		if (step <= 0) {
			break;
		}
		written += (size_t)step;
	}
	close(fd);
	printf("ca: wrote %zu of %lu bytes to %s\n", written, raiju_ca_bundle_len, RAIJU_CA_PATH);
	fflush(stdout);
}

/** Boots the payload: the data root, the embedded engine, the gateway and the ready toast. */
int main(void) {
	const raiju_platform *platform = raiju_platform_ps5();
	raiju_gateway_config config;
	raiju_supervisor supervisor;
	raiju_gateway *gateway;
	raiju_supervisor_status prepared;
	char address[64];

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("raiju %s\n", RAIJU_VERSION);
	install_ca_bundle();
	prepared = raiju_supervisor_prepare(&supervisor, platform, RAIJU_DATA_ROOT);
	platform->interface_ip(platform->ctx, address, sizeof address);

	if (prepared == RAIJU_SUPERVISOR_ALREADY_RUNNING) {
		raiju_supervisor_announce_running(&supervisor, address, RAIJU_PORT);
		printf("raiju: already running on %s:%u\n", address, (unsigned)RAIJU_PORT);
		payload_exit(1);
		return 1;
	}
	if (prepared != RAIJU_SUPERVISOR_OK) {
		platform->notify(platform->ctx, "Raiju: cannot prepare " RAIJU_DATA_ROOT);
		printf("raiju: cannot prepare %s\n", RAIJU_DATA_ROOT);
		payload_exit(1);
		return 1;
	}

	memset(&config, 0, sizeof config);
	config.platform = platform;
	config.port = RAIJU_PORT;
	config.data_dir = RAIJU_DATA_ROOT;
	config.aria2_url = RAIJU_ARIA2_URL;
	config.secret = supervisor.engine.secret;
	config.platform_name = "ps5";

	if (raiju_supervisor_launch_engine(&supervisor, "libaria2") != RAIJU_SUPERVISOR_OK) {
		platform->notify(platform->ctx, "Raiju: the engine did not start");
		printf("raiju: engine did not start\n");
		raiju_supervisor_close(&supervisor);
		payload_exit(1);
		return 1;
	}

	gateway = raiju_gateway_create(&config);
	if (gateway == NULL) {
		printf("raiju: invalid gateway configuration\n");
		raiju_engine_stop();
		raiju_supervisor_close(&supervisor);
		payload_exit(1);
		return 1;
	}
	if (!raiju_gateway_start(gateway)) {
		platform->notify(platform->ctx, "Raiju: the gateway did not start");
		printf("raiju: gateway did not start\n");
		raiju_gateway_destroy(gateway);
		raiju_engine_stop();
		raiju_supervisor_close(&supervisor);
		payload_exit(1);
		return 1;
	}

	printf("raiju: listening on %s:%u code %s\n", address, (unsigned)gateway->bound_port,
	       raiju_gateway_code(gateway));
	fflush(stdout);
	raiju_supervisor_announce(&supervisor, address, gateway->bound_port, raiju_gateway_code(gateway));

	while (true) {
		sleep(1);
	}

	raiju_gateway_stop(gateway);
	raiju_gateway_destroy(gateway);
	raiju_engine_stop();
	raiju_supervisor_close(&supervisor);
	payload_exit(0);
	return 0;
}
