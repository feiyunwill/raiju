PS5_HOST ?= ps5
PS5_PORT ?= 9021

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PROCESSOR := ../../processor

RAIJU_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF := smp.elf

NC_TIMEOUT ?= 600

CFLAGS := -Wall -Werror -O2 -g -I$(PROCESSOR)/include -I$(PROCESSOR)/tests -DRAIJU_VERSION='"$(RAIJU_VERSION)"'

LDLIBS := -lSceNotification

SOURCES := main.c $(PROCESSOR)/tests/image_builder.c $(PROCESSOR)/tests/fixtures.c

all: $(ELF)

$(ELF): $(SOURCES)
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
