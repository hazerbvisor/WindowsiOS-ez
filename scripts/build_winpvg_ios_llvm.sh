#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build LLVM 15.0.7 static libraries for the physical iPhoneOS target.
# Requires an ARM64 Mac, Xcode, CMake and Ninja. Does not build WindowsiOS.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]]
for c in cmake ninja git xcrun; do command -v "$c" >/dev/null; done
WORK="$ROOT/work/winpvg-llvm"
SRC="$WORK/llvm-project"
mkdir -p "$WORK"
if [[ ! -d "$SRC/.git" ]]; then
    git clone --depth 1 --branch llvmorg-15.0.7 https://github.com/llvm/llvm-project.git "$SRC"
fi
test "$(git -C "$SRC" describe --tags --exact-match HEAD)" = llvmorg-15.0.7
test -z "$(git -C "$SRC" status --porcelain)"
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD= \
    -DLLVM_ENABLE_PROJECTS= -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_BUILD_TOOLS=OFF)
# Build the native TableGen executable; the iOS build must not execute
# cross-compiled tools on the Mac.
cmake -S "$SRC/llvm" -B "$WORK/host" "${COMMON[@]}" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)"
cmake --build "$WORK/host" --target llvm-tblgen
TABLEGEN="$WORK/host/bin/llvm-tblgen"
test -x "$TABLEGEN"
PREFIX="$WORK/install-iphoneos-arm64"
# LLVM 15 add_tablegen installs the target executable when BUILD_UTILS
# is ON. iOS CMake defaults executables to bundles; this build needs only
# static libraries and the already-built native TableGen executable.
cmake -S "$SRC/llvm" -B "$WORK/iphoneos" "${COMMON[@]}" \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_MACOSX_BUNDLE=OFF \
    -DLLVM_BUILD_UTILS=OFF -DLLVM_INSTALL_UTILS=OFF \
    -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_TOOLS=OFF \
    -DCMAKE_OSX_SYSROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 \
    -DCMAKE_C_COMPILER="$(xcrun --sdk iphoneos --find clang)" \
    -DCMAKE_CXX_COMPILER="$(xcrun --sdk iphoneos --find clang++)" \
    -DLLVM_HOST_TRIPLE=arm64-apple-ios18.0 \
    -DLLVM_DEFAULT_TARGET_TRIPLE=arm64-apple-ios18.0 \
    -DLLVM_TABLEGEN="$TABLEGEN" -DLLVM_ENABLE_ZLIB=ON \
    -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$WORK/iphoneos"
cmake --install "$WORK/iphoneos"
test -s "$PREFIX/lib/libLLVMCore.a"
xcrun lipo -verify_arch arm64 "$PREFIX/lib/libLLVMCore.a"
printf 'LLVM prefix: %s\n' "$PREFIX"
