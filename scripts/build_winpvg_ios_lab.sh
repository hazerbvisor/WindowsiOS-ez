#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REV=1fe2e752c98207518a99c9322482f63ebb9f0232
: "${WPVG_IOS_LLVM_PREFIX:?Set ARM64 iphoneos LLVM 15.0.7 prefix}"
: "${WPVG_FIXTURES:?Set directory containing trusted FXC-built vertex.cso, red.cso, green.cso}"
LLVM="$(cd "$WPVG_IOS_LLVM_PREFIX" && pwd)"
FIXTURES="$(cd "$WPVG_FIXTURES" && pwd)"
for c in git meson ninja xcrun python3 zip; do command -v "$c" >/dev/null; done
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]]
for v in 'MAJOR 15' 'MINOR 0' 'PATCH 7'; do
    grep -Eq "^#define LLVM_VERSION_$v$" "$LLVM/include/llvm/Config/llvm-config.h"
done
test -s "$LLVM/lib/libLLVMCore.a"
xcrun lipo -verify_arch arm64 "$LLVM/lib/libLLVMCore.a"
for shader in vertex red green; do test -s "$FIXTURES/$shader.cso"; done
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC="$(xcrun --sdk iphoneos --find clang)"
CXX="$(xcrun --sdk iphoneos --find clang++)"
AR="$(xcrun --sdk iphoneos --find ar)"
STRIP="$(xcrun --sdk iphoneos --find strip)"
WORK="$ROOT/work/winpvg-ios"
mkdir -p "$WORK"
CHECK="$(mktemp -d "$WORK/platform.XXXXXX")"
trap 'rm -rf "$CHECK"' EXIT
member="$("$AR" -t "$LLVM/lib/libLLVMCore.a" | sed -n '/\.o$/p' | sed -n '1p')"
test -n "$member"
(cd "$CHECK"; "$AR" -x "$LLVM/lib/libLLVMCore.a" "$member")
xcrun vtool -show-build "$CHECK/$member" | grep -Eq 'platform IOS$'
SRC="$(mktemp -d "$WORK/dxmt.XXXXXX")"
git -C "$SRC" init -q
git -C "$SRC" remote add origin https://github.com/utmapp/dxmt.git
git -C "$SRC" fetch --depth 1 origin "$REV"
git -C "$SRC" checkout --detach FETCH_HEAD
test "$(git -C "$SRC" rev-parse HEAD)" = "$REV"
git -C "$SRC" submodule update --init --recursive
cp "$ROOT/devices/wpvg_shader.mm" "$ROOT/tests/test_wpvg_dxbc.mm" "$ROOT/tests/ios/WpvgLabApp.mm" "$SRC/"
cat >> "$SRC/meson.build" <<'MESON'

if not dxmt_crossbuild and apple_subsystem == 'ios'
  add_languages('objcpp', required: true)
  wpvg_frameworks = dependency('appleframeworks', modules: ['Foundation', 'Metal', 'UIKit'])
  executable('WinPVGLab',
    ['wpvg_shader.mm', 'test_wpvg_dxbc.mm', 'WpvgLabApp.mm'],
    dependencies: [airconv_dep_darwin, dxbc_parser_native_dep, wpvg_frameworks],
    objcpp_args: ['-std=c++20', '-fobjc-arc', '-fblocks', '-DWPVG_IOS_APP=1'],
    link_language: 'cpp',
  )
endif
MESON
CROSS="$SRC/iphoneos.ini"
cat > "$CROSS" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
objc = '$CC'
objcpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
[host_machine]
system = 'darwin'
subsystem = 'ios'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = ['-target','arm64-apple-ios18.0','-isysroot','$SDK']
cpp_args = ['-target','arm64-apple-ios18.0','-isysroot','$SDK']
objc_args = ['-target','arm64-apple-ios18.0','-isysroot','$SDK']
objcpp_args = ['-target','arm64-apple-ios18.0','-isysroot','$SDK']
c_link_args = ['-target','arm64-apple-ios18.0','-isysroot','$SDK']
cpp_link_args = ['-target','arm64-apple-ios18.0','-isysroot','$SDK']
objc_link_args = ['-target','arm64-apple-ios18.0','-isysroot','$SDK']
objcpp_link_args = ['-target','arm64-apple-ios18.0','-isysroot','$SDK']
EOF
meson setup "$SRC/build" "$SRC" --cross-file "$CROSS" --buildtype release \
    -Dnative_llvm_path="$LLVM" -Denable_d3d12=false -Denable_tests=false
meson compile -C "$SRC/build" WinPVGLab
BIN="$SRC/build/WinPVGLab"
test -s "$BIN"
xcrun lipo -verify_arch arm64 "$BIN"
xcrun vtool -show-build "$BIN" | grep -Eq 'platform IOS$'
xcrun otool -L "$BIN" | tee "$SRC/dependencies.txt"
python3 - "$SRC/dependencies.txt" <<'PY'
import sys
for line in open(sys.argv[1]).read().splitlines()[1:]:
    name=line.strip().split(" (",1)[0]
    if not name.startswith(("/System/Library/","/usr/lib/")):
        raise SystemExit("Unexpected runtime dependency: "+name)
PY
APP="$SRC/package/Payload/WinPVGLab.app"
mkdir -p "$APP"
cp "$BIN" "$APP/WinPVGLab"
chmod +x "$APP/WinPVGLab"
for shader in vertex red green; do cp "$FIXTURES/$shader.cso" "$APP/"; done
python3 - "$APP" <<'PY'
import plistlib,sys,pathlib
app=pathlib.Path(sys.argv[1])
plist={"CFBundleExecutable":"WinPVGLab","CFBundleIdentifier":"io.github.hazerbvisor.winpvg.lab",
"CFBundleName":"WinPVG Lab","CFBundleDisplayName":"WinPVG Lab","CFBundlePackageType":"APPL",
"CFBundleShortVersionString":"0.1","CFBundleVersion":"1","MinimumOSVersion":"18.0",
"CFBundleSupportedPlatforms":["iPhoneOS"],"UIDeviceFamily":[1,2],
"LSRequiresIPhoneOS":True,"UILaunchScreen":{},"UIFileSharingEnabled":True,
"LSSupportsOpeningDocumentsInPlace":True,
"UISupportedInterfaceOrientations":["UIInterfaceOrientationPortrait","UIInterfaceOrientationLandscapeLeft","UIInterfaceOrientationLandscapeRight"],
"UISupportedInterfaceOrientations~ipad":["UIInterfaceOrientationPortrait","UIInterfaceOrientationPortraitUpsideDown","UIInterfaceOrientationLandscapeLeft","UIInterfaceOrientationLandscapeRight"]}
(app/"Info.plist").write_bytes(plistlib.dumps(plist))
PY
git -C "$ROOT" rev-parse HEAD > "$APP/WinPVG-revision.txt"
printf '%s\n' "$REV" > "$APP/DXMT-revision.txt"
for license in LICENSE LICENSE.txt COPYING; do
    if [[ -f "$SRC/$license" ]]; then cp "$SRC/$license" "$APP/DXMT-$license"; fi
done
# Audit all transitive license obligations before distributing.
OUT="$WORK/WinPVGLab-unsigned.ipa"
rm -f "$OUT"
(cd "$SRC/package"; zip -qr "$OUT" Payload)
shasum -a 256 "$OUT"
echo "Unsigned lab IPA: $OUT (requires normal app signing; no hardware test performed)"
