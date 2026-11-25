#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Manual iPhoneOS-only Neptune renderer build, separate from the baseline IPA.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REV=dd0652b13e589c4c124ea79123a3adcd85173f4f
DXMT_REV=1fe2e752c98207518a99c9322482f63ebb9f0232
: "${NEPTUNE_IOS_DEPS_PREFIX:?Set an iPhoneOS arm64 dependency prefix with libepoxy and epoll-shim}"
DEPS="$(cd "$NEPTUNE_IOS_DEPS_PREFIX" && pwd)"
DXMT="$ROOT/work/metalgpu-dxmt/$DXMT_REV/install/lib/libdxmt-native.dylib"
test -s "$DXMT"
xcrun lipo -verify_arch arm64 "$DXMT"
xcrun vtool -show-build "$DXMT" | grep -Eq 'platform IOS$'
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC="$(xcrun --sdk iphoneos --find clang)"
CXX="$(xcrun --sdk iphoneos --find clang++)"
AR="$(xcrun --sdk iphoneos --find ar)"
STRIP="$(xcrun --sdk iphoneos --find strip)"
WORK="$ROOT/work/metalgpu-neptune/$REV"
SRC="$WORK/src"
OUT="$WORK/install"
mkdir -p "$WORK"
if [[ ! -d "$SRC/.git" ]]; then git clone --no-checkout https://github.com/utmapp/virglrenderer.git "$SRC"; fi
git -C "$SRC" checkout --detach "$REV"
git -C "$SRC" submodule update --init --recursive
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$REV" ]]
export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig:$DEPS/share/pkgconfig"
unset PKG_CONFIG_PATH
CROSS="$WORK/iphoneos.ini"
cat > "$CROSS" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK','-I$DEPS/include']
cpp_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK','-I$DEPS/include']
c_link_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK','-L$DEPS/lib']
cpp_link_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK','-L$DEPS/lib']
EOF
BUILD="$WORK/build"
OPTIONS=(-Dneptune=true -Dvenus=false -Dvulkan-dload=false -Drender-server-mode=thread -Drender-server-worker=thread -Dplatforms=[] -Dvtest=false -Dtests=false -Dvideo=false -Ddefault_library=shared)
if [[ -d "$BUILD/meson-private" ]]; then
  meson setup --reconfigure "$BUILD" "$SRC" --cross-file "$CROSS" --prefix "$OUT" --libdir lib --buildtype release "${OPTIONS[@]}"
else
  meson setup "$BUILD" "$SRC" --cross-file "$CROSS" --prefix "$OUT" --libdir lib --buildtype release "${OPTIONS[@]}"
fi
meson compile -C "$BUILD"
meson install -C "$BUILD"
LIB="$OUT/lib/libvirglrenderer.dylib"
test -s "$LIB"
xcrun lipo -verify_arch arm64 "$LIB"
xcrun vtool -show-build "$LIB" | grep -Eq 'platform IOS$'
xcrun otool -L "$LIB" | tee "$WORK/dependencies.txt"
# Compile the actual WindowsiOS adapter against the pinned public API.
# It intentionally remains out of the normal VM until PCI mappings and
# scanout lifetime have been validated.
HDR="$OUT/include/virgl"
if [[ ! -f "$HDR/virglrenderer.h" ]]; then HDR="$OUT/include"; fi
test -f "$HDR/virglrenderer.h"
"$CC" -target arm64-apple-ios17.0 -isysroot "$SDK" -std=c11 -D_DARWIN_C_SOURCE \
    -DWINIOS_ENABLE_NEPTUNE -Wall -Wextra -Werror -I"$HDR" \
    -c "$ROOT/devices/neptune_backend.c" -o "$WORK/neptune_backend.o"
"$CC" -target arm64-apple-ios17.0 -isysroot "$SDK" -std=c11 -Wall -Wextra -Werror \
    -c "$ROOT/devices/neptune_wire.c" -o "$WORK/neptune_wire.o"
echo "Native renderer, WindowsiOS adapter and staged VirtIO decoder compiled. No guest capabilities enabled."
