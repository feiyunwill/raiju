#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fake_platform.h"
#include "raiju/supervisor.h"
#include "test.h"

static bool is_dir(const char *path) {
	struct stat info;
	return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool is_file(const char *path) {
	struct stat info;
	return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static void prepare_creates_directories(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "supervisor"), 0);
	raiju_fake_platform fake;
	const raiju_platform *platform = raiju_fake_platform_init(&fake);
	raiju_supervisor supervisor;
	CHECK_INT(raiju_supervisor_prepare(&supervisor, platform, directory), RAIJU_SUPERVISOR_OK);
	CHECK(is_dir(supervisor.downloads_dir));
	CHECK(is_dir(supervisor.staging_dir));
	CHECK(is_dir(supervisor.trash_dir));
	CHECK(is_dir(supervisor.session_dir));
	CHECK(is_dir(supervisor.www_dir));
	CHECK(is_file(supervisor.options_path));
	CHECK_INT(strlen(supervisor.engine.secret), RAIJU_ENGINE_SECRET_HEX);
	raiju_supervisor_close(&supervisor);
	raiju_test_rmtree(directory);
}

static void single_instance_guard(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "single"), 0);
	raiju_fake_platform fake;
	const raiju_platform *platform = raiju_fake_platform_init(&fake);
	raiju_supervisor first;
	CHECK_INT(raiju_supervisor_prepare(&first, platform, directory), RAIJU_SUPERVISOR_OK);
	raiju_supervisor second;
	CHECK_INT(raiju_supervisor_prepare(&second, platform, directory), RAIJU_SUPERVISOR_ALREADY_RUNNING);
	raiju_supervisor_close(&first);
	raiju_supervisor third;
	CHECK_INT(raiju_supervisor_prepare(&third, platform, directory), RAIJU_SUPERVISOR_OK);
	raiju_supervisor_close(&third);
	raiju_test_rmtree(directory);
}

static void announce_and_launch_use_the_platform(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "announce"), 0);
	raiju_fake_platform fake;
	const raiju_platform *platform = raiju_fake_platform_init(&fake);
	raiju_supervisor supervisor;
	CHECK_INT(raiju_supervisor_prepare(&supervisor, platform, directory), RAIJU_SUPERVISOR_OK);
	CHECK_INT(raiju_supervisor_announce(&supervisor, "10.0.0.5", 8080, "4821"), RAIJU_SUPERVISOR_OK);
	CHECK(strstr(fake.notification, "http://10.0.0.5:8080") != NULL);
	CHECK(strstr(fake.notification, "code 4821") != NULL);
	CHECK_INT(raiju_supervisor_launch_engine(&supervisor, "aria2c"), RAIJU_SUPERVISOR_OK);
	CHECK_INT(fake.launch_count, 1);
	CHECK_STR(fake.launched_executable, "aria2c");
	raiju_supervisor_close(&supervisor);
	raiju_test_rmtree(directory);
}

void run_supervisor_tests(void) {
	RUN_TEST(prepare_creates_directories);
	RUN_TEST(single_instance_guard);
	RUN_TEST(announce_and_launch_use_the_platform);
}
