#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the generated device settings or an unsigned WindowsiOS-ez IPA."""

import argparse
import hashlib
import json
import plistlib
import struct
import sys
import zipfile
from pathlib import Path, PurePosixPath


BUNDLE_ID = "io.github.hazerbvisor.windowsios-ez"
APP = "Payload/WindowsiOS.app/"
ASSET_MANIFEST = Path(__file__).with_name("installer_assets.json")
ENTITLEMENTS = {
    "get-task-allow": True,
    "com.apple.developer.kernel.increased-memory-limit": True,
    "com.apple.developer.kernel.extended-virtual-addressing": True,
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify_entitlements(path):
    manifest = plistlib.loads(path.read_bytes())
    require(
        isinstance(manifest, dict) and manifest.keys() == ENTITLEMENTS.keys()
        and all(manifest[key] is True for key in ENTITLEMENTS),
        "Signing manifest does not contain exactly the expected boolean entitlements",
    )


def verify_settings(path, entitlements):
    targets = json.loads(path.read_text())
    settings = next(
        (item["buildSettings"] for item in targets if item.get("target") == "WindowsiOS"),
        None,
    )
    require(settings is not None, "WindowsiOS target missing from generated build settings")
    expected = {
        "ARCHS": "arm64",
        "CONFIGURATION": "Release",
        "PLATFORM_NAME": "iphoneos",
        "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE_ID,
        "IPHONEOS_DEPLOYMENT_TARGET": "17.0",
        "CODE_SIGNING_ALLOWED": "NO",
        "CODE_SIGNING_REQUIRED": "NO",
    }
    for key, value in expected.items():
        require(settings.get(key) == value, f"{key} must be {value}; got {settings.get(key)!r}")
    configured = Path(settings.get("CODE_SIGN_ENTITLEMENTS") or ".")
    source_dir = Path(settings.get("SRCROOT") or ".")
    require((source_dir / configured).resolve() == entitlements.resolve(),
            "Generated target does not reference the expected signing manifest")


def verify_macho(executable):
    header = executable.read(32)
    require(len(header) == 32, "Missing Mach-O header")
    magic, cpu, _, filetype, count, size, _, _ = struct.unpack("<8I", header)
    require((magic, cpu, filetype) == (0xFEEDFACF, 0x0100000C, 2),
            "Executable is not a thin ARM64 Mach-O application")
    require(0 < count <= size // 8 and size <= 1024 * 1024, "Invalid Mach-O load command table")
    commands = executable.read(size)
    require(len(commands) == size, "Truncated Mach-O load commands")
    offset = 0
    device = False
    for _ in range(count):
        require(offset + 8 <= size, "Truncated Mach-O load command")
        command, length = struct.unpack_from("<II", commands, offset)
        require(length >= 8 and length % 8 == 0 and offset + length <= size,
                "Invalid Mach-O load command length")
        require(command != 0x1D, "Mach-O contains a code signature")
        if command == 0x32:
            require(length >= 24, "Invalid Mach-O build version command")
            platform, minos = struct.unpack_from("<II", commands, offset + 8)
            device = platform == 2 and minos == 17 << 16
        offset += length
    require(offset == size and device, "Mach-O executable must target physical iOS 17.0")


def verify_ipa(path):
    assets = json.loads(ASSET_MANIFEST.read_text())
    with zipfile.ZipFile(path) as ipa:
        entries = ipa.namelist()
        require(len(entries) == len(set(entries)), "IPA has duplicate entries")
        for entry in entries:
            require(entry == "Payload/" or entry.startswith(APP), f"Unexpected IPA entry: {entry}")
            parts = PurePosixPath(entry).parts
            require(".." not in parts and "_CodeSignature" not in parts
                    and "embedded.mobileprovision" not in parts,
                    f"Unexpected signing or path data: {entry}")
        info = plistlib.loads(ipa.read(APP + "Info.plist"))
        expected = {
            "CFBundleIdentifier": BUNDLE_ID,
            "CFBundlePackageType": "APPL",
            "CFBundleExecutable": "WindowsiOS",
            "CFBundleSupportedPlatforms": ["iPhoneOS"],
            "MinimumOSVersion": "17.0",
        }
        for key, value in expected.items():
            require(info.get(key) == value, f"Unexpected bundle {key}: {info.get(key)!r}")
        require(ipa.getinfo(APP + "henge-jit.js").file_size > 0, "JIT helper script is empty")
        with ipa.open(APP + info["CFBundleExecutable"]) as executable:
            verify_macho(executable)
        asset_prefix = APP + "InstallerAssets/"
        bundled_assets = {entry[len(asset_prefix):] for entry in entries
                          if entry.startswith(asset_prefix) and not entry.endswith("/")}
        require(bundled_assets == set(assets), "Unexpected or missing installer assets")
        for name, expected in assets.items():
            with ipa.open(asset_prefix + name) as content:
                digest = hashlib.sha256()
                for chunk in iter(lambda: content.read(1 << 20), b""):
                    digest.update(chunk)
            require(digest.hexdigest() == expected, f"Invalid bundled installer asset: {name}")
        require(ipa.testzip() is None, "IPA contains a corrupted ZIP entry")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("ipa", "settings"))
    parser.add_argument("input", type=Path)
    parser.add_argument("entitlements", type=Path)
    args = parser.parse_args()
    try:
        verify_entitlements(args.entitlements)
        if args.mode == "settings":
            verify_settings(args.input, args.entitlements)
        else:
            verify_ipa(args.input)
    except (OSError, ValueError, KeyError, plistlib.InvalidFileException, zipfile.BadZipFile) as error:
        sys.exit(f"Verification failed: {error}")
    print(f"Verified {args.mode}: {args.input}")


if __name__ == "__main__":
    main()
