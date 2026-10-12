CIVETWEB_VERSION := 1.16
CIVETWEB_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))/civetweb
CIVETWEB_URL := https://github.com/civetweb/civetweb/archive/refs/tags/v$(CIVETWEB_VERSION).tar.gz
CIVETWEB_SHA256 := f0e471c1bf4e7804a6cfb41ea9d13e7d623b2bcc7bc1e2a4dd54951a24d60285
CIVETWEB_ARCHIVE := $(CIVETWEB_DIR)/civetweb-$(CIVETWEB_VERSION).tar.gz

.DEFAULT_GOAL := all

$(CIVETWEB_DIR)/civetweb.c:
	@mkdir -p $(CIVETWEB_DIR)
	curl -fsSL -o $(CIVETWEB_ARCHIVE) $(CIVETWEB_URL)
	echo "$(CIVETWEB_SHA256)  $(CIVETWEB_ARCHIVE)" | shasum -a 256 -c -
	tar -xzf $(CIVETWEB_ARCHIVE) -C $(CIVETWEB_DIR) --strip-components=2 \
	    civetweb-$(CIVETWEB_VERSION)/src civetweb-$(CIVETWEB_VERSION)/include
	rm -f $(CIVETWEB_ARCHIVE)

$(CIVETWEB_DIR)/civetweb.h: $(CIVETWEB_DIR)/civetweb.c
