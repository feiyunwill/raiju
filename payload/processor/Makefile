CC ?= cc
CFLAGS ?= -O1 -g
WARNINGS := -Wall -Wextra -Werror -std=c11 -pedantic
CPPFLAGS := -Iinclude -Isrc

BUILD := build
SOURCES := $(wildcard src/*.c)
TEST_SOURCES := $(wildcard tests/*.c)
OBJECTS := $(SOURCES:%.c=$(BUILD)/%.o)
TEST_OBJECTS := $(TEST_SOURCES:%.c=$(BUILD)/%.o)
HEADERS := $(wildcard include/raiju/*.h src/*.h tests/*.h)
TEST_BINARY := $(BUILD)/raiju-processor-tests

.PHONY: all test clean

all: $(OBJECTS)

test: $(TEST_BINARY)
	./$(TEST_BINARY)

$(TEST_BINARY): $(OBJECTS) $(TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)
