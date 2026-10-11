#include "test.h"

int raiju_test_checks = 0;
int raiju_test_failures = 0;
int raiju_test_count = 0;

int main(void) {
	printf("classify\n");
	run_classify_tests();
	printf("image_check\n");
	run_image_check_tests();
	printf("inspect\n");
	run_inspect_tests();
	printf("title_id\n");
	run_title_id_tests();
	printf("extract\n");
	run_extract_tests();
	printf("%d tests, %d checks, %d failures\n", raiju_test_count, raiju_test_checks, raiju_test_failures);
	return raiju_test_failures == 0 ? 0 : 1;
}
