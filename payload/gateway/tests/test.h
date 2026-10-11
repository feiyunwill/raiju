#ifndef RAIJU_GATEWAY_TEST_H
#define RAIJU_GATEWAY_TEST_H

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

#define CHECK_INT(actual, expected)                                                                          \
	do {                                                                                                     \
		long long check_actual_ = (long long)(actual);                                                       \
		long long check_expected_ = (long long)(expected);                                                   \
		raiju_test_checks++;                                                                                 \
		if (check_actual_ != check_expected_) {                                                              \
			raiju_test_failures++;                                                                           \
			fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #actual, check_actual_, \
			        check_expected_);                                                                        \
		}                                                                                                    \
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

#define RUN_TEST(test)           \
	do {                         \
		raiju_test_count++;      \
		printf("  %s\n", #test); \
		test();                  \
	} while (0)

/**
 * Runs the supervisor directory creation and single-instance guard tests.
 */
void run_supervisor_tests(void);

/**
 * Runs the engine options rendering and secret persistence tests.
 */
void run_engine_options_tests(void);

/**
 * Runs the pairing code, rate limit and device token tests.
 */
void run_pairing_tests(void);

/**
 * Runs the embedded asset lookup and MIME tests.
 */
void run_assets_tests(void);

/**
 * Runs the allowlist path resolution tests.
 */
void run_allowlist_tests(void);

/**
 * Runs the proxy secret injection and URL parsing tests.
 */
void run_proxy_tests(void);

/**
 * Runs the routing, auth gate and static serving tests.
 */
void run_router_tests(void);

/**
 * Runs the outbound HTTP(S) client tests against a local fixture server.
 */
void run_fetch_tests(void);

/**
 * Runs the artwork parsing, cache and endpoint tests.
 */
void run_art_tests(void);

/**
 * Runs the /api status, storage, folders and jobs tests.
 */
void run_api_tests(void);

/**
 * Runs the probe tests against a local fixture server.
 */
void run_probe_tests(void);

/**
 * Runs the real-HTTPS tests against a local TLS server. Compiled to a no-op
 * when the build has no OpenSSL (RAIJU_FETCH_TLS undefined).
 */
void run_tls_tests(void);

#endif
