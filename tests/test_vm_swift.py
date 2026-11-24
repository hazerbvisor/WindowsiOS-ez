# SPDX-License-Identifier: GPL-2.0-or-later
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SWIFTC = os.environ.get("SWIFTC") or shutil.which("swiftc")
GENISOIMAGE = os.environ.get("GENISOIMAGE") or shutil.which("genisoimage")
UDF_HEADER = os.environ.get("UDFREAD_HEADER")
UDF_LIBRARY = os.environ.get("UDFREAD_LIBRARY")


@unittest.skipUnless(SWIFTC, "Swift compiler is unavailable")
class SwiftFoundationTests(unittest.TestCase):
    def test_machine_configuration_media_and_recovery(self):
        sources = [
            "VirtualMachine", "InstallationDraft", "WindowsImageMetadata",
            "WindowsMediaRepository", "WindowsDownload", "PreparedDisk", "NetKVMDrivers",
        ]
        with tempfile.TemporaryDirectory() as workspace:
            binary = Path(workspace) / "test-vm-foundation"
            subprocess.run([
                SWIFTC, "-warnings-as-errors", "-o", binary, "tests/test_vm_foundation.swift",
                *[f"ios/Sources/{source}.swift" for source in sources],
            ], cwd=ROOT, check=True)
            subprocess.run([binary], check=True)

    @unittest.skipUnless(GENISOIMAGE and UDF_HEADER and UDF_LIBRARY, "real UDF fixture tools are unavailable")
    def test_udf_iso_media_validation(self):
        with tempfile.TemporaryDirectory() as workspace:
            work = Path(workspace)
            runner = work / "main.swift"
            runner.write_text(
                "import Foundation\n"
                "do {\n"
                "    let images = try WindowsISO.inspect(URL(fileURLWithPath: CommandLine.arguments[1]))\n"
                "    print(images.map { $0.build })\n"
                "} catch {\n"
                "    print(error.localizedDescription)\n"
                "    exit(2)\n"
                "}\n"
            )
            binary = work / "inspect-iso"
            subprocess.run([
                SWIFTC, "-warnings-as-errors", "-import-objc-header", UDF_HEADER,
                "-o", binary, runner, "ios/Sources/VirtualMachine.swift",
                "ios/Sources/WindowsImageMetadata.swift", "ios/Sources/WindowsISO.swift",
                "ios/Sources/PreparedDisk.swift", UDF_LIBRARY,
            ], cwd=ROOT, check=True)
            for machine, build, valid in [(0xAA64, 26100, True), (0x8664, 26100, False), (0xAA64, 28000, False)]:
                with self.subTest(machine=machine, build=build):
                    iso = self.make_udf_fixture(work, machine, build)
                    result = subprocess.run([binary, iso], capture_output=True)
                    if valid:
                        self.assertEqual(result.returncode, 0, result.stderr.decode())
                        self.assertIn(b"26100", result.stdout)
                    else:
                        self.assertNotEqual(result.returncode, 0)

    def make_udf_fixture(self, work, machine, build):
        root = work / "files"
        (root / "EFI/Boot").mkdir(parents=True, exist_ok=True)
        (root / "Sources").mkdir(exist_ok=True)
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
        subprocess.run([GENISOIMAGE, "-quiet", "-udf", "-o", iso, root], check=True)
        return iso


if __name__ == "__main__":
    unittest.main()
