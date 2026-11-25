# SPDX-License-Identifier: GPL-2.0-or-later
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class NVMeStorageTests(unittest.TestCase):
    def test_full_completion_queue_interrupts_and_nvram_persistence(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            if os.name == "posix" and os.uname().sysname != "Darwin":
                (work / "TargetConditionals.h").write_text("#define TARGET_OS_IPHONE 0\n")
            binary = work / "nvme"
            subprocess.run([
                *shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                "-I", str(work), "-Icore", "-Ihost", "-Idevices", "-pthread",
                "tests/test_nvme_storage.c", "devices/nvme.c", "devices/pcie.c",
                "devices/pflash.c", "host/disk.c", "-o", str(binary),
            ], cwd=ROOT, check=True)
            image = work / "disk.img"
            with image.open("wb") as disk:
                disk.truncate(64 << 20)
            nvram = work / "vars.fd"
            nvram.touch()
            subprocess.run([binary, image, nvram], check=True, timeout=15)
