# SPDX-License-Identifier: GPL-2.0-or-later
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TPMS_INCLUDE = os.environ.get("TPMS_INCLUDE")
TPMS_LIBRARY = os.environ.get("TPMS_LIBRARY")


@unittest.skipUnless(TPMS_INCLUDE and TPMS_LIBRARY, "host libtpms is unavailable")
class TPMDeviceTests(unittest.TestCase):
    def test_real_tpm2_commands_and_permanent_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runner = root / "tpm.c"
            runner.write_text(r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tpm_device.h"

static uint32_t command(WinIOSTpm *tpm, uint32_t locality, unsigned char *request, size_t size) {
    uint32_t base = locality * 4096;
    winios_tpm_write(tpm, base, 2, 1);
    assert(winios_tpm_read(tpm, base, 1) & 0x20);
    winios_tpm_write(tpm, base + 0x18, 0x40, 1);
    assert(winios_tpm_read(tpm, base + 0x18, 1) & 0x40);
    for (size_t i = 0; i < size; ++i) winios_tpm_write(tpm, base + 0x24, request[i], 1);
    assert(!(winios_tpm_read(tpm, base + 0x18, 1) & 0x08));
    winios_tpm_write(tpm, base + 0x18, 0x20, 1);
    assert(winios_tpm_read(tpm, base + 0x18, 1) & 0x10);
    uint8_t header[10];
    for (size_t i = 0; i < sizeof(header); ++i) header[i] = winios_tpm_read(tpm, base + 0x24, 1);
    uint32_t length = (uint32_t)header[2] << 24 | (uint32_t)header[3] << 16 | (uint32_t)header[4] << 8 | header[5];
    for (uint32_t i = sizeof(header); i < length; ++i) winios_tpm_read(tpm, base + 0x24, 1);
    assert(!(winios_tpm_read(tpm, base + 0x18, 1) & 0x10));
    winios_tpm_write(tpm, base, 0x20, 1);
    return (uint32_t)header[6] << 24 | (uint32_t)header[7] << 16 | (uint32_t)header[8] << 8 | header[9];
}

int main(int argc, char **argv) {
    unsigned char startup[] = {0x80, 1, 0, 0, 0, 12, 0, 0, 1, 0x44, 0, 0};
    unsigned char random[] = {0x80, 1, 0, 0, 0, 12, 0, 0, 1, 0x7b, 0, 8};
    if (argc != 2 && argc != 3) return 2;
    WinIOSTpm *tpm = winios_tpm_open(argv[1]);
    if (argc == 3) {
        assert(strcmp(argv[2], "corrupt") == 0 && tpm == NULL);
        return 0;
    }
    assert(tpm);
    assert(winios_tpm_open(argv[1]) == NULL);
    assert(winios_tpm_read(tpm, 0x5000, 4) == UINT64_MAX);
    assert(winios_tpm_read(tpm, 0x30, 4) == 0x2100);
    assert(command(tpm, 0, startup, sizeof(startup)) == 0);
    assert(command(tpm, 2, random, sizeof(random)) == 0);
    assert(winios_tpm_reset(tpm) == 0);
    assert(command(tpm, 0, startup, sizeof(startup)) == 0);
    winios_tpm_close(tpm);
    tpm = winios_tpm_open(argv[1]);
    assert(tpm);
    assert(command(tpm, 0, startup, sizeof(startup)) == 0);
    assert(command(tpm, 0, random, sizeof(random)) == 0);
    winios_tpm_close(tpm);
    return 0;
}
""")
            binary = root / "check"
            compiler = shlex.split(os.environ.get("CC", "cc"))
            library_dir = str(Path(TPMS_LIBRARY).parent)
            subprocess.run(compiler + [
                "-D_DEFAULT_SOURCE", "-D_DARWIN_C_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", ROOT / "host", "-I", TPMS_INCLUDE,
                runner, ROOT / "host/tpm_device.c", TPMS_LIBRARY, f"-Wl,-rpath,{library_dir}", "-o", binary,
            ], check=True)
            state = root / "private-state"
            result = subprocess.run([binary, state], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            nv = state / "permall"
            self.assertGreater(nv.stat().st_size, 0)
            self.assertEqual(nv.stat().st_mode & 0o777, 0o600)
            nv.write_bytes(b"invalid TPM state")
            result = subprocess.run([binary, state, "corrupt"], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(nv.read_bytes(), b"invalid TPM state")


class TPMPlatformDescriptionTests(unittest.TestCase):
    def test_acpi_and_dtb_describe_cpu_count_and_optional_tpm(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "TargetConditionals.h").write_text("#define TARGET_OS_IPHONE 1\n")
            runner = root / "tables.c"
            runner.write_text(r"""
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "acpi.h"
#include "fdt.h"

static int with_tpm;
static int saw_acpi;
static unsigned cpu_count;
static unsigned saw_madt;

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int contains(const void *opaque, size_t length, const char *needle) {
    const unsigned char *data = opaque;
    size_t wanted = strlen(needle);
    for (size_t i = 0; i + wanted <= length; ++i)
        if (memcmp(data + i, needle, wanted) == 0) return 1;
    return 0;
}

int hv_fw_cfg_add_file(HvFwCfg *fw, const char *name, const void *opaque, uint32_t length) {
    (void)fw;
    const unsigned char *data = opaque;
    if (strcmp(name, "etc/acpi/tables") == 0) {
        size_t offset = 0;
        assert(contains(data, length, "MSFT0101") == with_tpm);
        while (offset + 36 < length) {
            uint32_t len = le32(data + offset + 4);
            if (len < 36 || len > length - offset) break;
            if (memcmp(data + offset, "TPM2", 4) == 0) {
                assert(len == 52);
                assert(le32(data + offset + 48) == 6);
                saw_acpi++;
            }
            if (memcmp(data + offset, "APIC", 4) == 0) {
                unsigned processors = 0;
                for (size_t entry = offset + 44; entry + 2 <= offset + len;) {
                    unsigned entry_length = data[entry + 1];
                    assert(entry_length >= 2 && entry_length <= offset + len - entry);
                    if (data[entry] == 0x0b) {
                        assert(entry_length == 80);
                        assert(le32(data + entry + 8) == processors);
                        assert(le32(data + entry + 68) == processors);
                        processors++;
                    }
                    entry += entry_length;
                }
                assert(processors == cpu_count);
                saw_madt++;
            }
            offset += len;
        }
    }
    return 0;
}

int main(void) {
    _Static_assert(HV_MAX_CPUS == 8, "iOS vCPU limit");
    for (cpu_count = 4; cpu_count <= HV_MAX_CPUS; cpu_count += 4) {
        for (with_tpm = 0; with_tpm < 2; with_tpm++) {
            uint32_t pa = with_tpm ? 0x0c000000 : 0;
            saw_acpi = 0;
            saw_madt = 0;
            hv_acpi_build_and_register(NULL, cpu_count, 6ULL << 30, 0, 0, pa);
            assert(saw_acpi == with_tpm && saw_madt == 1);
            size_t size = 0;
            uint8_t *tree = hv_fdt_build_virt(cpu_count, 0x40000000, 6ULL << 30, pa, &size);
            assert(tree);
            assert(contains(tree, size, "cpu@3") && !contains(tree, size, "cpu@8"));
            assert(contains(tree, size, "cpu@7") == (cpu_count == 8));
            assert(contains(tree, size, "tcg,tpm-tis-mmio") == with_tpm);
            assert(contains(tree, size, "tpm@c000000") == with_tpm);
            free(tree);
        }
    }
    return 0;
}
""")
            compiler = shlex.split(os.environ.get("CC", "cc"))
            binary = root / "tables"
            subprocess.run(compiler + [
                "-D_DEFAULT_SOURCE", "-std=c11", "-Wall", "-Wextra",
                "-I", root, "-I", ROOT / "core", "-I", ROOT / "host", "-I", ROOT / "devices",
                runner, ROOT / "devices/acpi.c", ROOT / "devices/fdt.c", "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)
