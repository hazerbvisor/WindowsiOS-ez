# SPDX-License-Identifier: GPL-2.0-or-later
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VirtioGPUTests(unittest.TestCase):
    def test_2d_device_transport_and_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            if os.uname().sysname != "Darwin":
                (work / "TargetConditionals.h").write_text("#define TARGET_OS_IPHONE 1\n")
            binary = work / "virtio-gpu"
            subprocess.run([
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                *shlex.split(os.environ.get("VIRTIO_GPU_TEST_FLAGS", "")),
                "-I", str(work), "-Icore", "-Ihost", "-Idevices",
                "tests/test_virtio_gpu.c", "devices/virtio_gpu.c", "devices/virtio.c",
                "devices/pcie.c", "devices/fb_display.c", "-o", str(binary),
            ], cwd=ROOT, check=True)
            subprocess.run([binary], check=True, timeout=20)
