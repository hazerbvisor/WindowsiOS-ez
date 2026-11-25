#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Manual iPhoneOS-only Neptune renderer build, separate from the baseline IPA.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REV=dd0652b13e589c4c124ea79123a3adcd85173f4f
DXMT_REV=1fe2e752c98207518a99c9322482f63ebb9f0232
: "${NEPTUNE_IOS_DEPS_PREFIX:?Set an iPhoneOS arm64 dependency prefix with libepoxy, epoll-shim and ANGLE EGL headers/libraries}"
DEPS="$(cd "$NEPTUNE_IOS_DEPS_PREFIX" && pwd)"
DXMT="${WINIOS_GPU_PREBUILT_PREFIX:-$ROOT/work/metalgpu-dxmt/$DXMT_REV/install}/lib/libdxmt-native.dylib"
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
if [[ "${WINIOS_PATCH_NEPTUNE_SHMEM:-0}" == 1 ]]; then
    python3 "$ROOT/scripts/patch_neptune_container_shmem.py" "$SRC"
    TESTDIR="$(mktemp -d "$WORK/shmem-test.XXXXXX")"
    xcrun --sdk macosx clang -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
        -I"$SRC/src/mesa/util" "$SRC/src/mesa/util/anon_file.c" \
        "$ROOT/tests/test_neptune_container_shmem.c" -o "$WORK/test-neptune-shmem"
    "$WORK/test-neptune-shmem" "$TESTDIR"
    rmdir "$TESTDIR"
fi
export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig:$DEPS/share/pkgconfig"
unset PKG_CONFIG_PATH
CROSS="$WORK/iphoneos.ini"
cat > "$CROSS" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = '$(command -v pkg-config)'
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['$DEPS/lib/pkgconfig','$DEPS/share/pkgconfig']
[built-in options]
c_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK','-I$DEPS/include']
cpp_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK','-I$DEPS/include']
c_link_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK','-L$DEPS/lib']
cpp_link_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK','-L$DEPS/lib']
EOF
BUILD="$WORK/build"
OPTIONS=(-Dneptune=true -Dvenus=false -Dcheck-gl-errors=false -Dvulkan-dload=false -Drender-server-mode=thread -Drender-server-worker=thread -Dplatforms=egl -Dunstable-apis=true -Dvtest=false -Dtests=false -Dvideo=false -Ddefault_library=shared)
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
    -DWINIOS_ENABLE_NEPTUNE -Wall -Wextra -Werror -I"$HDR" -I"$DEPS/include" -I"$ROOT/core" -I"$ROOT/host" -I"$ROOT/devices" \
    -c "$ROOT/devices/neptune_backend.c" -o "$WORK/neptune_backend.o"
"$CC" -target arm64-apple-ios17.0 -isysroot "$SDK" -std=c11 -Wall -Wextra -Werror \
    -c "$ROOT/devices/neptune_wire.c" -o "$WORK/neptune_wire.o"
"$CC" -target arm64-apple-ios17.0 -isysroot "$SDK" -std=c11 -Wall -Wextra -Werror -I"$DEPS/include" \
    -c "$ROOT/devices/neptune_angle.c" -o "$WORK/neptune_angle.o"
echo "Native renderer, DXMT adapter, ANGLE shadow and VirtIO decoder compiled. Device build/test remains required."
