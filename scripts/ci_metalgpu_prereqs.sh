#!/bin/bash
set -euo pipefail
# Reuse pinned UTM cross-build functions, only for ANGLE, epoxy and LLVM.
# Never build or embed UTM/QEMU itself.
ROOT="$(pwd)"
UTM="$RUNNER_TEMP/metalgpu-utm"
REV=3d4b8d8cb1ce93a8b1a9610695c12487685f0c15
git init "$UTM"
git -C "$UTM" remote add origin https://github.com/utmapp/UTM.git
git -C "$UTM" fetch --depth 1 origin "$REV"
git -C "$UTM" checkout --detach FETCH_HEAD
test "$(git -C "$UTM" rev-parse HEAD)" = "$REV"
cd "$UTM"
python3 - <<'PY'
from pathlib import Path
p=Path("scripts/build_dependencies.sh")
s=p.read_text()
s=s.replace('IOS_SDKMINVER="15.0"', 'IOS_SDKMINVER="17.0"')
# Preserve the workflow-selected Xcode when upstream sanitizes its environment.
old = 'env -i PATH=$PATH xcodebuild archive'
assert s.count(old) == 1
s=s.replace(old, 'env -i PATH="$PATH" DEVELOPER_DIR="$DEVELOPER_DIR" HOME="$HOME" xcodebuild archive')
a=s.index("\ncheck_env\necho")
# The original script has completed target, toolchain and environment setup here.
# The selected dependency build uses its original pinned source and patches.
s=s[:a]+'''
mkdir -p "$BUILD_DIR" "$PREFIX/include" "$PREFIX/lib" "$PREFIX/Frameworks"
download $PKG_CONFIG_SRC
clone $WEBKIT_REPO $WEBKIT_COMMIT "$WEBKIT_SUBDIRS"
# The pinned WebKit configuration uses a removed libc++ macro.
# Preserve hardening rather than disable assertions to get a build.
python3 "$ROOT/scripts/patch_angle_xcode27.py" "$BUILD_DIR/WebKit.git"
clone $EPOXY_REPO $EPOXY_COMMIT
download $LLVM15_SRC
copy_private_headers
build_pkg_config
build_angle
meson_build $EPOXY_REPO -Dtests=false -Dglx=no -Degl=yes
LLVM15_NAME=$(basename "${LLVM15_SRC%.tar.*}")
cmake_build "$BUILD_DIR/$LLVM15_NAME/llvm" -DLLVM_ENABLE_ZSTD=Off -DLLVM_TARGETS_TO_BUILD="" -DLLVM_BUILD_TOOLS=Off -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off -DLLVM_ENABLE_TERMINFO=Off -DLLVM_ENABLE_PROJECTS="" -DLLVM_INCLUDE_TESTS=Off -DLLVM_INCLUDE_EXAMPLES=Off -DLLVM_INCLUDE_BENCHMARKS=Off
'''
p.with_name("build_metalgpu_prereqs.sh").write_text(s)
PY
bash scripts/build_metalgpu_prereqs.sh -p ios -a arm64
PREFIX="$UTM/sysroot-iOS-arm64"
test -f "$PREFIX/include/llvm/Config/llvm-config.h"
test -f "$PREFIX/lib/libLLVMCore.a"
test -f "$PREFIX/lib/libEGL.dylib"
echo "DXMT_IOS_LLVM_PREFIX=$PREFIX" >> "$GITHUB_ENV"
echo "NEPTUNE_IOS_DEPS_PREFIX=$PREFIX" >> "$GITHUB_ENV"
