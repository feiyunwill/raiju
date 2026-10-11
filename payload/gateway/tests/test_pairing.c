#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_platform.h"
#include "raiju/pairing.h"
#include "test.h"

static bool is_four_digits(const char *code) {
	if (strlen(code) != RAIJU_PAIR_CODE_DIGITS) {
		return false;
	}
	for (size_t i = 0; i < RAIJU_PAIR_CODE_DIGITS; i++) {
		if (code[i] < '0' || code[i] > '9') {
			return false;
		}
	}
	return true;
}

static void code_is_four_digits(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "pairing-code"), 0);
	char path[1200];
	snprintf(path, sizeof path, "%s/devices.json", directory);
	raiju_fake_platform fake;
	const raiju_platform *platform = raiju_fake_platform_init(&fake);
	raiju_pairing pairing;
	CHECK(raiju_pairing_init(&pairing, platform, path));
	CHECK(is_four_digits(pairing.code));
	CHECK(raiju_pairing_new_code(&pairing));
	CHECK(is_four_digits(pairing.code));
	raiju_test_rmtree(directory);
}

static void verify_right_and_wrong_code(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "pairing-verify"), 0);
	char path[1200];
	snprintf(path, sizeof path, "%s/devices.json", directory);
	raiju_fake_platform fake;
	const raiju_platform *platform = raiju_fake_platform_init(&fake);
	raiju_pairing pairing;
	CHECK(raiju_pairing_init(&pairing, platform, path));
	CHECK_INT(raiju_pairing_verify(&pairing, pairing.code, 1000), RAIJU_PAIR_OK);
	char bad[8];
	snprintf(bad, sizeof bad, "%04u", (unsigned)((strtoul(pairing.code, NULL, 10) + 1) % 10000));
	CHECK_INT(raiju_pairing_verify(&pairing, bad, 1000), RAIJU_PAIR_WRONG_CODE);
	CHECK_INT(raiju_pairing_verify(&pairing, pairing.code, 1000), RAIJU_PAIR_OK);
	raiju_test_rmtree(directory);
}

static void rate_limit_backs_off(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "pairing-ratelimit"), 0);
	char path[1200];
	snprintf(path, sizeof path, "%s/devices.json", directory);
	raiju_fake_platform fake;
	const raiju_platform *platform = raiju_fake_platform_init(&fake);
	raiju_pairing pairing;
	CHECK(raiju_pairing_init(&pairing, platform, path));
	const char *wrong[] = {"0000", "0001", "0002"};
	for (unsigned i = 0; i < RAIJU_PAIR_FREE_ATTEMPTS; i++) {
		CHECK_INT(raiju_pairing_verify(&pairing, wrong[i % 3], 1000), RAIJU_PAIR_WRONG_CODE);
	}
	CHECK_INT(raiju_pairing_verify(&pairing, "1111", 1000), RAIJU_PAIR_WRONG_CODE);
	CHECK_INT(raiju_pairing_verify(&pairing, pairing.code, 1001), RAIJU_PAIR_RATE_LIMITED);
	raiju_test_rmtree(directory);
}

static void issue_list_revoke_and_persist(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "pairing-devices"), 0);
	char path[1200];
	snprintf(path, sizeof path, "%s/devices.json", directory);
	raiju_fake_platform fake;
	const raiju_platform *platform = raiju_fake_platform_init(&fake);
	raiju_pairing pairing;
	CHECK(raiju_pairing_init(&pairing, platform, path));
	raiju_device device;
	CHECK_INT(raiju_pairing_issue(&pairing, "laptop", &device), RAIJU_PAIR_OK);
	CHECK_INT(strlen(device.token), RAIJU_PAIR_TOKEN_HEX);
	CHECK(strlen(device.id) > 0);
	CHECK(raiju_pairing_find(&pairing, device.token) != NULL);
	CHECK(raiju_pairing_find(&pairing, "deadbeef") == NULL);

	raiju_pairing reloaded;
	CHECK(raiju_pairing_init(&reloaded, platform, path));
	CHECK_INT(reloaded.device_count, 1);
	CHECK(raiju_pairing_find(&reloaded, device.token) != NULL);

	char id[64];
	snprintf(id, sizeof id, "%s", device.id);
	CHECK_INT(raiju_pairing_revoke(&reloaded, id), RAIJU_PAIR_OK);
	CHECK_INT(reloaded.device_count, 0);
	CHECK_INT(raiju_pairing_revoke(&reloaded, id), RAIJU_PAIR_NOT_FOUND);
	raiju_test_rmtree(directory);
}

static void issue_refuses_an_unsafe_label(void) {
	char directory[1024];
	CHECK_INT(raiju_test_tmpdir(directory, sizeof directory, "pairing-labels"), 0);
	char path[1200];
	snprintf(path, sizeof path, "%s/devices.json", directory);
	raiju_fake_platform fake;
	const raiju_platform *platform = raiju_fake_platform_init(&fake);
	raiju_pairing pairing;
	CHECK(raiju_pairing_init(&pairing, platform, path));
	raiju_device device;
	CHECK_INT(raiju_pairing_issue(&pairing, "quote\"here", &device), RAIJU_PAIR_INVALID_LABEL);
	CHECK_INT(pairing.device_count, 0);
	CHECK_INT(raiju_pairing_issue(&pairing, "back\\slash", &device), RAIJU_PAIR_INVALID_LABEL);
	CHECK_INT(raiju_pairing_issue(&pairing, "line\nbreak", &device), RAIJU_PAIR_INVALID_LABEL);
	char long_label[RAIJU_DEVICE_LABEL_MAX + 4];
	memset(long_label, 'a', sizeof long_label);
	long_label[sizeof long_label - 1] = '\0';
	CHECK_INT(raiju_pairing_issue(&pairing, long_label, &device), RAIJU_PAIR_INVALID_LABEL);
	CHECK_INT(pairing.device_count, 0);
	CHECK_INT(raiju_pairing_issue(&pairing, "phone", &device), RAIJU_PAIR_OK);
	CHECK_INT(pairing.device_count, 1);
	CHECK_STR(device.label, "phone");
	raiju_test_rmtree(directory);
}

void run_pairing_tests(void) {
	RUN_TEST(code_is_four_digits);
	RUN_TEST(verify_right_and_wrong_code);
	RUN_TEST(rate_limit_backs_off);
	RUN_TEST(issue_list_revoke_and_persist);
	RUN_TEST(issue_refuses_an_unsafe_label);
}
