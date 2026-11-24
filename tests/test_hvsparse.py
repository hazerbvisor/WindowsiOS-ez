# SPDX-License-Identifier: GPL-2.0-or-later
import os
import shlex
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


class SparseDiskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temporary.cleanup)
        cls.tool = Path(temporary.name) / "hvsparse"
        root = Path(__file__).resolve().parents[1]
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(
            compiler + ["-D_GNU_SOURCE", "-D_DARWIN_C_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Ihost", "tools/hvsparse_main.c", "host/hvsparse.c", "-o", cls.tool],
            cwd=root, check=True,
        )

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.archive = self.work / "disk.hvsparse"
        self.destination = self.work / "disk.img"

    def make_archive(self, size, records):
        self.archive.write_bytes(b"HVSPARSE" + struct.pack("<Q", size) + records)

    def unpack(self):
        return subprocess.run([self.tool, "unpack", self.archive, self.destination], check=False)

    def test_sparse_64_gib_round_trip(self):
        original = self.work / "original.img"
        size = 64 << 30
        offset = size - 4096
        data = b"test disk block"
        with original.open("wb") as file:
            file.truncate(size)
            file.seek(offset)
            file.write(data)
        subprocess.run([self.tool, "pack", original, self.archive], check=True)
        self.unpack().check_returncode()
        disk = self.destination.stat()
        self.assertEqual(disk.st_size, size)
        self.assertLess(disk.st_blocks * 512, 1 << 20)
        with self.destination.open("rb") as file:
            self.assertEqual(file.read(4096), bytes(4096))
            file.seek(offset)
            self.assertEqual(file.read(len(data)), data)
        self.assertTrue(self.archive.is_file())

    def test_existing_destination_is_preserved(self):
        self.make_archive(4096, bytes(16))
        self.destination.write_bytes(b"existing data")
        self.assertNotEqual(self.unpack().returncode, 0)
        self.assertEqual(self.destination.read_bytes(), b"existing data")
        self.assertEqual(list(self.work.glob("*.import.*")), [])

    def test_concurrent_import_publishes_one_disk(self):
        payload = b"first block"
        record = struct.pack("<QQ", 0, len(payload)) + payload
        self.make_archive(4096, record + bytes(16))
        cmd = [self.tool, "unpack", self.archive, self.destination]
        with subprocess.Popen(cmd) as first, subprocess.Popen(cmd) as second:
            results = sorted([first.wait(), second.wait()])
        self.assertEqual(results, [0, 1])
        self.assertEqual(self.destination.read_bytes()[:len(payload)], payload)
        self.assertEqual(list(self.work.glob("*.import.*")), [])

    def test_invalid_records_leave_no_disk(self):
        overflow = struct.pack("<QQ", (1 << 64) - 2, 4) + b"data"
        overlap = struct.pack("<QQ", 16, 4) + b"data" + struct.pack("<QQ", 18, 2) + b"xx"
        truncated = struct.pack("<QQ", 16, 4) + b"x"
        for records in (overflow + bytes(16), overlap + bytes(16), truncated,
                        bytes(16) + b"trailing bytes", struct.pack("<QQ", 123, 0)):
            with self.subTest(records=records):
                self.make_archive(4096, records)
                self.assertNotEqual(self.unpack().returncode, 0)
                self.assertFalse(self.destination.exists())
                self.assertEqual(list(self.work.glob("*.import.*")), [])
                self.assertTrue(self.archive.is_file())


if __name__ == "__main__":
    unittest.main()
