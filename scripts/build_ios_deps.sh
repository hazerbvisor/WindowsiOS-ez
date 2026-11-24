#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Cross-compile libffi, GLib and libslirp for arm64-apple-ios into ios/deps/.
set -euo pipefail

export PATH="/opt/homebrew/bin:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DEPS="$ROOT/ios/deps"
LIBS=(libslirp.a libglib-2.0.a libintl.a libpcre2-8.a libffi.a)

ARCH=arm64
SDK=iphoneos
SDKMINVER=17.0
NCPU="$(sysctl -n hw.ncpu)"
SDKROOT="$(xcrun --sdk "$SDK" --show-sdk-path)"
PYTHON="$(command -v python3)"
KEY="$({
    shasum -a 256 < "$ROOT/scripts/build_ios_deps.sh"
    xcodebuild -version
    xcrun --sdk "$SDK" --show-sdk-build-version
    meson --version
    ninja --version
    pkg-config --version
    "$PYTHON" --version
    printf '%s\n' "$ARCH" "$SDK" "$SDKMINVER"
} | shasum -a 256 | cut -d ' ' -f 1)"

cache_ready() {
    [ -f "$OUT_DEPS/build-key" ] && [ "$(cat "$OUT_DEPS/build-key")" = "$KEY" ] || return 1
    [ -s "$OUT_DEPS/include/slirp/libslirp.h" ] &&
        [ -s "$OUT_DEPS/include/slirp/libslirp-version.h" ] || return 1
    for lib in "${LIBS[@]}"; do
        [ -s "$OUT_DEPS/lib/$lib" ] || return 1
    done
}

if cache_ready; then
    echo "Using cached ARM64 iOS dependencies ($KEY)"
    exit 0
fi

WORK_DEPS="$ROOT/work/ios_deps_build/$KEY"
DL="$WORK_DEPS/sources"
SRC="$WORK_DEPS/src"
PREFIX="$WORK_DEPS/sysroot"
STAMPS="$WORK_DEPS/stamps"
mkdir -p "$DL" "$SRC" "$PREFIX" "$STAMPS" "$OUT_DEPS/lib" "$OUT_DEPS/include"

CFLAGS_TARGET="-target $ARCH-apple-ios$SDKMINVER"

export CC="$(xcrun --sdk $SDK --find clang) $CFLAGS_TARGET"
export CXX="$(xcrun --sdk $SDK --find clang++) $CFLAGS_TARGET"
export OBJCC="$(xcrun --sdk $SDK --find clang)"
export AR="$(xcrun --sdk $SDK --find ar)"
export NM="$(xcrun --sdk $SDK --find nm)"
export RANLIB="$(xcrun --sdk $SDK --find ranlib)"
export STRIP="$(xcrun --sdk $SDK --find strip)"

export CFLAGS="-arch $ARCH -isysroot $SDKROOT -I$PREFIX/include $CFLAGS_TARGET -O2"
export CXXFLAGS="$CFLAGS"
export CPPFLAGS="-arch $ARCH -isysroot $SDKROOT -I$PREFIX/include $CFLAGS_TARGET"
export OBJCFLAGS="$CFLAGS"
export LDFLAGS="-arch $ARCH -isysroot $SDKROOT -L$PREFIX/lib $CFLAGS_TARGET"

export PKG_CONFIG="$(command -v pkg-config)"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
unset PKG_CONFIG_PATH

FFI_URL="https://github.com/libffi/libffi/releases/download/v3.5.0/libffi-3.5.0.tar.gz"
GLIB_URL="https://download.gnome.org/sources/glib/2.83/glib-2.83.0.tar.xz"
SLIRP_URL="https://github.com/utmapp/libslirp/releases/download/v4.9.1-release-mirror/libslirp-v4.9.1.tar.gz"

fetch_unpack() {
    local url="$1" checksum="$2"
    local fname
    fname="$(basename "$url")"
    if [ ! -s "$DL/$fname" ]; then
        echo "[fetch] $fname"
        curl -fL --retry 3 -o "$DL/$fname.part" "$url"
        printf '%s  %s\n' "$checksum" "$DL/$fname.part" | shasum -a 256 -c -
        mv "$DL/$fname.part" "$DL/$fname"
    fi
    printf '%s  %s\n' "$checksum" "$DL/$fname" | shasum -a 256 -c -
    if [ ! -f "$STAMPS/unpack-$fname" ]; then
        echo "[unpack] $fname"
        tar -xf "$DL/$fname" -C "$SRC"
        touch "$STAMPS/unpack-$fname"
    fi
}

fetch_unpack "$FFI_URL" 8c72678628a5dd8782f08ad421d5a441e42c1c5c1b33e0bc211cbfcf1f3b3978
fetch_unpack "$GLIB_URL" a07d9e1a57a4279c5ece71c26dc44eea12bd518ea9ff695d53e722997032b614
fetch_unpack "$SLIRP_URL" 3970542143b7c11e6a09a4d2b50f30a133473c41f15ed0bdcc3b7a1c450d9a5c

meson_quote() { echo "'$(echo $* | sed "s/ /','/g")'"; }

gen_cross() {
    local cross="$1" system="$2"
    cat > "$cross" <<EOF
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = [$(meson_quote $CFLAGS)]
cpp_args = [$(meson_quote $CXXFLAGS)]
objc_args = [$(meson_quote $OBJCFLAGS)]
objcpp_args = [$(meson_quote $OBJCFLAGS)]
c_link_args = [$(meson_quote $LDFLAGS)]
cpp_link_args = [$(meson_quote $LDFLAGS)]
objc_link_args = [$(meson_quote $LDFLAGS)]
objcpp_link_args = [$(meson_quote $LDFLAGS)]
[binaries]
c = [$(meson_quote $CC)]
cpp = [$(meson_quote $CXX)]
objc = [$(meson_quote $OBJCC)]
objcpp = [$(meson_quote $OBJCC)]
ar = ['$AR']
nm = ['$NM']
ranlib = ['$RANLIB']
strip = ['$STRIP', '-x']
pkg-config = ['$PKG_CONFIG']
pkgconfig = ['$PKG_CONFIG']
python = ['$PYTHON']
[host_machine]
system = '$system'
EOF
    if [ "$system" = "darwin" ]; then
        echo "kernel = 'xnu'" >> "$cross"
        echo "subsystem = 'ios'" >> "$cross"
    fi
    cat >> "$cross" <<EOF
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
}

CROSS_IOS="$WORK_DEPS/cross-ios.meson"
CROSS_DARWIN="$WORK_DEPS/cross-darwin.meson"
gen_cross "$CROSS_IOS" ios
gen_cross "$CROSS_DARWIN" darwin

show_failure() {
    tail -n 150 "$1" >&2
    exit 1
}

# 1. libffi
if [ ! -f "$STAMPS/libffi" ]; then
    echo "[build] libffi-3.5.0"
    ( cd "$SRC/libffi-3.5.0" \
      && ./configure --host=aarch64-apple-darwin --prefix="$PREFIX" --enable-static --disable-shared \
      && make -j"$NCPU" \
      && make install ) > "$WORK_DEPS/libffi.log" 2>&1 || show_failure "$WORK_DEPS/libffi.log"
    touch "$STAMPS/libffi"
fi

# 2. glib (automatically fetches/builds subprojects proxy-libintl & pcre2 if needed)
if [ ! -f "$STAMPS/glib" ]; then
    echo "[build] glib-2.83.0"
    rm -rf "$SRC/glib-2.83.0/_ios_build"
    ( cd "$SRC/glib-2.83.0" \
      && meson setup _ios_build --cross-file "$CROSS_IOS" --prefix="$PREFIX" \
            --buildtype=release --default-library=static \
            -Dtests=false -Dnls=disabled -Dintrospection=disabled \
            -Dselinux=disabled -Dlibmount=disabled -Ddtrace=disabled \
            -Dman-pages=disabled -Dglib_debug=disabled -Dxattr=false \
            --force-fallback-for=intl,libpcre2-8 \
      && meson compile -C _ios_build -j "$NCPU" \
      && meson install -C _ios_build --no-rebuild ) > "$WORK_DEPS/glib.log" 2>&1 || show_failure "$WORK_DEPS/glib.log"
    touch "$STAMPS/glib"
fi

# 3. libslirp
if [ ! -f "$STAMPS/libslirp" ]; then
    echo "[build] libslirp-v4.9.1"
    rm -rf "$SRC/libslirp-v4.9.1/_ios_build"
    ( cd "$SRC/libslirp-v4.9.1" \
      && meson setup _ios_build --cross-file "$CROSS_DARWIN" --prefix="$PREFIX" \
            --buildtype=release --default-library=static \
      && meson compile -C _ios_build -j "$NCPU" slirp \
      && meson install -C _ios_build --no-rebuild ) > "$WORK_DEPS/libslirp.log" 2>&1 || show_failure "$WORK_DEPS/libslirp.log"
    touch "$STAMPS/libslirp"
fi

# Stage only a complete set of headers and libraries.
for lib in "${LIBS[@]}"; do
    if [ ! -s "$PREFIX/lib/$lib" ]; then
        echo "Missing cross-compiled dependency: $PREFIX/lib/$lib" >&2
        exit 1
    fi
done
rm -f "$OUT_DEPS/build-key"
rm -rf "$OUT_DEPS/include/slirp"
cp -R "$PREFIX/include/slirp" "$OUT_DEPS/include/slirp"
for lib in "${LIBS[@]}"; do
    cp "$PREFIX/lib/$lib" "$OUT_DEPS/lib/$lib"
done
printf '%s\n' "$KEY" > "$OUT_DEPS/build-key"
echo "=== ARM64 iOS dependencies staged in $OUT_DEPS ==="
