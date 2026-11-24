#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Stage the upstream FatFs sources and configure two 512-byte volumes.
set -euo pipefail

stage="${1:?Usage: prepare_fatfs.sh OUTPUT_DIRECTORY}"
mkdir -p "$stage"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
archive="$work/ff16.zip"
curl -fLSs --retry 3 https://elm-chan.org/fsw/ff/arc/ff16.zip -o "$archive"
printf '%s  %s\n' 99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e "$archive" |
    shasum -a 256 -c -
unzip -q "$archive" 'source/*' -d "$work"
cp "$work"/source/{ff.c,ff.h,ffunicode.c,diskio.h} "$stage/"
sed -E \
    -e 's/^(#define FF_USE_MKFS)[[:space:]]+[0-9]+/\1 1/' \
    -e 's/^(#define FF_CODE_PAGE)[[:space:]]+[0-9]+/\1 437/' \
    -e 's/^(#define FF_USE_LFN)[[:space:]]+[0-9]+/\1 2/' \
    -e 's/^(#define FF_LFN_UNICODE)[[:space:]]+[0-9]+/\1 2/' \
    -e 's/^(#define FF_VOLUMES)[[:space:]]+[0-9]+/\1 2/' \
    -e 's/^(#define FF_FS_EXFAT)[[:space:]]+[0-9]+/\1 1/' \
    -e 's/^(#define FF_LBA64)[[:space:]]+[0-9]+/\1 1/' \
    -e 's/^(#define FF_FS_NORTC)[[:space:]]+[0-9]+/\1 1/' \
    "$work/source/ffconf.h" > "$stage/ffconf.h"
