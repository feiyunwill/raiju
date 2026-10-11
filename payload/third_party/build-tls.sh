#!/bin/sh

set -eu

PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

ZLIB_VERSION=1.3.1
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
OPENSSL_VERSION=3.6.5
OPENSSL_SHA256=a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98
CA_BUNDLE_COMMIT=ab325adf04921579c89f77559fce20f964695ca9
CA_BUNDLE_SHA256=88039912c0ec918f6980d7533e0bb10b04f306d586894b515cff954ba9175c8b

here=$(cd "$(dirname "$0")" && pwd)
cache="$here/cache"
src="$here/src"
prefix="$here/ps5"

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

# Prints the sha256 of a file, whichever tool the host ships.
sha256() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		sha256sum "$1" | cut -d' ' -f1
	fi
}

# Fails early, and legibly, when the SDK wrappers cannot reach an LLVM of the right vintage.
check_toolchain() {
	if ! "$PS5_PAYLOAD_SDK/bin/prospero-clang" --version >/dev/null 2>&1; then
		echo "the SDK's clang cannot find llvm-config:" >&2
		echo "put llvm-config-18 on PATH, or set LLVM_CONFIG to it" >&2
		exit 1
	fi
}

# Downloads a pinned file, refusing anything whose hash differs from the pin.
fetch() {
	url="$1"
	out="$2"
	want="$3"

	if [ -f "$out" ] && [ "$(sha256 "$out")" = "$want" ]; then
		return 0
	fi
	echo "fetch $(basename "$out")"
	curl -fsSL -o "$out" "$url"
	if [ "$(sha256 "$out")" != "$want" ]; then
		echo "$out does not match its pinned sha256" >&2
		exit 1
	fi
}

# Unpacks a cached tarball into the source tree.
unpack() {
	archive="$1"
	name="$2"

	rm -rf "$src/$name"
	mkdir -p "$src"
	tar -C "$src" -xzf "$cache/$archive"
}

# Builds zlib as a static library for the console. zlib's own configure picks the host's libtool
# when it runs on macOS, which cannot archive console objects, so the objects are listed here and
# archived with the SDK's ar instead.
build_zlib() {
	[ -f "$prefix/lib/libz.a" ] && return 0

	unpack "zlib-$ZLIB_VERSION.tar.gz" "zlib-$ZLIB_VERSION"
	(
		cd "$src/zlib-$ZLIB_VERSION"
		mkdir -p "$prefix/include" "$prefix/lib"
		"$PS5_PAYLOAD_SDK/bin/prospero-clang" -fPIC -O2 -D_LARGEFILE64_SOURCE=1 -DHAVE_HIDDEN -I. \
			-c adler32.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c \
			zutil.c compress.c uncompr.c gzclose.c gzlib.c gzread.c gzwrite.c
		"$PS5_PAYLOAD_SDK/bin/prospero-ar" rcs "$prefix/lib/libz.a" ./*.o
		"$PS5_PAYLOAD_SDK/bin/prospero-ranlib" "$prefix/lib/libz.a"
		cp zlib.h zconf.h "$prefix/include/"
		rm -f ./*.o
	)
}

# Builds OpenSSL 3 as static libraries for the console, without test programs, without assembly and
# without the engines that are Linux- or VIA-only, none of which exist on the console.
build_openssl() {
	[ -f "$prefix/lib/libssl.a" ] && return 0

	unpack "openssl-$OPENSSL_VERSION.tar.gz" "openssl-$OPENSSL_VERSION"
	(
		cd "$src/openssl-$OPENSSL_VERSION"
		perl Configure no-shared no-tests no-asm no-docs no-afalgeng no-padlockeng \
			--prefix="$prefix" --openssldir="$prefix/ssl" --libdir=lib \
			CC="$PS5_PAYLOAD_SDK/bin/prospero-clang" \
			AR="$PS5_PAYLOAD_SDK/bin/prospero-ar" \
			RANLIB="$PS5_PAYLOAD_SDK/bin/prospero-ranlib" \
			CFLAGS="-fPIC -O2" \
			linux-x86_64
		make -j"$jobs" build_sw
		make install_sw
	)
}

# Keeps the CA bundle as a PEM file and as a C array the payload can embed.
build_ca_bundle() {
	cp "$cache/ca-bundle.crt" "$prefix/ca.pem"
	perl -0777 -ne 'printf "const unsigned char raiju_ca_bundle[] = {%s};\nconst unsigned long raiju_ca_bundle_len = %d;\n", join(",", map { sprintf "0x%02x", ord } split //), length' \
		"$prefix/ca.pem" >"$prefix/ca_bundle.c"
}

mkdir -p "$cache" "$src" "$prefix"

check_toolchain

fetch "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz" \
	"$cache/zlib-$ZLIB_VERSION.tar.gz" "$ZLIB_SHA256"
fetch "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz" \
	"$cache/openssl-$OPENSSL_VERSION.tar.gz" "$OPENSSL_SHA256"
fetch "https://raw.githubusercontent.com/bagder/ca-bundle/$CA_BUNDLE_COMMIT/ca-bundle.crt" \
	"$cache/ca-bundle.crt" "$CA_BUNDLE_SHA256"

build_zlib
build_openssl
build_ca_bundle

echo "prefix: $prefix"
ls -l "$prefix/lib/libz.a" "$prefix/lib/libssl.a" "$prefix/lib/libcrypto.a"
echo "ca bundle: $(grep -c 'BEGIN CERTIFICATE' "$prefix/ca.pem") certificates"
