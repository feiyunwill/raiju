PS5_HOST ?= ps5
PS5_PORT ?= 9021

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

RAIJU_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

PREFIX := ../../third_party/ps5

SAMPLE_NAME ?= sample.7z
FETCH_HOST ?= 192.168.100.74
FETCH_PORT ?= 8000
ARCHIVE_PATH ?= /data/raiju/spike/$(SAMPLE_NAME)

ELF := libarchive.elf


NC_TIMEOUT ?= 600

CFLAGS := -Wall -Werror -O2 -g -DRAIJU_VERSION='"$(RAIJU_VERSION)"' \
	-DRAIJU_SAMPLE_NAME='"$(SAMPLE_NAME)"' -DRAIJU_ARCHIVE_PATH='"$(ARCHIVE_PATH)"' \
	-DRAIJU_FETCH_HOST='"$(FETCH_HOST)"' -DRAIJU_FETCH_PORT=$(FETCH_PORT) \
	-I$(PREFIX)/include

LDLIBS := -lSceNotification -L$(PREFIX)/lib -larchive -lzstd -llzma -lbz2 -lz

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

send: $(ELF)
	@if command -v socat >/dev/null 2>&1; then \
		$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF); \
	else \
		nc -w $(NC_TIMEOUT) $(PS5_HOST) $(PS5_PORT) < $(ELF); \
	fi

clean:
	rm -f $(ELF)

.PHONY: all clean send
