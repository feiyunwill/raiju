#!/bin/sh

set -eu

PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

ARIA2_VERSION=1.37.0
ARIA2_SHA256=8e7021c6d5e8f8240c9cc19482e0c8589540836747744724d86bf8af5a21f0e8
ARIA2_HOST_TRIPLET=x86_64-unknown-freebsd12
MOTRIX_TAG=v1.37.0-motrix.17
MOTRIX_SHA256=bf22fae53b3a186c7d227cd37ef82422601764af902337d27622c72349ae8789

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
	if ! "$PS5_PAYLOAD_SDK/bin/prospero-clang++" --version >/dev/null 2>&1; then
		echo "the SDK's clang++ cannot find llvm-config:" >&2
		echo "put llvm-config-18 on PATH, or set LLVM_CONFIG to it" >&2
		exit 1
	fi
}

# Fails early when the prefix lacks the libraries aria2 links against, which build-tls.sh builds.
check_dependencies() {
	for library in libssl.a libcrypto.a libz.a; do
		if [ ! -f "$prefix/lib/$library" ]; then
			echo "$prefix/lib/$library is missing: run build-tls.sh first" >&2
			exit 1
		fi
	done
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

# Unpacks a cached tarball into the source tree, at the depth the archive uses.
unpack() {
	archive="$1"
	name="$2"
	depth="$3"

	rm -rf "$src/$name"
	mkdir -p "$src"
	if [ "$depth" = 1 ]; then
		mkdir -p "$src/$name"
		tar -C "$src/$name" --strip-components=1 -xzf "$cache/$archive"
	else
		tar -C "$src" -xzf "$cache/$archive"
	fi
}

# Configures one aria2 tree for the console against the prefix's OpenSSL and zlib.
configure_aria2() {
	./configure --host="$ARIA2_HOST_TRIPLET" --prefix="$prefix" \
		CC="$PS5_PAYLOAD_SDK/bin/prospero-clang" \
		CXX="$PS5_PAYLOAD_SDK/bin/prospero-clang++" \
		AR="$PS5_PAYLOAD_SDK/bin/prospero-ar" \
		RANLIB="$PS5_PAYLOAD_SDK/bin/prospero-ranlib" \
		CFLAGS="-fPIC -O2 -I$prefix/include" \
		CXXFLAGS="-fPIC -O2 -I$prefix/include" \
		LDFLAGS="-L$prefix/lib" \
		OPENSSL_CFLAGS="-I$prefix/include" \
		OPENSSL_LIBS="-L$prefix/lib -lssl -lcrypto" \
		--enable-static --disable-shared --disable-nls \
		PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" \
		--without-gnutls --with-openssl --with-libz --without-libxml2 \
		--without-libexpat --without-sqlite3 --enable-libaria2
}

# Builds upstream aria2 as a static aria2c and libaria2 for the console.
build_upstream_aria2() {
	[ -f "$prefix/bin/aria2c" ] && return 0

	unpack "aria2-$ARIA2_VERSION.tar.gz" "aria2-$ARIA2_VERSION" 0
	(
		cd "$src/aria2-$ARIA2_VERSION"
		patch -p1 -i "$here/patches/aria2-connections.patch"
		configure_aria2
		make -j"$jobs"
		make install
	)
}

# Builds the motrixapp fork the same way, into its own prefix beside the upstream one. The fork's
# archive is a source tree without generated build files, so its configure script has to be made,
# which is why autotools is needed for this one and not for the upstream release tarball. Only the
# #4 benchmark compares against it, so it is built when RAIJU_MOTRIX_BENCHMARK=1: the console links
# the upstream library alone.
build_motrix_aria2() {
	motrix_prefix="$prefix/motrix"

	[ -f "$motrix_prefix/bin/aria2c" ] && return 0

	unpack "aria2-$MOTRIX_TAG.tar.gz" "aria2-$MOTRIX_TAG" 1
	(
		cd "$src/aria2-$MOTRIX_TAG"
		if [ ! -x ./configure ]; then
			if ! command -v autoreconf >/dev/null 2>&1; then
				echo "the motrixapp archive ships no configure, and autoreconf is missing:" >&2
				echo "install autotools, for example brew install autoconf automake gettext" >&2
				exit 1
			fi
			touch ABOUT-NLS
			AUTOPOINT=true autoreconf -i
		fi
		prefix="$motrix_prefix"
		configure_aria2
		make -j"$jobs"
		make install
	)
}

mkdir -p "$cache" "$src" "$prefix"

check_toolchain
check_dependencies

fetch "https://github.com/aria2/aria2/releases/download/release-$ARIA2_VERSION/aria2-$ARIA2_VERSION.tar.gz" \
	"$cache/aria2-$ARIA2_VERSION.tar.gz" "$ARIA2_SHA256"

build_upstream_aria2

echo "prefix: $prefix"
ls -l "$prefix/bin/aria2c"

if [ "${RAIJU_MOTRIX_BENCHMARK:-0}" = "1" ]; then
	fetch "https://github.com/motrixapp/aria2/archive/refs/tags/$MOTRIX_TAG.tar.gz" \
		"$cache/aria2-$MOTRIX_TAG.tar.gz" "$MOTRIX_SHA256"
	build_motrix_aria2
	ls -l "$prefix/motrix/bin/aria2c"
fi
