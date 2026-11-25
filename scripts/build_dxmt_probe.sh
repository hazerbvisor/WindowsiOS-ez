#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Standalone, opt-in DXMT dependency probe. Does not link the VM or run CI.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REV=1fe2e752c98207518a99c9322482f63ebb9f0232
: "${DXMT_IOS_LLVM_PREFIX:?Set to an arm64 iPhoneOS LLVM 15.0.7 install prefix (not Homebrew macOS LLVM)}"
LLVM="$(cd "$DXMT_IOS_LLVM_PREFIX" && pwd)"
[[ -f "$LLVM/include/llvm/Config/llvm-config.h" && -f "$LLVM/lib/libLLVMCore.a" ]]
grep -Eq '^#define LLVM_VERSION_MAJOR +15$' "$LLVM/include/llvm/Config/llvm-config.h"
grep -Eq '^#define LLVM_VERSION_MINOR +0$' "$LLVM/include/llvm/Config/llvm-config.h"
grep -Eq '^#define LLVM_VERSION_PATCH +7$' "$LLVM/include/llvm/Config/llvm-config.h"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC="$(xcrun --sdk iphoneos --find clang)"
CXX="$(xcrun --sdk iphoneos --find clang++)"
AR="$(xcrun --sdk iphoneos --find ar)"
STRIP="$(xcrun --sdk iphoneos --find strip)"
WORK="$ROOT/work/metalgpu-dxmt/$REV"
mkdir -p "$WORK"
SRC="$WORK/src"
if [[ ! -d "$SRC/.git" ]]; then git clone --no-checkout https://github.com/utmapp/dxmt.git "$SRC"; fi
git -C "$SRC" checkout --detach "$REV"
git -C "$SRC" submodule update --init --recursive
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$REV" ]]
# Prevent accidental use of macOS libraries through pkg-config.
export PKG_CONFIG_LIBDIR="$LLVM/lib/pkgconfig:$LLVM/share/pkgconfig"
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
c_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK']
cpp_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK']
c_link_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK']
cpp_link_args = ['-target','arm64-apple-ios17.0','-isysroot','$SDK']
EOF
OUT="$WORK/install"
BUILD="$WORK/build"
if [[ -d "$BUILD/meson-private" ]]; then
    meson setup --reconfigure "$BUILD" "$SRC" --cross-file "$CROSS" --prefix "$OUT" --libdir lib --buildtype release -Dnative_llvm_path="$LLVM" -Denable_d3d12=false -Denable_tests=false
else
    meson setup "$BUILD" "$SRC" --cross-file "$CROSS" --prefix "$OUT" --libdir lib --buildtype release -Dnative_llvm_path="$LLVM" -Denable_d3d12=false -Denable_tests=false
fi
meson compile -C "$BUILD"
meson install -C "$BUILD"
LIB="$OUT/lib/libdxmt-native.dylib"
test -s "$LIB"
xcrun lipo -verify_arch arm64 "$LIB"
xcrun vtool -show-build "$LIB" | tee "$WORK/platform.txt"
grep -Eq 'platform IOS$' "$WORK/platform.txt"
xcrun otool -L "$LIB" | tee "$WORK/dependencies.txt"
xcrun nm -gU "$LIB" | tee "$WORK/exports.txt"
grep -q '_D3D11CreateDevice' "$WORK/exports.txt"
if grep -q '_D3D12CreateDevice' "$WORK/exports.txt"; then echo "Unexpected DX12 export" >&2; exit 1; fi
echo "DXMT iPhoneOS binary built at $LIB; runtime link/signing and Neptune integration remain separate gates."
