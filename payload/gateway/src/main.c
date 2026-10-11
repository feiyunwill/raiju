#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "raiju/gateway.h"
#include "raiju/platform.h"
#include "raiju/supervisor.h"

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signal_number) {
	(void)signal_number;
	keep_running = 0;
}

static void print_usage(const char *program) {
	fprintf(stderr,
	        "usage: %s [--port N] [--bind ADDR] [--data-dir DIR] [--www-dir DIR]\n"
	        "          [--aria2-url URL] [--secret HEX] [--no-engine]\n"
	        "          [--engine CMD]\n",
	        program);
}

static const char *flag_value(int argc, char **argv, int *index, const char *name) {
	const char *argument = argv[*index];
	const char *equals = strchr(argument, '=');
	if (equals != NULL) {
		return equals + 1;
	}
	if (*index + 1 < argc) {
		(*index)++;
		return argv[*index];
	}
	fprintf(stderr, "raiju-gateway: %s needs a value\n", name);
	return NULL;
}

static bool flag_named(const char *argument, const char *name) {
	size_t length = strlen(name);
	if (strncmp(argument, name, length) != 0) {
		return false;
	}
	return argument[length] == '\0' || argument[length] == '=';
}

int main(int argc, char **argv) {
	const raiju_platform *platform = raiju_platform_host();
	raiju_gateway_config config;
	memset(&config, 0, sizeof config);
	config.platform = platform;
	config.port = 8080;
	config.data_dir = "/data/raiju";
	config.aria2_url = "http://127.0.0.1:6800/jsonrpc";
	const char *secret = NULL;
	const char *engine_command = "aria2c";
	bool run_engine = true;

	for (int i = 1; i < argc; i++) {
		const char *argument = argv[i];
		if (flag_named(argument, "--port")) {
			const char *value = flag_value(argc, argv, &i, "--port");
			if (value == NULL) {
				return 2;
			}
			config.port = (uint16_t)strtoul(value, NULL, 10);
		} else if (flag_named(argument, "--bind")) {
			config.bind_address = flag_value(argc, argv, &i, "--bind");
		} else if (flag_named(argument, "--data-dir")) {
			config.data_dir = flag_value(argc, argv, &i, "--data-dir");
		} else if (flag_named(argument, "--www-dir")) {
			config.www_dir = flag_value(argc, argv, &i, "--www-dir");
		} else if (flag_named(argument, "--aria2-url")) {
			config.aria2_url = flag_value(argc, argv, &i, "--aria2-url");
		} else if (flag_named(argument, "--secret")) {
			secret = flag_value(argc, argv, &i, "--secret");
		} else if (strcmp(argument, "--no-engine") == 0) {
			run_engine = false;
		} else if (flag_named(argument, "--engine")) {
			engine_command = flag_value(argc, argv, &i, "--engine");
		} else if (strcmp(argument, "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "raiju-gateway: unknown option %s\n", argument);
			print_usage(argv[0]);
			return 2;
		}
	}

	raiju_supervisor supervisor;
	raiju_supervisor_status prepared = raiju_supervisor_prepare(&supervisor, platform, config.data_dir);
	if (prepared == RAIJU_SUPERVISOR_ALREADY_RUNNING) {
		char address[64];
		platform->interface_ip(platform->ctx, address, sizeof address);
		raiju_supervisor_announce(&supervisor, address, config.port, "----");
		fprintf(stderr, "raiju-gateway: already running\n");
		return 1;
	}
	if (prepared != RAIJU_SUPERVISOR_OK) {
		fprintf(stderr, "raiju-gateway: could not prepare %s\n", config.data_dir);
		return 1;
	}
	if (secret == NULL) {
		secret = supervisor.engine.secret;
	}
	config.secret = secret;

	if (run_engine && raiju_supervisor_launch_engine(&supervisor, engine_command) != RAIJU_SUPERVISOR_OK) {
		fprintf(stderr, "raiju-gateway: could not launch %s\n", engine_command);
		raiju_supervisor_close(&supervisor);
		return 1;
	}

	raiju_gateway *gateway = raiju_gateway_create(&config);
	if (gateway == NULL) {
		fprintf(stderr, "raiju-gateway: invalid configuration\n");
		raiju_supervisor_close(&supervisor);
		return 1;
	}
	if (!raiju_gateway_start(gateway)) {
		fprintf(stderr, "raiju-gateway: could not start the server\n");
		raiju_gateway_destroy(gateway);
		raiju_supervisor_close(&supervisor);
		return 1;
	}
	char address[64];
	platform->interface_ip(platform->ctx, address, sizeof address);
	printf("raiju gateway listening on %s:%u code %s\n", address, (unsigned)gateway->bound_port,
	       raiju_gateway_code(gateway));
	fflush(stdout);
	raiju_supervisor_announce(&supervisor, address, gateway->bound_port, raiju_gateway_code(gateway));

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	while (keep_running) {
		sleep(1);
	}

	raiju_gateway_stop(gateway);
	raiju_gateway_destroy(gateway);
	raiju_supervisor_close(&supervisor);
	return 0;
}
