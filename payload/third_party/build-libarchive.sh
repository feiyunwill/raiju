#!/bin/sh

set -eu

PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

BZIP2_VERSION=1.0.8
BZIP2_SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
XZ_VERSION=5.6.4
XZ_SHA256=269e3f2e512cbd3314849982014dc199a7b2148cf5c91cedc6db629acdf5e09b
ZSTD_VERSION=1.5.6
ZSTD_SHA256=8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
LIBARCHIVE_VERSION=3.7.7
LIBARCHIVE_SHA256=4cc540a3e9a1eebdefa1045d2e4184831100667e6d7d5b315bb1cbc951f8ddff
CONSOLE_HOST_TRIPLET=x86_64-unknown-freebsd12

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

# Fails early when the prefix lacks the zlib libarchive links against, which build-tls.sh builds.
check_dependencies() {
	if [ ! -f "$prefix/lib/libz.a" ]; then
		echo "$prefix/lib/libz.a is missing: run build-tls.sh first" >&2
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

# Unpacks a cached tarball into the source tree at the depth the archive uses.
unpack() {
	archive="$1"
	name="$2"

	rm -rf "$src/$name"
	mkdir -p "$src"
	tar -C "$src" -xzf "$cache/$archive"
}

# Builds bzip2 as a static library for the console. bzip2 ships a hand-written Makefile that
# expects a POSIX make, so the objects are listed here and archived with the SDK's ar instead.
build_bzip2() {
	[ -f "$prefix/lib/libbz2.a" ] && return 0

	unpack "bzip2-$BZIP2_VERSION.tar.gz" "bzip2-$BZIP2_VERSION"
	(
		cd "$src/bzip2-$BZIP2_VERSION"
		mkdir -p "$prefix/include" "$prefix/lib"
		"$PS5_PAYLOAD_SDK/bin/prospero-clang" -fPIC -O2 -D_FILE_OFFSET_BITS=64 \
			-c blocksort.c huffman.c crctable.c randtable.c compress.c decompress.c bzlib.c
		"$PS5_PAYLOAD_SDK/bin/prospero-ar" rcs "$prefix/lib/libbz2.a" ./*.o
		"$PS5_PAYLOAD_SDK/bin/prospero-ranlib" "$prefix/lib/libbz2.a"
		cp bzlib.h "$prefix/include/"
		rm -f ./*.o
	)
}

# Builds liblzma as a static library for the console, without the command line tools. The
# assembler is left off: libarchive only needs the C decoders, and the SDK's assembler support
# for a FreeBSD target is not worth the risk here.
build_xz() {
	[ -f "$prefix/lib/liblzma.a" ] && return 0

	unpack "xz-$XZ_VERSION.tar.gz" "xz-$XZ_VERSION"
	(
		cd "$src/xz-$XZ_VERSION"
		./configure --host="$CONSOLE_HOST_TRIPLET" --prefix="$prefix" \
			CC="$PS5_PAYLOAD_SDK/bin/prospero-clang" \
			AR="$PS5_PAYLOAD_SDK/bin/prospero-ar" \
			RANLIB="$PS5_PAYLOAD_SDK/bin/prospero-ranlib" \
			CFLAGS="-fPIC -O2" \
			--enable-static --disable-shared --disable-nls --disable-assembler \
			--disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-scripts \
			--disable-doc --disable-doxygen
		make -j"$jobs"
		make install
	)
}

# Builds zstd as a static library for the console. Upstream ships no configure, and its Makefile
# honours CC, AR and RANLIB, so it is driven directly and its public headers are copied by hand.
build_zstd() {
	[ -f "$prefix/lib/libzstd.a" ] && return 0

	unpack "zstd-$ZSTD_VERSION.tar.gz" "zstd-$ZSTD_VERSION"
	(
		cd "$src/zstd-$ZSTD_VERSION/lib"
		mkdir -p "$prefix/include" "$prefix/lib"
		make -j"$jobs" libzstd.a \
			CC="$PS5_PAYLOAD_SDK/bin/prospero-clang" \
			AR="$PS5_PAYLOAD_SDK/bin/prospero-ar" \
			RANLIB="$PS5_PAYLOAD_SDK/bin/prospero-ranlib" \
			CFLAGS="-fPIC -O2"
		cp libzstd.a "$prefix/lib/"
		cp zstd.h zstd_errors.h "$prefix/include/"
	)
}

# Builds libarchive as a static library against the console copies of zlib, bzip2, liblzma and
# zstd, with the readers and writers that have no place on the console left out.
build_libarchive() {
	[ -f "$prefix/lib/libarchive.a" ] && return 0

	unpack "libarchive-$LIBARCHIVE_VERSION.tar.gz" "libarchive-$LIBARCHIVE_VERSION"
	(
		cd "$src/libarchive-$LIBARCHIVE_VERSION"
		./configure --host="$CONSOLE_HOST_TRIPLET" --prefix="$prefix" \
			CC="$PS5_PAYLOAD_SDK/bin/prospero-clang" \
			AR="$PS5_PAYLOAD_SDK/bin/prospero-ar" \
			RANLIB="$PS5_PAYLOAD_SDK/bin/prospero-ranlib" \
			CFLAGS="-fPIC -O2 -I$prefix/include" \
			LDFLAGS="-L$prefix/lib" \
			PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" \
			--enable-static --disable-shared --disable-nls \
			--with-zlib --with-bz2lib --with-lzma --with-zstd \
			--without-xml2 --without-expat --without-openssl --without-nettle --without-lz4 \
			--without-lzo2 --without-iconv \
			--disable-bsdtar --disable-bsdcpio --disable-bsdcat --disable-bsdunzip
		make -j"$jobs"
		make install
	)
}

mkdir -p "$cache" "$src" "$prefix"

check_toolchain
check_dependencies

fetch "https://sourceware.org/pub/bzip2/bzip2-$BZIP2_VERSION.tar.gz" \
	"$cache/bzip2-$BZIP2_VERSION.tar.gz" "$BZIP2_SHA256"
fetch "https://github.com/tukaani-project/xz/releases/download/v$XZ_VERSION/xz-$XZ_VERSION.tar.gz" \
	"$cache/xz-$XZ_VERSION.tar.gz" "$XZ_SHA256"
fetch "https://github.com/facebook/zstd/releases/download/v$ZSTD_VERSION/zstd-$ZSTD_VERSION.tar.gz" \
	"$cache/zstd-$ZSTD_VERSION.tar.gz" "$ZSTD_SHA256"
fetch "https://github.com/libarchive/libarchive/releases/download/v$LIBARCHIVE_VERSION/libarchive-$LIBARCHIVE_VERSION.tar.gz" \
	"$cache/libarchive-$LIBARCHIVE_VERSION.tar.gz" "$LIBARCHIVE_SHA256"

build_bzip2
build_xz
build_zstd
build_libarchive

echo "prefix: $prefix"
ls -l "$prefix/lib/libbz2.a" "$prefix/lib/liblzma.a" "$prefix/lib/libzstd.a" "$prefix/lib/libarchive.a"
