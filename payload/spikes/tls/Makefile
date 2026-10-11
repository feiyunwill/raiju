PS5_HOST ?= ps5
PS5_PORT ?= 9021

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
THIRD_PARTY ?= ../../third_party/ps5

RAIJU_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF ?= tls.elf

CFLAGS := -Wall -Werror -O2 -g -I$(THIRD_PARTY)/include -DRAIJU_VERSION='"$(RAIJU_VERSION)"'

LDLIBS := -L$(THIRD_PARTY)/lib -lssl -lcrypto -lz -lSceNotification

SRC := main.c $(THIRD_PARTY)/ca_bundle.c

all: tls.elf tls-empty-ca.elf

tls.elf: $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

tls-empty-ca.elf: $(SRC)
	$(CC) $(CFLAGS) -DRAIJU_TLS_EMPTY_CA -o $@ $^ $(LDLIBS)

send: $(ELF)
	@if command -v socat >/dev/null 2>&1; then \
		$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF); \
	else \
		nc -w 12 $(PS5_HOST) $(PS5_PORT) < $(ELF); \
	fi

clean:
	rm -f tls.elf tls-empty-ca.elf

.PHONY: all clean send
