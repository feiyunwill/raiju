CC ?= cc
CFLAGS ?= -O1 -g
WARNINGS := -Wall -Wextra -Werror -std=c11 -pedantic
PKG_CONFIG ?= pkg-config
BREW_OPENSSL := $(shell command -v brew >/dev/null 2>&1 && brew --prefix openssl 2>/dev/null)
OPENSSL_PKG_CONFIG := PKG_CONFIG_PATH="$(if $(BREW_OPENSSL),$(BREW_OPENSSL)/lib/pkgconfig:)$$PKG_CONFIG_PATH" $(PKG_CONFIG)
OPENSSL_LIBS := $(shell $(OPENSSL_PKG_CONFIG) --libs openssl 2>/dev/null)
BREW_LIBARCHIVE := $(shell command -v brew >/dev/null 2>&1 && brew --prefix libarchive 2>/dev/null)
LIBARCHIVE_PKG_CONFIG := PKG_CONFIG_PATH="$(if $(BREW_LIBARCHIVE),$(BREW_LIBARCHIVE)/lib/pkgconfig:)$$PKG_CONFIG_PATH" $(PKG_CONFIG)
RAIJU_VERSION ?= 0.0.0
CPPFLAGS := -Iinclude -Isrc -I../third_party/civetweb -I../processor/include -DRAIJU_VERSION=\"$(RAIJU_VERSION)\"
CPPFLAGS += $(shell $(LIBARCHIVE_PKG_CONFIG) --cflags libarchive)
LDLIBS += $(shell $(LIBARCHIVE_PKG_CONFIG) --libs libarchive)
ifneq ($(OPENSSL_LIBS),)
CPPFLAGS += $(shell $(OPENSSL_PKG_CONFIG) --cflags openssl) -DRAIJU_FETCH_TLS
LDLIBS += $(OPENSSL_LIBS)
endif

CIVETWEB_VERSION := 1.16
CIVETWEB_DIR := ../third_party/civetweb
CIVETWEB_URL := https://github.com/civetweb/civetweb/archive/refs/tags/v$(CIVETWEB_VERSION).tar.gz
CIVETWEB_SHA256 := f0e471c1bf4e7804a6cfb41ea9d13e7d623b2bcc7bc1e2a4dd54951a24d60285
CIVETWEB_ARCHIVE := $(CIVETWEB_DIR)/civetweb-$(CIVETWEB_VERSION).tar.gz

EMBED_DIR ?= $(abspath ../../ui/dist)
FIXTURE_EMBED := tests/fixtures/embed

LDLIBS += -lpthread

BUILD := build
GENERATED := $(BUILD)/generated

CORE_SOURCES := \
	src/strbuf.c \
	src/fsutil.c \
	src/platform_host.c \
	src/engine_options.c \
	src/assets.c \
	src/pairing.c \
	src/allowlist.c \
	src/auth.c \
	src/proxy.c \
	src/fetch.c \
	src/art.c \
	src/api.c \
	src/api_probe.c \
	src/supervisor.c \
	src/http.c \
	src/gateway.c
SERVER_SOURCES := src/server.c
MAIN_SOURCES := src/main.c
TEST_SOURCES := $(wildcard tests/test_*.c)
INTEGRATION_SOURCES := $(wildcard tests/integration_*.c)

CORE_OBJECTS := $(CORE_SOURCES:%.c=$(BUILD)/%.o)
PROCESSOR_DIR := ../processor
CPPFLAGS += -I$(PROCESSOR_DIR)/tests
PROCESSOR_SOURCES := $(wildcard $(PROCESSOR_DIR)/src/*.c)
PROCESSOR_OBJECTS := $(patsubst $(PROCESSOR_DIR)/src/%.c,$(BUILD)/processor/%.o,$(PROCESSOR_SOURCES))
PROCESSOR_TEST_SUPPORT_SOURCES := $(PROCESSOR_DIR)/tests/archive_builder.c
PROCESSOR_TEST_SUPPORT_OBJECTS := $(patsubst $(PROCESSOR_DIR)/tests/%.c,$(BUILD)/processor-tests/%.o,$(PROCESSOR_TEST_SUPPORT_SOURCES))
HEADERS := $(wildcard include/raiju/*.h src/*.h tests/*.h)

GEN_EMBED_ASSETS := $(BUILD)/tools/gen_assets
ASSETS_TABLE := $(GENERATED)/assets_table.c
ASSETS_TABLE_OBJECT := $(GENERATED)/assets_table.o
ASSETS_FIXTURE := $(GENERATED)/assets_fixture.c
ASSETS_FIXTURE_OBJECT := $(GENERATED)/assets_fixture.o

CIVETWEB_OBJECT := $(BUILD)/third_party/civetweb/civetweb.o
SERVER_OBJECT := $(BUILD)/src/server.o
MAIN_OBJECT := $(BUILD)/src/main.o
TEST_OBJECTS := $(TEST_SOURCES:%.c=$(BUILD)/%.o)
TEST_SUPPORT_OBJECTS := $(BUILD)/tests/fixture_server.o
INTEGRATION_OBJECTS := $(INTEGRATION_SOURCES:%.c=$(BUILD)/%.o)

GATEWAY_BINARY := $(BUILD)/raiju-gateway
TEST_BINARY := $(BUILD)/raiju-gateway-tests
INTEGRATION_BINARY := $(BUILD)/raiju-gateway-integration

.PHONY: all test sanitize clean cli

all: $(GATEWAY_BINARY)

cli: $(GATEWAY_BINARY)

test: $(TEST_BINARY) $(INTEGRATION_BINARY)
	./$(TEST_BINARY)
	./$(INTEGRATION_BINARY)

SANITIZE_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

sanitize:
	$(MAKE) BUILD=$(BUILD)/sanitize CFLAGS="-O1 -g $(SANITIZE_FLAGS)" LDFLAGS="$(SANITIZE_FLAGS)" test

$(GATEWAY_BINARY): $(CORE_OBJECTS) $(PROCESSOR_OBJECTS) $(SERVER_OBJECT) $(MAIN_OBJECT) $(ASSETS_TABLE_OBJECT) $(CIVETWEB_OBJECT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_BINARY): $(CORE_OBJECTS) $(PROCESSOR_OBJECTS) $(TEST_OBJECTS) $(TEST_SUPPORT_OBJECTS) $(PROCESSOR_TEST_SUPPORT_OBJECTS) $(ASSETS_FIXTURE_OBJECT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(INTEGRATION_BINARY): $(CORE_OBJECTS) $(PROCESSOR_OBJECTS) $(SERVER_OBJECT) $(INTEGRATION_OBJECTS) $(TEST_SUPPORT_OBJECTS) $(ASSETS_FIXTURE_OBJECT) $(CIVETWEB_OBJECT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/processor/%.o: $(PROCESSOR_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/processor-tests/%.o: $(PROCESSOR_DIR)/tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) -c -o $@ $<

$(GEN_EMBED_ASSETS): tools/gen_assets.c
	@mkdir -p $(dir $@)
	$(CC) $(WARNINGS) $(CFLAGS) -o $@ $<

$(ASSETS_TABLE): $(GEN_EMBED_ASSETS)
	@mkdir -p $(dir $@)
	$(GEN_EMBED_ASSETS) $(EMBED_DIR) $@

$(ASSETS_FIXTURE): $(GEN_EMBED_ASSETS)
	@mkdir -p $(dir $@)
	$(GEN_EMBED_ASSETS) $(FIXTURE_EMBED) $@

$(ASSETS_TABLE_OBJECT): $(ASSETS_TABLE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) -c -o $@ $<

$(ASSETS_FIXTURE_OBJECT): $(ASSETS_FIXTURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) -c -o $@ $<

$(CIVETWEB_OBJECT): $(CIVETWEB_DIR)/civetweb.c $(CIVETWEB_DIR)/civetweb.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -DNO_SSL -DNO_CGI -DUSE_WEBSOCKET -I$(CIVETWEB_DIR) -c -o $@ $(CIVETWEB_DIR)/civetweb.c

$(SERVER_OBJECT): $(CIVETWEB_DIR)/civetweb.h

$(CIVETWEB_DIR)/civetweb.c:
	@mkdir -p $(CIVETWEB_DIR)
	curl -fsSL -o $(CIVETWEB_ARCHIVE) $(CIVETWEB_URL)
	echo "$(CIVETWEB_SHA256)  $(CIVETWEB_ARCHIVE)" | shasum -a 256 -c -
	tar -xzf $(CIVETWEB_ARCHIVE) -C $(CIVETWEB_DIR) --strip-components=2 \
	    civetweb-$(CIVETWEB_VERSION)/src civetweb-$(CIVETWEB_VERSION)/include
	rm -f $(CIVETWEB_ARCHIVE)

$(CIVETWEB_DIR)/civetweb.h: $(CIVETWEB_DIR)/civetweb.c

clean:
	rm -rf $(BUILD)
