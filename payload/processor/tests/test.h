#ifndef RAIJU_TEST_H
#define RAIJU_TEST_H

#include <stdio.h>
#include <string.h>

extern int raiju_test_checks;
extern int raiju_test_failures;
extern int raiju_test_count;

#define CHECK(condition)                                                                   \
	do {                                                                                   \
		raiju_test_checks++;                                                               \
		if (!(condition)) {                                                                \
			raiju_test_failures++;                                                         \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
		}                                                                                  \
	} while (0)

#define CHECK_INT(actual, expected)                                                                         \
	do {                                                                                                    \
		long long check_actual_ = (long long)(actual);                                                      \
		long long check_expected_ = (long long)(expected);                                                  \
		raiju_test_checks++;                                                                                \
		if (check_actual_ != check_expected_) {                                                             \
			raiju_test_failures++;                                                                          \
			fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #actual, check_actual_, \
			        check_expected_);                                                                       \
		}                                                                                                   \
	} while (0)

#define CHECK_STR(actual, expected)                                                                                \
	do {                                                                                                           \
		const char *check_actual_ = (actual);                                                                      \
		const char *check_expected_ = (expected);                                                                  \
		raiju_test_checks++;                                                                                       \
		if (strcmp(check_actual_, check_expected_) != 0) {                                                         \
			raiju_test_failures++;                                                                                 \
			fprintf(stderr, "%s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #actual, check_actual_, \
			        check_expected_);                                                                              \
		}                                                                                                          \
	} while (0)

#define RUN_TEST(test)                 \
	do {                               \
		raiju_test_count++;            \
		printf("  %s\n", #test);       \
		test();                        \
	} while (0)

/**
 * Runs the magic-byte and extension classification tests.
 */
void run_classify_tests(void);

/**
 * Runs the exFAT and UFS2 structural check tests.
 */
void run_image_check_tests(void);

/**
 * Runs the end-to-end file inspection tests against temporary files.
 */
void run_inspect_tests(void);

/**
 * Runs the param.json, file name and resolution tests.
 */
void run_title_id_tests(void);

#endif
