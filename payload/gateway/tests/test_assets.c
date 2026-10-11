#include "raiju/assets.h"
#include "test.h"

static void mime_types_follow_extension(void) {
	CHECK_STR(raiju_assets_mime("index.html"), "text/html; charset=utf-8");
	CHECK_STR(raiju_assets_mime("app.js"), "text/javascript; charset=utf-8");
	CHECK_STR(raiju_assets_mime("style.css"), "text/css; charset=utf-8");
	CHECK_STR(raiju_assets_mime("logo.svg"), "image/svg+xml");
	CHECK_STR(raiju_assets_mime("data.json"), "application/json");
	CHECK_STR(raiju_assets_mime("noext"), "application/octet-stream");
	CHECK_STR(raiju_assets_mime("archive.bin"), "application/octet-stream");
}

static void embedded_lookup_uses_the_fixture_table(void) {
	const raiju_asset *index = raiju_assets_find("/");
	CHECK(index != NULL);
	if (index != NULL) {
		CHECK_STR(index->path, "index.html");
		CHECK(index->length > 0);
		CHECK(memcmp(index->bytes, "<!doctype", 9) == 0);
	}
	const raiju_asset *script = raiju_assets_find("/app.js");
	CHECK(script != NULL);
	CHECK(raiju_assets_find("/missing.txt") == NULL);
	CHECK(raiju_assets_find("/nested/deep.css") == NULL);
}

void run_assets_tests(void) {
	RUN_TEST(mime_types_follow_extension);
	RUN_TEST(embedded_lookup_uses_the_fixture_table);
}
