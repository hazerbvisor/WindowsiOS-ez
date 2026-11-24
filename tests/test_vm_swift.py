# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SWIFTC = os.environ.get("SWIFTC") or shutil.which("swiftc")
GENISOIMAGE = os.environ.get("GENISOIMAGE") or shutil.which("genisoimage")
UDF_HEADER = os.environ.get("UDFREAD_HEADER")
UDF_LIBRARY = os.environ.get("UDFREAD_LIBRARY")
GENUINE_ISO = os.environ.get("WINDOWS_24H2_ARM64_ISO")
GENUINE_SHA256 = os.environ.get("WINDOWS_24H2_ARM64_SHA256")


@unittest.skipUnless(SWIFTC, "Swift compiler is unavailable")
class SwiftFoundationTests(unittest.TestCase):
    def test_machine_configuration_media_and_recovery(self):
        sources = [
            "VirtualMachine", "InstallationDraft", "WindowsImageMetadata",
            "WindowsMediaRepository", "WindowsDownload", "PreparedDisk", "NetKVMDrivers", "WindowsUnattend",
        ]
        with tempfile.TemporaryDirectory() as workspace:
            binary = Path(workspace) / "test-vm-foundation"
            subprocess.run([
                SWIFTC, "-warnings-as-errors", "-o", binary, "tests/test_vm_foundation.swift",
                *[f"ios/Sources/{source}.swift" for source in sources],
            ], cwd=ROOT, check=True)
            subprocess.run([binary], check=True)

    def compile_iso_inspector(self, work):
        include = work / "udfread"
        include.mkdir()
        for name in ("udfread.h", "blockinput.h"):
            (include / name).symlink_to(Path(UDF_HEADER).resolve().with_name(name))
        runner = work / "main.swift"
        runner.write_text(
            "import Foundation\n"
            "do {\n"
            "    let args = CommandLine.arguments\n"
            "    let media = try WindowsISO.inspect(URL(fileURLWithPath: args[1]), expectedSHA256: args.count == 3 ? args[2] : nil)\n"
            "    print(media.diagnostics)\n"
            "    print(media.computedSHA256)\n"
            "    print(media.images.map { \"\\($0.name):\\($0.architecture):\\($0.build)\" })\n"
            "} catch {\n"
            "    print(error.localizedDescription)\n"
            "    exit(2)\n"
            "}\n"
        )
        objects = []
        cc = os.environ.get("CC", "cc")
        for source in ("disc_image", "media_hash"):
            obj = work / (source + ".o")
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", work,
                            "-c", ROOT / "host" / (source + ".c"), "-o", obj], check=True)
            objects.append(obj)
        bridge = work / "bridge.h"
        bridge.write_text('#include "disc_image.h"\n#include "media_hash.h"\n')
        binary = work / "inspect-iso"
        subprocess.run([
            SWIFTC, "-warnings-as-errors", "-import-objc-header", bridge, "-I", ROOT / "host",
            "-o", binary, runner,
            *[f"ios/Sources/{source}.swift" for source in ("VirtualMachine", "WindowsImageMetadata", "WindowsISO",
                                                          "WindowsMediaRepository", "WindowsDownload", "PreparedDisk")],
            *objects, UDF_LIBRARY, *([] if sys.platform == "darwin" else ["-lcrypto"]),
        ], cwd=ROOT, check=True)
        return binary

    @unittest.skipUnless(GENISOIMAGE and UDF_HEADER and UDF_LIBRARY, "ISO fixture tools are unavailable")
    def test_iso_media_validation_and_hash_order(self):
        with tempfile.TemporaryDirectory() as workspace:
            work = Path(workspace)
            binary = self.compile_iso_inspector(work)
            for format, options in [("UDF", ["-udf"]), ("Joliet", ["-J"]), ("ISO9660", [])]:
                for machine, build, valid in [(0xAA64, 26100, True), (0x8664, 26100, False), (0xAA64, 28000, False)]:
                    with self.subTest(format=format, machine=machine, build=build):
                        iso = self.make_iso_fixture(work, machine, build, options)
                        checksum = hashlib.sha256(iso.read_bytes()).hexdigest()
                        result = subprocess.run([binary, iso, checksum], capture_output=True)
                        self.assertIn(f"Selected {format}".encode(), result.stdout)
                        if valid:
                            self.assertEqual(result.returncode, 0, result.stdout.decode())
                            self.assertIn(b"Pro:12:26100", result.stdout)
                        else:
                            self.assertEqual(result.returncode, 2)
            hybrid = self.make_iso_fixture(work, 0xAA64, 26100, ["-udf", "-J"])
            with hybrid.open("r+b") as image:
                prefix = image.read(256 * 2048)
                descriptor = prefix.find(b"NSR02")
                self.assertGreaterEqual(descriptor, 16 * 2048)
                image.seek(descriptor)
                image.write(b"NSR00")
            result = subprocess.run([binary, hybrid], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout.decode())
            self.assertIn(b"libudfread:", result.stdout)
            self.assertIn(b"Selected Joliet", result.stdout)

            invalid = work / "bad.iso"
            invalid.write_bytes(b"not an ISO")
            result = subprocess.run([binary, invalid, "0" * 64], capture_output=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn(b"SHA-256 mismatch", result.stdout)
            self.assertNotIn(b"Invalid disc image length", result.stdout)
            result = subprocess.run([binary, invalid], capture_output=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn(b"Invalid disc image length", result.stdout)
            self.assertIn(hashlib.sha256(b"not an ISO").hexdigest().encode(), result.stdout)

    @unittest.skipUnless(GENUINE_ISO and GENUINE_SHA256 and UDF_HEADER and UDF_LIBRARY,
                         "provide a genuine Windows 24H2 ARM64 ISO and its known digest")
    def test_genuine_microsoft_windows_iso(self):
        with tempfile.TemporaryDirectory() as workspace:
            binary = self.compile_iso_inspector(Path(workspace))
            published = os.environ.get("WINDOWS_24H2_MISMATCHED_PUBLISHED_SHA256")
            if published:
                mismatch = subprocess.run([binary, GENUINE_ISO, published], capture_output=True)
                self.assertEqual(mismatch.returncode, 2)
                self.assertIn(b"SHA-256 mismatch", mismatch.stdout)
                self.assertNotIn(b"Selected UDF", mismatch.stdout)
            result = subprocess.run([binary, GENUINE_ISO, GENUINE_SHA256], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout.decode())
            self.assertIn(b":12:26100", result.stdout)

    def make_iso_fixture(self, work, machine, build, options):
        root = work / "files"
        (root / "EFI/Boot").mkdir(parents=True, exist_ok=True)
        (root / "Sources").mkdir(exist_ok=True)
        (root / "Boot").mkdir(exist_ok=True)
        (root / "Boot" / "BCD").write_bytes(b"fixture")
        loader = bytearray(70)
        loader[:2] = b"MZ"
        struct.pack_into("<I", loader, 60, 64)
        loader[64:68] = b"PE\0\0"
        struct.pack_into("<H", loader, 68, machine)
        (root / "EFI/Boot/BOOTAA64.EFI").write_bytes(loader)

        xml = f"""<WIM><IMAGE INDEX="1"><NAME>Pro</NAME><WINDOWS><ARCH>12</ARCH><VERSION><BUILD>{build}</BUILD></VERSION></WINDOWS></IMAGE></WIM>""".encode("utf-16")
        header = bytearray(208)
        header[:8] = b"MSWIM\0\0\0"
        struct.pack_into("<I", header, 8, len(header))
        struct.pack_into("<HHI", header, 40, 1, 1, 1)
        struct.pack_into("<QQQ", header, 72, len(xml), len(header), len(xml))
        for name in ("boot.wim", "install.wim"):
            (root / "Sources" / name).write_bytes(header + xml)

        iso = work / f"{machine}-{build}.iso"
        subprocess.run([GENISOIMAGE, "-quiet", *options, "-o", iso, root], check=True)
        return iso


if __name__ == "__main__":
    unittest.main()
