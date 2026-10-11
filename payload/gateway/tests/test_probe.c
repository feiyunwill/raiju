#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <archive_entry.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "api_internal.h"
#include "archive_builder.h"
#include "fake_platform.h"
#include "fixture_server.h"
#include "raiju/fetch.h"
#include "raiju/gateway.h"
#include "raiju/http.h"
#include "test.h"

static size_t put16(unsigned char *out, uint16_t value) {
	out[0] = (unsigned char)(value & 0xFF);
	out[1] = (unsigned char)((value >> 8) & 0xFF);
	return 2;
}

static size_t put32(unsigned char *out, uint32_t value) {
	out[0] = (unsigned char)(value & 0xFF);
	out[1] = (unsigned char)((value >> 8) & 0xFF);
	out[2] = (unsigned char)((value >> 16) & 0xFF);
	out[3] = (unsigned char)((value >> 24) & 0xFF);
	return 4;
}

/**
 * Builds a minimal stored zip with one entry and returns its length.
 */
static size_t build_zip(const char *name, const unsigned char *data, size_t data_length, unsigned char *out,
                        size_t capacity) {
	size_t name_length = strlen(name);
	size_t local_size = 30 + name_length + data_length;
	size_t central_size = 46 + name_length;
	if (local_size + central_size + 22 > capacity) {
		return 0;
	}
	unsigned char *cursor = out;
	unsigned char *local = cursor;
	put32(cursor, 0x04034b50);
	put16(cursor + 4, 20);
	put16(cursor + 6, 0);
	put16(cursor + 8, 0);
	put16(cursor + 10, 0);
	put16(cursor + 12, 0);
	put32(cursor + 14, 0);
	put32(cursor + 18, (uint32_t)data_length);
	put32(cursor + 22, (uint32_t)data_length);
	put16(cursor + 26, (uint16_t)name_length);
	put16(cursor + 28, 0);
	memcpy(cursor + 30, name, name_length);
	memcpy(cursor + 30 + name_length, data, data_length);
	cursor += local_size;
	unsigned char *central = cursor;
	put32(cursor, 0x02014b50);
	put16(cursor + 4, 20);
	put16(cursor + 6, 20);
	put16(cursor + 8, 0);
	put16(cursor + 10, 0);
	put16(cursor + 12, 0);
	put16(cursor + 14, 0);
	put32(cursor + 16, 0);
	put32(cursor + 20, (uint32_t)data_length);
	put32(cursor + 24, (uint32_t)data_length);
	put16(cursor + 28, (uint16_t)name_length);
	put16(cursor + 30, 0);
	put16(cursor + 32, 0);
	put16(cursor + 34, 0);
	put16(cursor + 36, 0);
	put32(cursor + 38, 0);
	put32(cursor + 42, (uint32_t)(local - out));
	memcpy(cursor + 46, name, name_length);
	cursor += central_size;
	put32(cursor, 0x06054b50);
	put16(cursor + 4, 0);
	put16(cursor + 6, 0);
	put16(cursor + 8, 1);
	put16(cursor + 10, 1);
	put32(cursor + 12, (uint32_t)central_size);
	put32(cursor + 16, (uint32_t)(central - out));
	put16(cursor + 20, 0);
	cursor += 22;
	return (size_t)(cursor - out);
}

static size_t write_number(unsigned char *out, uint64_t value) {
	size_t extra = 0;
	while (extra < 8 && value >= ((uint64_t)1 << (extra * 8 + 7 - extra))) {
		extra++;
	}
	unsigned char mask = 0x80;
	unsigned char first = 0;
	for (size_t i = 0; i < extra; i++) {
		first |= mask;
		mask >>= 1;
	}
	for (size_t i = 0; i < extra; i++) {
		out[1 + i] = (unsigned char)((value >> (8 * i)) & 0xFF);
	}
	first |= (unsigned char)((value >> (8 * extra)) & (mask - 1));
	out[0] = first;
	return extra + 1;
}

/**
 * Builds a minimal 7z with one Copy-coded file: its packed and unpacked sizes
 * are both unpacked_size. Returns the archive length.
 */
static size_t build_sevenz(uint64_t unpacked_size, unsigned char *out, size_t capacity) {
	size_t packed = (size_t)unpacked_size;
	size_t header_max = 64;
	if (32 + packed + header_max > capacity) {
		return 0;
	}
	memset(out, 0, 32 + packed);
	static const unsigned char magic[6] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
	memcpy(out, magic, 6);
	unsigned char *header = out + 32 + packed;
	unsigned char *cursor = header;
	*cursor++ = 0x01;
	*cursor++ = 0x04;
	*cursor++ = 0x06;
	cursor += write_number(cursor, 0);
	cursor += write_number(cursor, 1);
	*cursor++ = 0x09;
	cursor += write_number(cursor, unpacked_size);
	*cursor++ = 0x00;
	*cursor++ = 0x07;
	*cursor++ = 0x0B;
	cursor += write_number(cursor, 1);
	*cursor++ = 0x00;
	cursor += write_number(cursor, 1);
	*cursor++ = 0x01;
	*cursor++ = 0x00;
	*cursor++ = 0x0C;
	cursor += write_number(cursor, unpacked_size);
	*cursor++ = 0x00;
	*cursor++ = 0x00;
	*cursor++ = 0x00;
	size_t header_size = (size_t)(cursor - header);
	put32(out + 8, 0);
	unsigned char offset[8];
	for (int i = 0; i < 8; i++) {
		offset[i] = (unsigned char)((packed >> (8 * i)) & 0xFF);
	}
	memcpy(out + 12, offset, 8);
	unsigned char size_bytes[8];
	for (int i = 0; i < 8; i++) {
		size_bytes[i] = (unsigned char)((header_size >> (8 * i)) & 0xFF);
	}
	memcpy(out + 20, size_bytes, 8);
	put32(out + 28, 0);
	return 32 + packed + header_size;
}

static void pair_the_gateway(raiju_gateway *gateway, char *cookie, size_t capacity) {
	char body[128];
	snprintf(body, sizeof body, "{\"code\":\"%s\",\"label\":\"test\"}", raiju_gateway_code(gateway));
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/pair";
	request.body = (const unsigned char *)body;
	request.body_length = strlen(body);
	raiju_http_response response;
	raiju_response_init(&response);
	CHECK_INT(raiju_gateway_handle(gateway, &request, &response), 200);
	const char *token = strstr(raiju_strbuf_cstr(&response.body), "\"token\":\"");
	char value[80] = {0};
	if (token != NULL) {
		token += strlen("\"token\":\"");
		memcpy(value, token, RAIJU_PAIR_TOKEN_HEX);
	}
	snprintf(cookie, capacity, "raiju_device=%s", value);
	raiju_response_free(&response);
}

static raiju_gateway *make_gateway(raiju_fake_platform *fake, char *directory, size_t capacity) {
	if (raiju_test_tmpdir(directory, capacity, "probe") != 0) {
		return NULL;
	}
	char session[1200];
	snprintf(session, sizeof session, "%s/session", directory);
	mkdir(session, 0755);
	const raiju_platform *platform = raiju_fake_platform_init(fake);
	raiju_gateway_config config;
	memset(&config, 0, sizeof config);
	config.platform = platform;
	config.data_dir = directory;
	config.aria2_url = "http://127.0.0.1:6800/jsonrpc";
	config.fetch = raiju_fetch_default();
	return raiju_gateway_create(&config);
}

static int probe(raiju_gateway *gateway, const char *cookie, const char *uri, char *out, size_t capacity) {
	char body[256];
	snprintf(body, sizeof body, "{\"uri\":\"%s\"}", uri);
	raiju_http_request request;
	memset(&request, 0, sizeof request);
	request.method = "POST";
	request.path = "/api/probe";
	request.cookie = cookie;
	request.body = (const unsigned char *)body;
	request.body_length = strlen(body);
	raiju_http_response response;
	raiju_response_init(&response);
	int status = raiju_gateway_handle(gateway, &request, &response);
	snprintf(out, capacity, "%s", raiju_strbuf_cstr(&response.body));
	raiju_response_free(&response);
	return status;
}

static void probes_a_zip_and_a_sevenz(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	unsigned char zip[2048];
	static const unsigned char payload[] = "hello raiju payload";
	size_t zip_size = build_zip("sce_sys/eboot.bin", payload, sizeof payload - 1, zip, sizeof zip);
	CHECK(zip_size > 0);
	CHECK(raiju_fixture_server_add_file(server, "/game.zip", zip, zip_size));
	unsigned char sevenz[4096];
	size_t sevenz_size = build_sevenz(100, sevenz, sizeof sevenz);
	CHECK(sevenz_size > 0);
	CHECK(raiju_fixture_server_add_file(server, "/game.7z", sevenz, sevenz_size));
	static const char text[] = "just some text, not an archive";
	CHECK(raiju_fixture_server_add_file(server, "/notes.txt", text, sizeof text - 1));

	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, directory, sizeof directory);
	CHECK(gateway != NULL);
	fake.free_bytes = 4ull * 1024 * 1024 * 1024;
	char cookie[160];
	pair_the_gateway(gateway, cookie, sizeof cookie);
	char uri[128];
	char body[2048];

	snprintf(uri, sizeof uri, "http://127.0.0.1:%u/game.zip", raiju_fixture_server_port(server));
	CHECK_INT(probe(gateway, cookie, uri, body, sizeof body), 200);
	CHECK(strstr(body, "\"kind\":\"zip\"") != NULL);
	CHECK(strstr(body, "\"uncompressedKnown\":true") != NULL);
	char expected[64];
	snprintf(expected, sizeof expected, "\"uncompressedSize\":%zu", sizeof payload - 1);
	CHECK(strstr(body, expected) != NULL);
	CHECK(strstr(body, "\"fits\":true") != NULL);
	CHECK(strstr(raiju_fixture_server_last_range(server), "bytes=") != NULL);

	snprintf(uri, sizeof uri, "http://127.0.0.1:%u/game.7z", raiju_fixture_server_port(server));
	CHECK_INT(probe(gateway, cookie, uri, body, sizeof body), 200);
	CHECK(strstr(body, "\"kind\":\"7z\"") != NULL);
	CHECK(strstr(body, "\"uncompressedSize\":100") != NULL);
	CHECK(strstr(body, "\"uncompressedKnown\":true") != NULL);

	snprintf(uri, sizeof uri, "http://127.0.0.1:%u/notes.txt", raiju_fixture_server_port(server));
	CHECK_INT(probe(gateway, cookie, uri, body, sizeof body), 200);
	CHECK(strstr(body, "\"kind\":\"unknown\"") != NULL);
	CHECK(strstr(body, "\"uncompressedKnown\":false") != NULL);

	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
	raiju_fixture_server_stop(server);
}

static size_t read_file_bytes(const char *path, unsigned char *out, size_t capacity) {
	FILE *handle = fopen(path, "rb");
	if (handle == NULL) {
		return 0;
	}
	size_t length = fread(out, 1, capacity, handle);
	fclose(handle);
	return length;
}

static void probes_real_archives_written_by_libarchive(void) {
	char directory[1024];
	if (raiju_test_tmpdir(directory, sizeof directory, "probe-real") != 0) {
		CHECK(false);
		return;
	}
	unsigned char first[100];
	unsigned char second[250];
	memset(first, 'a', sizeof first);
	memset(second, 'b', sizeof second);
	const size_t expected_sum = sizeof first + sizeof second;
	const fixture_entry entries[] = {
	    {"first.bin", AE_IFREG, first, sizeof first, NULL},
	    {"second.bin", AE_IFREG, second, sizeof second, NULL},
	};
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);

	char sevenz_path[1200];
	snprintf(sevenz_path, sizeof sevenz_path, "%s/game.7z", directory);
	int sevenz_status = archive_builder_write_ex(sevenz_path, FORMAT_7Z, entries, 2, true);
	char zip_path[1200];
	snprintf(zip_path, sizeof zip_path, "%s/game.zip", directory);
	int zip_status = archive_builder_write(zip_path, FORMAT_ZIP, entries, 2);
	if (sevenz_status == 1) {
		printf("  (skipping real 7z: libarchive cannot write 7z on this host)\n");
	}
	if (zip_status == 1) {
		printf("  (skipping real zip: libarchive cannot write zip on this host)\n");
	}
	CHECK(sevenz_status == 0 || sevenz_status == 1);
	CHECK(zip_status == 0 || zip_status == 1);

	unsigned char bytes[65536];
	if (sevenz_status == 0) {
		size_t length = read_file_bytes(sevenz_path, bytes, sizeof bytes);
		CHECK(length > 32);
		CHECK(raiju_fixture_server_add_file(server, "/real.7z", bytes, length));
	}
	if (zip_status == 0) {
		size_t length = read_file_bytes(zip_path, bytes, sizeof bytes);
		CHECK(length > 22);
		CHECK(raiju_fixture_server_add_file(server, "/real.zip", bytes, length));
	}

	raiju_fake_platform fake;
	char gateway_dir[1024];
	raiju_gateway *gateway = make_gateway(&fake, gateway_dir, sizeof gateway_dir);
	CHECK(gateway != NULL);
	fake.free_bytes = 4ull * 1024 * 1024 * 1024;
	char cookie[160];
	pair_the_gateway(gateway, cookie, sizeof cookie);
	char uri[128];
	char body[2048];
	char expected[64];
	snprintf(expected, sizeof expected, "\"uncompressedSize\":%zu", expected_sum);

	if (sevenz_status == 0) {
		snprintf(uri, sizeof uri, "http://127.0.0.1:%u/real.7z", raiju_fixture_server_port(server));
		CHECK_INT(probe(gateway, cookie, uri, body, sizeof body), 200);
		CHECK(strstr(body, "\"kind\":\"7z\"") != NULL);
		CHECK(strstr(body, "\"uncompressedKnown\":true") != NULL);
		CHECK(strstr(body, expected) != NULL);
	}
	if (zip_status == 0) {
		snprintf(uri, sizeof uri, "http://127.0.0.1:%u/real.zip", raiju_fixture_server_port(server));
		CHECK_INT(probe(gateway, cookie, uri, body, sizeof body), 200);
		CHECK(strstr(body, "\"kind\":\"zip\"") != NULL);
		CHECK(strstr(body, "\"uncompressedKnown\":true") != NULL);
		CHECK(strstr(body, expected) != NULL);
	}

	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(gateway_dir);
	raiju_test_rmtree(directory);
	raiju_fixture_server_stop(server);
}

static void a_probe_is_refused_while_another_runs(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	static const char blob[] = "0123456789";
	CHECK(raiju_fixture_server_add_file(server, "/blob.bin", blob, sizeof blob - 1));
	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, directory, sizeof directory);
	CHECK(gateway != NULL);
	char cookie[160];
	pair_the_gateway(gateway, cookie, sizeof cookie);
	raiju_api *api = gateway->api;
	CHECK(api != NULL);
	char uri[128];
	snprintf(uri, sizeof uri, "http://127.0.0.1:%u/blob.bin", raiju_fixture_server_port(server));
	char body[2048];
	pthread_mutex_lock(&api->probe_lock);
	CHECK_INT(probe(gateway, cookie, uri, body, sizeof body), 503);
	CHECK(strstr(body, "probe_busy") != NULL);
	pthread_mutex_unlock(&api->probe_lock);
	CHECK_INT(probe(gateway, cookie, uri, body, sizeof body), 200);
	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
	raiju_fixture_server_stop(server);
}

static void fits_reflects_free_space(void) {
	raiju_fixture_server *server = raiju_fixture_server_start();
	CHECK(server != NULL);
	unsigned char zip[2048];
	static const unsigned char payload[] = "payload";
	size_t zip_size = build_zip("a.bin", payload, sizeof payload - 1, zip, sizeof zip);
	CHECK(raiju_fixture_server_add_file(server, "/small.zip", zip, zip_size));

	raiju_fake_platform fake;
	char directory[1024];
	raiju_gateway *gateway = make_gateway(&fake, directory, sizeof directory);
	CHECK(gateway != NULL);
	char cookie[160];
	pair_the_gateway(gateway, cookie, sizeof cookie);
	char uri[128];
	snprintf(uri, sizeof uri, "http://127.0.0.1:%u/small.zip", raiju_fixture_server_port(server));
	char body[2048];
	fake.free_bytes = 4ull * 1024 * 1024 * 1024;
	CHECK_INT(probe(gateway, cookie, uri, body, sizeof body), 200);
	CHECK(strstr(body, "\"fits\":true") != NULL);
	fake.free_bytes = 1024;
	CHECK_INT(probe(gateway, cookie, uri, body, sizeof body), 200);
	CHECK(strstr(body, "\"fits\":false") != NULL);

	raiju_gateway_destroy(gateway);
	raiju_test_rmtree(directory);
	raiju_fixture_server_stop(server);
}

void run_probe_tests(void) {
	RUN_TEST(probes_a_zip_and_a_sevenz);
	RUN_TEST(probes_real_archives_written_by_libarchive);
	RUN_TEST(a_probe_is_refused_while_another_runs);
	RUN_TEST(fits_reflects_free_space);
}
