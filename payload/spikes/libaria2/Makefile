PS5_HOST ?= ps5
PS5_PORT ?= 9021

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
THIRD_PARTY ?= ../../third_party/ps5

RAIJU_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

SOURCE_URL ?= https://192.168.100.74:8443/raiju-10g.bin

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF := libaria2.elf

NC_TIMEOUT ?= 1500

CXXFLAGS := -std=c++17 -Wall -Werror -O2 -g -I$(THIRD_PARTY)/include \
	-DRAIJU_VERSION='"$(RAIJU_VERSION)"' \
	-DRAIJU_SOURCE_URL='"$(SOURCE_URL)"'

LDLIBS := -L$(THIRD_PARTY)/lib -laria2 -lssl -lcrypto -lz -lSceNotification

all: $(ELF)

$(ELF): main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

send: $(ELF)
	@if command -v socat >/dev/null 2>&1; then \
		$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF); \
	else \
		nc -w $(NC_TIMEOUT) $(PS5_HOST) $(PS5_PORT) < $(ELF); \
	fi

clean:
	rm -f $(ELF)

.PHONY: all clean send
