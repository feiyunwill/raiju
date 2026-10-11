#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "fake_platform.h"
#include "raiju/engine_options.h"
#include "raiju/fsutil.h"
#include "test.h"

static void config_generates_and_persists_secret(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "engine"), 0);
	char secret_path[1200];
	snprintf(secret_path, sizeof secret_path, "%s/aria2.secret", directory);
	raiju_fake_platform fake;
	const raiju_platform *platform = raiju_fake_platform_init(&fake);
	raiju_engine_config config;
	CHECK(raiju_engine_config_init(&config, platform, directory, secret_path));
	CHECK_INT(strlen(config.secret), RAIJU_ENGINE_SECRET_HEX);
	CHECK_INT(config.rpc_port, 6800);
	CHECK(strstr(config.downloads_dir, "/downloads") != NULL);
	CHECK(strstr(config.session_dir, "/session") != NULL);

	raiju_engine_config reloaded;
	CHECK(raiju_engine_config_init(&reloaded, platform, directory, secret_path));
	CHECK_STR(reloaded.secret, config.secret);
	raiju_test_rmtree(directory);
}

static void options_contain_the_required_settings(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "options"), 0);
	char secret_path[1200];
	snprintf(secret_path, sizeof secret_path, "%s/aria2.secret", directory);
	raiju_fake_platform fake;
	const raiju_platform *platform = raiju_fake_platform_init(&fake);
	raiju_engine_config config;
	CHECK(raiju_engine_config_init(&config, platform, directory, secret_path));
	char *rendered = raiju_engine_options_render(&config);
	CHECK(rendered != NULL);
	CHECK(strstr(rendered, "enable-rpc=true\n") != NULL);
	CHECK(strstr(rendered, "rpc-listen-all=false\n") != NULL);
	CHECK(strstr(rendered, "rpc-listen-port=6800\n") != NULL);
	CHECK(strstr(rendered, "rpc-secret=") != NULL);
	CHECK(strstr(rendered, config.secret) != NULL);
	CHECK(strstr(rendered, "force-save=true\n") != NULL);
	CHECK(strstr(rendered, "save-session-interval=10\n") != NULL);
	CHECK(strstr(rendered, "save-session=") != NULL);
	CHECK(strstr(rendered, "input-file=") != NULL);
	CHECK(strstr(rendered, "max-connection-per-server=16\n") != NULL);
	free(rendered);

	char options_path[1200];
	snprintf(options_path, sizeof options_path, "%s/aria2.conf", directory);
	CHECK(raiju_engine_options_write(&config, options_path));
	size_t length = 0;
	char *loaded = raiju_read_file(options_path, 65536, &length);
	CHECK(loaded != NULL && length > 0);
	free(loaded);
	raiju_test_rmtree(directory);
}

void run_engine_options_tests(void) {
	RUN_TEST(config_generates_and_persists_secret);
	RUN_TEST(options_contain_the_required_settings);
}
