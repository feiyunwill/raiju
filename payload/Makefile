PS5_HOST ?= ps5
PS5_PORT ?= 9021

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
THIRD_PARTY ?= third_party/ps5
include third_party/civetweb.mk
EMBED_DIR ?= $(abspath ../ui/dist)
HOSTCC ?= cc

RAIJU_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF := raiju.elf
BUILD := build
GENERATED := $(BUILD)/generated

GATEWAY_DIR := gateway
PROCESSOR_DIR := processor

WARNINGS := -Wall -Wextra -Werror -std=c11 -pedantic
CXX_WARNINGS := -Wall -Wextra -Werror -std=c++17
CFLAGS := -O2 -g
CXXFLAGS := -O2 -g

BSD_SURFACE := -D__BSD_VISIBLE=1

CPPFLAGS := -I$(GATEWAY_DIR)/include -I$(PROCESSOR_DIR)/include -I$(THIRD_PARTY)/include \
	-I$(CIVETWEB_DIR) -DRAIJU_VERSION='"$(RAIJU_VERSION)"' \
	-DRAIJU_FETCH_TLS -DRAIJU_HAVE_LZMA $(BSD_SURFACE)

LDLIBS := -L$(THIRD_PARTY)/lib -laria2 -larchive -lzstd -llzma -lbz2 -lssl -lcrypto -lz \
	-lSceNotification -lpthread

GATEWAY_SOURCES := \
	$(GATEWAY_DIR)/src/strbuf.c \
	$(GATEWAY_DIR)/src/fsutil.c \
	$(GATEWAY_DIR)/src/engine_options.c \
	$(GATEWAY_DIR)/src/assets.c \
	$(GATEWAY_DIR)/src/pairing.c \
	$(GATEWAY_DIR)/src/allowlist.c \
	$(GATEWAY_DIR)/src/auth.c \
	$(GATEWAY_DIR)/src/proxy.c \
	$(GATEWAY_DIR)/src/fetch.c \
	$(GATEWAY_DIR)/src/art.c \
	$(GATEWAY_DIR)/src/api.c \
	$(GATEWAY_DIR)/src/api_probe.c \
	$(GATEWAY_DIR)/src/sevenz.c \
	$(GATEWAY_DIR)/src/supervisor.c \
	$(GATEWAY_DIR)/src/http.c \
	$(GATEWAY_DIR)/src/gateway.c \
	$(GATEWAY_DIR)/src/local_code.c \
	$(GATEWAY_DIR)/src/torrent.c \
	$(GATEWAY_DIR)/src/server.c

PROCESSOR_SOURCES := $(wildcard $(PROCESSOR_DIR)/src/*.c)
PAYLOAD_SOURCES := src/main.c src/platform_ps5.c $(THIRD_PARTY)/ca_bundle.c

GATEWAY_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(GATEWAY_SOURCES))
PROCESSOR_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(PROCESSOR_SOURCES))
PAYLOAD_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(PAYLOAD_SOURCES))
ENGINE_OBJECT := $(BUILD)/src/engine_ps5.o
CIVETWEB_OBJECT := $(BUILD)/third_party/civetweb/civetweb.o

GEN_EMBED_ASSETS := $(BUILD)/tools/gen_assets
ASSETS_TABLE := $(GENERATED)/assets_table.c
ASSETS_OBJECT := $(GENERATED)/assets_table.o

HEADERS := $(wildcard $(GATEWAY_DIR)/include/raiju/*.h) \
	$(wildcard $(PROCESSOR_DIR)/include/raiju/*.h) \
	$(wildcard src/*.h) \
	$(CIVETWEB_DIR)/civetweb.h

OBJECTS := $(GATEWAY_OBJECTS) $(PROCESSOR_OBJECTS) $(PAYLOAD_OBJECTS) $(ENGINE_OBJECT) \
	$(CIVETWEB_OBJECT) $(ASSETS_OBJECT)

all: $(ELF)

$(ELF): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXX_WARNINGS) $(CXXFLAGS) -c -o $@ $<

$(CIVETWEB_OBJECT): $(CIVETWEB_DIR)/civetweb.c $(CIVETWEB_DIR)/civetweb.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DNO_SSL -DNO_CGI -DUSE_WEBSOCKET -I$(CIVETWEB_DIR) -c -o $@ $<

$(ASSETS_OBJECT): $(ASSETS_TABLE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) -c -o $@ $<

$(ASSETS_TABLE): $(GEN_EMBED_ASSETS) $(shell find $(EMBED_DIR) -type f 2>/dev/null)
	@mkdir -p $(dir $@)
	$(GEN_EMBED_ASSETS) $(EMBED_DIR) $@

$(GEN_EMBED_ASSETS): $(GATEWAY_DIR)/tools/gen_assets.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(WARNINGS) -O1 -o $@ $<

send: $(ELF)
	@if command -v socat >/dev/null 2>&1; then \
		$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF); \
	else \
		nc -w 12 $(PS5_HOST) $(PS5_PORT) < $(ELF); \
	fi

clean:
	rm -rf $(BUILD) $(ELF)

.PHONY: all clean send
