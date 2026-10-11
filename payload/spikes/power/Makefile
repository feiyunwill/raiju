PS5_HOST ?= ps5
PS5_PORT ?= 9021

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

RAIJU_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF := power.elf

NC_TIMEOUT ?= 600

HOLD_SECONDS ?= 7200
TICK_SECONDS ?= 10
RELEASE_SECONDS ?= 3600
RELEASE_ONLY ?= 0
PEER_HOST ?= 192.168.100.74
PEER_PORT ?= 8000

CFLAGS := -Wall -Werror -O2 -g -DRAIJU_VERSION='"$(RAIJU_VERSION)"' \
	-DRAIJU_HOLD_SECONDS=$(HOLD_SECONDS) -DRAIJU_TICK_SECONDS=$(TICK_SECONDS) \
	-DRAIJU_RELEASE_SECONDS=$(RELEASE_SECONDS) -DRAIJU_RELEASE_ONLY=$(RELEASE_ONLY) \
	-DRAIJU_PEER_HOST='"$(PEER_HOST)"' -DRAIJU_PEER_PORT=$(PEER_PORT)

LDLIBS := -lSceSystemService -lSceNotification -lpthread

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
