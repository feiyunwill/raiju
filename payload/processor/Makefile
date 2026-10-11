CC ?= cc
CFLAGS ?= -O1 -g
WARNINGS := -Wall -Wextra -Werror -std=c11 -pedantic
CPPFLAGS := -Iinclude -Isrc

PKG_CONFIG ?= pkg-config
BREW_LIBARCHIVE := $(shell command -v brew >/dev/null 2>&1 && brew --prefix libarchive 2>/dev/null)
LIBARCHIVE_PKG_CONFIG := PKG_CONFIG_PATH="$(if $(BREW_LIBARCHIVE),$(BREW_LIBARCHIVE)/lib/pkgconfig:)$$PKG_CONFIG_PATH" $(PKG_CONFIG)
CPPFLAGS += $(shell $(LIBARCHIVE_PKG_CONFIG) --cflags libarchive)
LDLIBS += $(shell $(LIBARCHIVE_PKG_CONFIG) --libs libarchive)

BUILD := build
SOURCES := $(wildcard src/*.c)
TEST_SOURCES := $(wildcard tests/*.c)
OBJECTS := $(SOURCES:%.c=$(BUILD)/%.o)
TEST_OBJECTS := $(TEST_SOURCES:%.c=$(BUILD)/%.o)
HEADERS := $(wildcard include/raiju/*.h src/*.h tests/*.h)
TEST_BINARY := $(BUILD)/raiju-processor-tests

.PHONY: all test sanitize clean

all: $(OBJECTS)

test: $(TEST_BINARY)
	./$(TEST_BINARY)

SANITIZE_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

sanitize:
	$(MAKE) BUILD=$(BUILD)/sanitize CFLAGS="-O1 -g $(SANITIZE_FLAGS)" LDFLAGS="$(SANITIZE_FLAGS)" test

$(TEST_BINARY): $(OBJECTS) $(TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)
