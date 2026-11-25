#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Package the pinned upstream native offscreen suite for a separate physical-device test.
# Run only on an owner-authorized Xcode build machine; never triggers CI.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REV=1fe2e752c98207518a99c9322482f63ebb9f0232
WORK="$ROOT/work/metalgpu-dxmt/$REV"
SRC="$WORK/src"
LIB="$WORK/install/lib/libdxmt-native.dylib"
test -s "$LIB"
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$REV" ]]
[[ -z "$(git -C "$SRC" status --porcelain --untracked-files=no)" ]]
xcrun lipo -verify_arch arm64 "$LIB"
xcrun vtool -show-build "$LIB" | grep -Eq 'platform IOS$'
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
STAGE="$(mktemp -d "$WORK/smoke.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
APP="$STAGE/Payload/DXMTSmoke.app"
mkdir -p "$APP/Frameworks"
cp "$LIB" "$APP/Frameworks/libdxmt-native.dylib"
OLD_ID="$(xcrun otool -D "$LIB" | sed -n '2p')"
test -n "$OLD_ID"
xcrun install_name_tool -id @rpath/libdxmt-native.dylib "$APP/Frameworks/libdxmt-native.dylib"
# Do not package unverified third-party dylib dependencies. System frameworks are provided by iOS.
while IFS= read -r dependency; do
    case "$dependency" in
        "$OLD_ID"|@rpath/libdxmt-native.dylib|/System/Library/*|/usr/lib/*) ;;
        *) echo "Unresolved DXMT dependency: $dependency" >&2; exit 1 ;;
    esac
done < <(xcrun otool -L "$LIB" | tail -n +2 | awk '{print $1}')
# Compile the upstream checked-in tests and embedded DXBC shaders, without any macOS runtime.
xcrun --sdk iphoneos clang++ -target arm64-apple-ios17.0 -isysroot "$SDK" -std=c++20 -O1 \
    -I"$SRC/include/native/windows" -I"$SRC/include/native/directx" \
    "$SRC/tests/native/d3d11_test.cpp" -o "$APP/DXMTSmoke" \
    -L"$APP/Frameworks" -ldxmt-native -Wl,-rpath,@executable_path/Frameworks
xcrun lipo -verify_arch arm64 "$APP/DXMTSmoke"
xcrun vtool -show-build "$APP/DXMTSmoke" | grep -Eq 'platform IOS$'
cat > "$APP/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>org.windowsios.dxmtsmoke</string>
<key>CFBundleExecutable</key><string>DXMTSmoke</string>
<key>CFBundleName</key><string>DXMT Smoke</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>0.1</string>
<key>MinimumOSVersion</key><string>17.0</string>
<key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
<key>UIFileSharingEnabled</key><true/>
<key>LSSupportsOpeningDocumentsInPlace</key><true/>
<key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
</dict></plist>
PLIST
plutil -lint "$APP/Info.plist"
# This is a separate unsigned developer probe, not the WindowsiOS-ez app.
OUT="$WORK/DXMTSmoke-unsigned.ipa"
TMP="$STAGE/DXMTSmoke-unsigned.ipa"
(cd "$STAGE" && zip -q -r "$TMP" Payload)
unzip -tq "$TMP"
mv -f "$TMP" "$OUT"
echo "$OUT"
echo "Sign app and embedded dylib with a development identity; launch using a supported developer launch tool."
echo "Collect Documents/dxmt_results.txt and dxmt_stderr.txt. A successful build alone does not prove GPU execution."
