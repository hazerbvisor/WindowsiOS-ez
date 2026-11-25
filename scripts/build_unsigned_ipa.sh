#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
export PATH="/opt/homebrew/bin:$PATH"

if [ "$(uname -m)" != arm64 ] || [ "$(uname -s)" != Darwin ]; then
    echo "The device build requires Apple Silicon macOS and Xcode 27.2." >&2
    exit 1
fi

XCODE_VERSION="$(xcodebuild -version | sed -n '1s/^Xcode //p')"
if [ "$XCODE_VERSION" != "27.2" ]; then
    echo "Expected Xcode 27.2. Found: $XCODE_VERSION" >&2
    exit 1
fi

python3 scripts/prepare_installer_assets.py
./scripts/build_ios_deps.sh
make -j"$(sysctl -n hw.ncpu)" ios-lib
for archive in build/ios/libwinvm.a ios/deps/lib/{libslirp,libglib-2.0,libintl,libpcre2-8,libffi,libudfread,libfatfs,libwim,libtpms,libcrypto}.a; do
    test "$(xcrun lipo -archs "$archive")" = arm64
done

xcodegen generate --spec ios/project.yml
mkdir -p build/artifacts
# A fresh build directory prevents accidentally packaging an older product.
STAGE="$(mktemp -d "$ROOT/build/unsigned.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
PROJECT_ARGS=(-project ios/WindowsiOS.xcodeproj -scheme WindowsiOS
    -configuration Release -sdk iphoneos -destination 'generic/platform=iOS'
    -derivedDataPath "$STAGE/DerivedData"
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=
    DEVELOPMENT_TEAM= PROVISIONING_PROFILE_SPECIFIER=)
xcodebuild "${PROJECT_ARGS[@]}" -showBuildSettings -json > "$STAGE/settings.json"
python3 scripts/verify_ios_artifact.py settings "$STAGE/settings.json" ios/WindowsiOS.entitlements

xcodebuild "${PROJECT_ARGS[@]}" LD_GENERATE_MAP_FILE=YES \
    "LD_MAP_FILE_PATH=$STAGE/link-map.txt" build

APP="$STAGE/DerivedData/Build/Products/Release-iphoneos/WindowsiOS.app"
test "$(xcrun lipo -archs "$APP/WindowsiOS")" = arm64
xcrun vtool -show-build "$APP/WindowsiOS" | awk '$1 == "platform" && $2 == "IOS" { found = 1 } END { exit !found }'
if codesign -d "$APP" >/dev/null 2>&1; then
    echo "Unexpected signature on app." >&2
    exit 1
fi
for archive in libwinvm.a libslirp.a libglib-2.0.a libudfread.a libfatfs.a libwim.a libtpms.a libcrypto.a; do
    if ! grep -Fq "/$archive(" "$STAGE/link-map.txt"; then
        echo "No linked objects from $archive found in the application link map." >&2
        exit 1
    fi
done

mkdir "$STAGE/Payload"
ditto "$APP" "$STAGE/Payload/WindowsiOS.app"
(
    cd "$STAGE"
    zip -qry WindowsiOS-ez.ipa Payload
)
python3 scripts/verify_ios_artifact.py ipa "$STAGE/WindowsiOS-ez.ipa" ios/WindowsiOS.entitlements
cp ios/WindowsiOS.entitlements "$STAGE/WindowsiOS-ez.entitlements"
(
    cd "$STAGE"
    shasum -a 256 WindowsiOS-ez.ipa WindowsiOS-ez.entitlements > SHA256SUMS
    shasum -a 256 -c SHA256SUMS
)
mv "$STAGE/WindowsiOS-ez.ipa" "$STAGE/WindowsiOS-ez.entitlements" "$STAGE/SHA256SUMS" build/artifacts/
