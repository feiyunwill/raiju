#include "test.h"

int raiju_test_checks = 0;
int raiju_test_failures = 0;
int raiju_test_count = 0;

int main(void) {
	printf("fsutil\n");
	run_engine_options_tests();
	printf("supervisor\n");
	run_supervisor_tests();
	printf("pairing\n");
	run_pairing_tests();
	printf("assets\n");
	run_assets_tests();
	printf("allowlist\n");
	run_allowlist_tests();
	printf("proxy\n");
	run_proxy_tests();
	printf("router\n");
	run_router_tests();
	printf("fetch\n");
	run_fetch_tests();
	printf("art\n");
	run_art_tests();
	printf("api\n");
	run_api_tests();
	printf("probe\n");
	run_probe_tests();
	printf("%d tests, %d checks, %d failures\n", raiju_test_count, raiju_test_checks, raiju_test_failures);
	return raiju_test_failures == 0 ? 0 : 1;
}
