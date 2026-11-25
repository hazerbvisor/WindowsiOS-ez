#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
DEST="$RUNNER_TEMP/metalgpu-precompiled"
mkdir -p "$DEST"
# UTM build 44272292460, commit 3d4b8d8cb1ce93a8b1a9610695c12487685f0c15.
# Its pinned sources match our DXMT and virglrenderer adapter ABI.
URL=https://api.github.com/repos/utmapp/UTM/actions/artifacts/14412813611/zip
SHA=7793bd74af863b48226b63bca88a94c520dd3c8ee905836a515333f1923be4b3
curl --fail --silent --show-error --location --retry 3 \
    --header "Authorization: Bearer $GH_TOKEN" \
    --header "Accept: application/vnd.github+json" \
    --output "$DEST/sysroot.zip" "$URL"
echo "$SHA  $DEST/sysroot.zip" | shasum -a 256 -c -
python3 - "$DEST" <<'PY'
import sys,zipfile,tarfile,shutil
from pathlib import Path
dest=Path(sys.argv[1])
with zipfile.ZipFile(dest/"sysroot.zip") as z:
    if z.namelist()!=["sysroot.tgz"]:
        raise SystemExit(f"Unexpected UTM artifact entries: {z.namelist()[:10]}")
    with z.open("sysroot.tgz") as src, (dest/"sysroot.tgz").open("wb") as out:
        shutil.copyfileobj(src,out)
with tarfile.open(dest/"sysroot.tgz") as t:
    # No links or traversal outside the extracted sysroot.
    t.extractall(dest,filter="data")
PY
PREFIX="$DEST/sysroot-iOS-arm64"
test -d "$PREFIX"
for lib in libdxmt-native.dylib libvirglrenderer.dylib libEGL.dylib libGLESv2.dylib; do
    test -s "$PREFIX/lib/$lib"
    xcrun lipo -verify_arch arm64 "$PREFIX/lib/$lib"
    xcrun vtool -show-build "$PREFIX/lib/$lib" | grep -Eq 'platform IOS$'
done
HDR="$PREFIX/include/virgl"
test -f "$HDR/virglrenderer.h"
# Fail before the app compilation if the required native API is missing.
xcrun nm -gU "$PREFIX/lib/libdxmt-native.dylib" > "$DEST/dxmt-exports"
xcrun nm -gU "$PREFIX/lib/libvirglrenderer.dylib" > "$DEST/virgl-exports"
for sym in D3D11CreateDevice CreateDXGIFactory1 dxmt_native_abi_version; do
    grep -Eq "[[:space:]]_$sym$" "$DEST/dxmt-exports"
done
for sym in virgl_renderer_init virgl_renderer_context_create_with_flags virgl_renderer_create_handle_for_scanout virgl_renderer_release_handle_for_scanout; do
    grep -Eq "[[:space:]]_$sym$" "$DEST/virgl-exports"
done
echo "WINIOS_GPU_PREBUILT_PREFIX=$PREFIX" >> "$GITHUB_ENV"
echo "Verified pinned precompiled iOS graphics libraries."
