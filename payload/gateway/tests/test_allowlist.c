#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fake_platform.h"
#include "raiju/allowlist.h"
#include "test.h"

static void rejects_relative_and_dotdot(void) {
	raiju_allowlist list = raiju_allowlist_default();
	CHECK_INT(raiju_allowlist_check(&list, "relative/path", NULL, 0), RAIJU_ALLOW_NOT_ABSOLUTE);
	CHECK_INT(raiju_allowlist_check(&list, "/data/raiju/../secret", NULL, 0), RAIJU_ALLOW_HAS_DOTDOT);
	CHECK_INT(raiju_allowlist_check(&list, "/etc/passwd", NULL, 0), RAIJU_ALLOW_OUTSIDE_ROOTS);
	CHECK_INT(raiju_allowlist_check(&list, "/data/raiju/nope", NULL, 0), RAIJU_ALLOW_RESOLVE_FAILED);
}

static void accepts_paths_inside_a_root(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "allowlist"), 0);
	char root[1200];
	char file[1400];
	snprintf(root, sizeof root, "%s/root", directory);
	snprintf(file, sizeof file, "%s/root/game.elf", directory);
	char command[1500];
	snprintf(command, sizeof command, "%s/root", directory);
	mkdir(root, 0755);
	FILE *handle = fopen(file, "wb");
	CHECK(handle != NULL);
	if (handle != NULL) {
		fputs("data", handle);
		fclose(handle);
	}
	raiju_allowlist list;
	memset(&list, 0, sizeof list);
	CHECK(raiju_allowlist_add(&list, root));
	char resolved[2048];
	CHECK_INT(raiju_allowlist_check(&list, file, resolved, sizeof resolved), RAIJU_ALLOW_OK);
	CHECK_INT(raiju_allowlist_check(&list, "/etc/passwd", NULL, 0), RAIJU_ALLOW_OUTSIDE_ROOTS);
	raiju_test_rmtree(directory);
}

static void rejects_symlink_escape(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "allowlist-symlink"), 0);
	char root[1200];
	char link[1400];
	snprintf(root, sizeof root, "%s/root", directory);
	snprintf(link, sizeof link, "%s/root/escape", directory);
	mkdir(root, 0755);
	unlink(link);
	if (symlink("/etc", link) != 0) {
		raiju_test_rmtree(directory);
		return;
	}
	raiju_allowlist list;
	memset(&list, 0, sizeof list);
	CHECK(raiju_allowlist_add(&list, root));
	char target[1400];
	snprintf(target, sizeof target, "%s/root/escape/passwd", directory);
	CHECK_INT(raiju_allowlist_check(&list, target, NULL, 0), RAIJU_ALLOW_OUTSIDE_ROOTS);
	raiju_test_rmtree(directory);
}

void run_allowlist_tests(void) {
	RUN_TEST(rejects_relative_and_dotdot);
	RUN_TEST(accepts_paths_inside_a_root);
	RUN_TEST(rejects_symlink_escape);
}
