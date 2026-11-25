# SPDX-License-Identifier: GPL-2.0-or-later
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import uuid
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FATFS = os.environ.get("FATFS_SOURCE_DIR")
UDF_HEADER = os.environ.get("UDFREAD_HEADER")
UDF_LIBRARY = os.environ.get("UDFREAD_LIBRARY")
GENISOIMAGE = os.environ.get("GENISOIMAGE") or shutil.which("genisoimage")
MTYPE = os.environ.get("MTYPE") or shutil.which("mtype")
FSCK_EXFAT = os.environ.get("FSCK_EXFAT") or shutil.which("fsck.exfat")
ESP = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b").bytes_le
BASIC_DATA = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7").bytes_le


@unittest.skipUnless(all((FATFS, UDF_HEADER, UDF_LIBRARY, GENISOIMAGE, MTYPE, FSCK_EXFAT)),
                     "host FAT/UDF fixture tools are unavailable")
class InstallerDiskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(directory.cleanup)
        work = Path(directory.name)
        (work / "fatfs").symlink_to(Path(FATFS).resolve(), target_is_directory=True)
        (work / "udfread").mkdir()
        for name in ("udfread.h", "blockinput.h"):
            (work / "udfread" / name).symlink_to(Path(UDF_HEADER).resolve().with_name(name))
        runner = work / "main.c"
        runner.write_text(r"""
#include <stdio.h>
#include <string.h>
#include "installer_disk.h"

static int progress(uint64_t copied, uint64_t total, void *context) {
    printf("%llu/%llu\n", (unsigned long long)copied, (unsigned long long)total);
    return context && copied > 0;
}

int main(int argc, char **argv) {
    char error[256] = {0};
    if (argc == 3 && !strcmp(argv[2], "logs"))
        return winios_prepare_setup_logs(argv[1]) != 0;
    if (argc == 4 && !strcmp(argv[3], "export-logs"))
        return winios_export_setup_logs(argv[1], argv[2]) != 1;
    if (argc == 4 && !strcmp(argv[3], "check"))
        return !winios_has_windows_boot_files(argv[1], 2048, 524288);
    if (argc == 4 && !strcmp(argv[3], "configure"))
        return winios_configure_installer_answer(argv[1], argv[2]) != 0;
    if (argc == 4 && !strcmp(argv[3], "disarm"))
        return winios_configure_installer_answer(argv[1], NULL) != 0;
    if (argc != 3 && argc != 4) return 2;
    int result = winios_build_installer_disk(argv[1], argv[2], NULL, NULL, NULL, progress, argc == 4 ? argv[3] : NULL,
                                             error, sizeof(error));
    if (result) fprintf(stderr, "%s\n", error);
    return result != 0;
}
""")
        cls.binary = work / "installer"
        subprocess.run([
            os.environ.get("CC", "cc"), "-D_GNU_SOURCE", "-D_DEFAULT_SOURCE", "-D_DARWIN_C_SOURCE", "-std=c11", "-O2",
            "-I", work, "-I", ROOT / "host", runner, ROOT / "host" / "installer_disk.c", ROOT / "host" / "disc_image.c",
            Path(FATFS) / "ff.c", Path(FATFS) / "ffunicode.c", UDF_LIBRARY, "-pthread", "-o", cls.binary,
        ], check=True)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.iso = self.work / "windows.iso"
        self.output = self.work / "installer.img"
        self.source = self.work / "files"
        for name in ("EFI/Boot", "Boot", "Sources"):
            (self.source / name).mkdir(parents=True)
        setup_exe = bytearray(70)
        setup_exe[:2] = b"MZ"
        struct.pack_into("<I", setup_exe, 60, 64)
        setup_exe[64:] = b"PE\0\0\x64\xaa"
        self.files = {
            "EFI/Boot/BOOTAA64.EFI": b"synthetic ARM64 EFI",
            "Boot/BCD": b"synthetic BCD",
            "Sources/Boot.WIM": b"synthetic WinPE",
            "Sources/Setup.exe": setup_exe,
            "Sources/Setup.cfg": b"Setup source tree test fixture",
            "Sources/Install.WIM": b"synthetic install payload" * 1024,
            "Root file.txt": b"FAT32 long filename",
        }
        for name, data in self.files.items():
            (self.source / name).write_bytes(data)

    def make_iso(self, *options):
        subprocess.run([GENISOIMAGE, "-quiet", *options, "-o", self.iso, self.source], check=True)

    def build(self, *options):
        return subprocess.run([self.binary, self.iso, self.output, *options], capture_output=True)

    def test_layout_and_files_are_readable(self):
        self.make_iso("-udf")
        process = self.build()
        self.assertEqual(process.returncode, 0, process.stderr.decode())
        copied = sum(map(len, self.files.values()))
        self.assertEqual(process.stdout.splitlines()[0], f"0/{copied}".encode())
        self.assertEqual(process.stdout.splitlines()[-1], f"{copied}/{copied}".encode())
        with self.output.open("rb") as disk:
            first = disk.read(512)
            self.assertEqual(first[450], 0xEE)
            self.assertEqual(first[510:512], b"\x55\xaa")
            last = self.output.stat().st_size // 512 - 1
            tables = []
            for lba, alternate in [(1, last), (last, 1)]:
                disk.seek(lba * 512)
                header = bytearray(disk.read(512))
                self.assertEqual(header[:8], b"EFI PART")
                checksum = struct.unpack_from("<I", header, 16)[0]
                struct.pack_into("<I", header, 16, 0)
                self.assertEqual(zlib.crc32(header[:92]), checksum)
                self.assertEqual(struct.unpack_from("<QQ", header, 24), (lba, alternate))
                disk.seek(struct.unpack_from("<Q", header, 72)[0] * 512)
                table = disk.read(32 * 512)
                self.assertEqual(zlib.crc32(table), struct.unpack_from("<I", header, 88)[0])
                tables.append(table)
            self.assertEqual(*tables)
            table = tables[0]
            self.assertEqual(table[:16], ESP)
            self.assertEqual(table[128:144], BASIC_DATA)
            boot_lba, boot_end = struct.unpack_from("<QQ", table, 32)
            data_lba, data_end = struct.unpack_from("<QQ", table, 160)
            self.assertLess(boot_end, data_lba)
            fat_uri = f"{self.output}@@{boot_lba * 512}"
            for name in ("EFI/Boot/BOOTAA64.EFI", "Boot/BCD", "Sources/Boot.WIM", "Root file.txt"):
                content = subprocess.check_output([MTYPE, "-i", fat_uri, f"::/{name}"])
                self.assertEqual(content, self.files[name])
            result = subprocess.run([MTYPE, "-i", fat_uri, "::/Sources/Install.WIM"], capture_output=True)
            self.assertNotEqual(result.returncode, 0)

            disk.seek(data_lba * 512)
            data = disk.read((data_end - data_lba + 1) * 512)
            self.assertEqual(data[3:11], b"EXFAT   ")
            self.assertIn(self.files["Sources/Install.WIM"], data)
            exfat = self.work / "payload.img"
            exfat.write_bytes(data)
            result = subprocess.run([FSCK_EXFAT, "-n", exfat], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_iso9660_multi_extent_payload_is_copied_in_order(self):
        self.files["Sources/Install.WIM"] = b"A" * 2048 + b"B" * 619
        (self.source / "Sources/Install.WIM").write_bytes(self.files["Sources/Install.WIM"])
        self.make_iso()
        image = bytearray(self.iso.read_bytes())
        sector = 2048

        def entry(directory, name):
            cursor = directory * sector
            while image[cursor]:
                length, name_length = image[cursor], image[cursor + 32]
                if image[cursor + 33:cursor + 33 + name_length] == name:
                    return cursor
                cursor += length
            self.fail(f"missing test fixture entry {name!r}")

        def extent(record):
            return struct.unpack_from("<I", image, record + 2)[0]

        root = extent(16 * sector + 156)
        sources = extent(entry(root, b"SOURCES"))
        record = entry(sources, b"INSTALL.WIM;1")
        length = image[record]
        tail = record + length
        end = (sources + 1) * sector
        self.assertEqual(image[end - length:end], bytes(length))
        second = bytearray(image[record:tail])
        struct.pack_into("<I", second, 2, extent(record) + 1)
        struct.pack_into(">I", second, 6, extent(record) + 1)
        struct.pack_into("<I", second, 10, 619)
        struct.pack_into(">I", second, 14, 619)
        image[tail + length:end] = image[tail:end - length]
        image[tail:tail + length] = second
        image[record + 25] |= 0x80
        struct.pack_into("<I", image, record + 10, sector)
        struct.pack_into(">I", image, record + 14, sector)
        self.iso.write_bytes(image)

        result = self.build()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        with self.output.open("rb") as disk:
            disk.seek(512 * 2 + 160)
            start, end = struct.unpack("<QQ", disk.read(16))
            disk.seek(start * 512)
            partition = disk.read((end - start + 1) * 512)
        self.assertIn(self.files["Sources/Install.WIM"], partition)

        self.output.unlink()
        image[tail + 33] = ord("X")
        self.iso.write_bytes(image)
        result = self.build()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"malformed directory", result.stderr)
        self.assertFalse(self.output.exists())

    def test_cancel_and_invalid_media_do_not_publish(self):
        self.make_iso("-udf")
        original = self.iso.stat().st_size
        result = self.build("cancel")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"cancelled", result.stderr)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.work.glob("*.building.*")), [])
        self.assertEqual(self.iso.stat().st_size, original)

        alias = self.work / "alias.iso"
        alias.symlink_to(self.iso)
        result = subprocess.run([self.binary, alias, self.output], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"Unable to open the ISO file", result.stderr)
        self.assertFalse(self.output.exists())

        (self.source / "Boot/BCD").unlink()
        self.make_iso("-udf")
        self.assertNotEqual(self.build().returncode, 0)
        self.assertFalse(self.output.exists())

    def test_setup_logs_persist_and_export_without_writing_the_image(self):
        image = self.work / "SetupLogs.img"
        subprocess.run([self.binary, image, "logs"], check=True)
        self.assertEqual(image.stat().st_size, 128 << 20)
        root = f"{image}@@1048576"
        subprocess.run([MTYPE, "-i", root, "::/WINIOS.LOG"], check=True)
        mmd = Path(MTYPE).with_name("mmd")
        mcopy = Path(MTYPE).with_name("mcopy")
        if not (mmd.is_file() and mcopy.is_file()):
            self.skipTest("mtools write utilities are unavailable")
        subprocess.run([mmd, "-i", root, "::/Logs"], check=True)
        log = self.work / "launcher.log"
        log.write_text("setup.exe exit code: -2147024809\n")
        subprocess.run([mcopy, "-i", root, log, "::/Logs/launcher.log"], check=True)
        mtime = image.stat().st_mtime_ns
        subprocess.run([self.binary, image, "logs"], check=True)
        folder = self.work / "export"
        folder.mkdir()
        subprocess.run([self.binary, image, folder, "export-logs"], check=True)
        self.assertEqual((folder / "launcher.log").read_text(), log.read_text())
        self.assertEqual(image.stat().st_mtime_ns, mtime)

    def test_existing_installer_remains_untouched(self):
        self.make_iso("-udf")
        self.output.write_bytes(b"existing")
        self.assertNotEqual(self.build().returncode, 0)
        self.assertEqual(self.output.read_bytes(), b"existing")
        self.assertEqual(list(self.work.glob("*.building.*")), [])

    def test_read_only_windows_boot_marker_check(self):
        boot = self.source / "EFI/Microsoft/Boot"
        boot.mkdir(parents=True)
        loader = bytearray(70)
        loader[:2] = b"MZ"
        struct.pack_into("<I", loader, 60, 64)
        loader[64:] = b"PE\0\0\x64\xaa"
        (boot / "bootmgfw.efi").write_bytes(loader)
        (boot / "BCD").write_bytes(b"test boot configuration")
        self.make_iso("-udf")
        self.assertEqual(self.build().returncode, 0)
        before = self.output.stat().st_mtime_ns
        subprocess.run([self.binary, self.output, self.iso, "check"], check=True)
        self.assertEqual(self.output.stat().st_mtime_ns, before)

    @unittest.skipUnless(all(os.environ.get(key) for key in ("QEMU_AARCH64", "EDK2_FIRMWARE", "EFI_CLANG", "EFI_LLD")),
                         "ARM64 QEMU firmware or EFI toolchain is unavailable")
    def test_armvirt_boots_generated_nvme_media(self):
        source = self.work / "efi.c"
        source.write_text(r"""
typedef unsigned short CHAR16;
typedef unsigned long long EFI_STATUS;
typedef struct Output {
    void *reset;
    EFI_STATUS (*print)(struct Output *, const CHAR16 *);
} Output;
typedef void (*ResetSystem)(unsigned, EFI_STATUS, unsigned long long, void *);

EFI_STATUS efi_main(void *image, void *system_table) {
    (void)image;
    Output *out = *(Output **)((char *)system_table + 64);
    out->print(out, L"WindowsiOS FAT32 NVMe fixture reached EFI entry\r\n");
    char *runtime = *(char **)((char *)system_table + 88);
    ResetSystem shutdown = *(ResetSystem *)(runtime + 104);
    shutdown(2, 0, 0, 0);
    return 0;
}
""")
        object_file = self.work / "efi.obj"
        efi = self.source / "EFI/Boot/BOOTAA64.EFI"
        subprocess.run([
            os.environ["EFI_CLANG"], "--target=aarch64-pc-windows-msvc", "-ffreestanding",
            "-fshort-wchar", "-fno-stack-protector", "-O2", "-c", source, "-o", object_file,
        ], check=True)
        subprocess.run([
            os.environ["EFI_LLD"], "-flavor", "link", "/machine:arm64", "/subsystem:efi_application",
            "/entry:efi_main", "/nodefaultlib", f"/out:{efi}", object_file,
        ], check=True)
        self.make_iso("-udf")
        self.assertEqual(self.build().returncode, 0)
        rom_path = os.environ.get("QEMU_DATA_DIR")
        qemu = [os.environ["QEMU_AARCH64"]]
        if rom_path:
            qemu.extend(["-L", rom_path])
        if assets := os.environ.get("WINDOWSIOS_INSTALLER_ASSETS"):
            firmware = Path(assets)
            variables = self.work / "vars.fd"
            shutil.copyfile(firmware / "edk2-arm-vars.fd", variables)
            flash = ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={firmware / 'edk2-aarch64-code.fd'}",
                     "-drive", f"if=pflash,format=raw,unit=1,file={variables}"]
        else:
            flash = ["-bios", os.environ["EDK2_FIRMWARE"]]
        result = subprocess.run(qemu + [
            "-machine", "virt,accel=tcg", "-cpu", "cortex-a57", "-m", "512",
            *flash,
            "-drive", f"if=none,format=raw,id=setup,file={self.output},readonly=on",
            "-device", "nvme,drive=setup,serial=WIN11INST", "-nic", "none",
            "-display", "none", "-serial", "stdio", "-monitor", "none",
        ], capture_output=True, timeout=30, check=True)
        self.assertIn(b"WindowsiOS FAT32 NVMe fixture reached EFI entry", result.stdout)

    def exfat_root_names(self):
        with self.output.open("rb") as disk:
            disk.seek(512 * 2 + 160)
            start = struct.unpack("<Q", disk.read(8))[0]
            disk.seek(start * 512)
            boot = disk.read(512)
            heap = struct.unpack_from("<I", boot, 88)[0]
            root = struct.unpack_from("<I", boot, 96)[0]
            sectors_per_cluster = 1 << boot[109]
            disk.seek((start + heap + (root - 2) * sectors_per_cluster) * 512)
            directory = disk.read(sectors_per_cluster * 512)
        names = []
        for offset in range(0, len(directory), 32):
            if directory[offset] == 0xc1:
                names.append(directory[offset + 2:offset + 32].decode("utf-16-le").rstrip("\x00"))
        return names

    def compile_winpe_preparer(self):
        runner = self.work / "prepare.c"
        runner.write_text(r"""
#include <stdio.h>
#include <string.h>
#include "installer_winpe.h"

static int cancel_plan(uint64_t copied, uint64_t total, void *context) {
    (void)copied;
    (void)total;
    (void)context;
    return 1;
}

int main(int argc, char **argv) {
    char error[256] = {0};
    if (argc == 5) {
        int result = winios_build_installer_disk(argv[1], argv[4], argv[3], NULL, NULL,
                                                 cancel_plan, NULL, error, sizeof(error));
        if (result) fprintf(stderr, "%s\n", error);
        return !(result && strstr(error, "cancelled"));
    }
    int result = winios_prepare_winpe_wim(argv[1], argv[2], NULL, NULL, error, sizeof(error));
    if (!result && argc == 7)
        result = winios_build_installer_disk(argv[1], argv[4], argv[3], argv[5], argv[6], NULL, NULL, error, sizeof(error));
    if (result) fprintf(stderr, "%s\n", error);
    return result != 0;
}
""")
        include = self.binary.parent
        binary = self.work / "prepare"
        subprocess.run([
            os.environ.get("CC", "cc"), "-D_DEFAULT_SOURCE", "-D_DARWIN_C_SOURCE", "-D_GNU_SOURCE",
            "-std=c11", "-O2", "-I", include,
            "-I", Path(os.environ["WIMLIB_HEADER"]).parent, "-I", ROOT / "host", runner,
            ROOT / "host" / "installer_winpe.c", ROOT / "host" / "installer_disk.c", ROOT / "host" / "disc_image.c",
            Path(FATFS) / "ff.c", Path(FATFS) / "ffunicode.c", UDF_LIBRARY, os.environ["WIMLIB_LIBRARY"],
            "-pthread", "-o", binary,
        ], check=True)
        return binary

    @unittest.skipUnless(all(os.environ.get(name) for name in
                             ("WINDOWS_24H2_ARM64_ISO", "WIMLIB_HEADER", "WIMLIB_LIBRARY", "WIMLIB_IMAGEX")),
                         "a genuine ARM64 24H2 ISO and host wimlib are required")
    def test_genuine_windows_iso_winpe_preparation(self):
        binary = self.compile_winpe_preparer()
        staging = self.work / "winpe"
        staging.mkdir()
        result = subprocess.run([binary, os.environ["WINDOWS_24H2_ARM64_ISO"], staging], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        patched = staging / "boot.wim"
        imagex = os.environ["WIMLIB_IMAGEX"]
        info = subprocess.check_output([imagex, "info", patched], text=True)
        self.assertRegex(info, r"Architecture:\s+ARM64")
        self.assertRegex(info, r"Build:\s+26100")
        boot = re.search(r"Boot Index:\s+(\d+)", info)
        self.assertIsNotNone(boot, info)
        subprocess.run([imagex, "extract", patched, boot[1], "/Windows/System32/winios-setup.cmd",
                        f"--dest-dir={staging}"], stdout=subprocess.DEVNULL, check=True)
        script = (staging / "winios-setup.cmd").read_text()
        self.assertIn("setup.exe exit code:", script)
        self.assertIn("/CopyLogs", script)
        self.assertIn("Windows\\Panther", script)
        plan = subprocess.run([binary, os.environ["WINDOWS_24H2_ARM64_ISO"], staging, patched, self.output],
                              capture_output=True)
        self.assertEqual(plan.returncode, 0, plan.stderr.decode())
        self.assertFalse(self.output.exists())

    @unittest.skipUnless(os.environ.get("WIMLIB_HEADER") and os.environ.get("WIMLIB_LIBRARY")
                         and os.environ.get("WIMLIB_IMAGEX"), "host wimlib is unavailable")
    def test_winpe_patch_and_final_disk_with_real_wim_container(self):
        wim_source = self.work / "winpe"
        system = wim_source / "Windows/System32"
        system.mkdir(parents=True)
        (system / "winpeshl.ini").write_text("[LaunchApps]\r\nold.exe\r\n")
        imagex = os.environ["WIMLIB_IMAGEX"]
        wim_path = self.source / "Sources/Boot.WIM"
        wim_path.unlink()
        subprocess.run([imagex, "capture", wim_source, wim_path, "Windows Setup", "--boot"],
                       stdout=subprocess.DEVNULL, check=True)
        before = wim_path.read_bytes()
        self.make_iso("-J")

        binary = self.compile_winpe_preparer()
        staging = self.work / "staging"
        staging.mkdir()
        patched = staging / "boot.wim"
        drivers = self.work / "drivers"
        drivers.mkdir()
        for name in ("netkvm.inf", "netkvm.cat", "netkvm.sys", "netkvmco.dll", "netkvmp.exe"):
            (drivers / name).write_bytes(("test content for " + name).encode())
        answer = self.work / "answer.xml"
        answer.write_bytes(b"<unattend>test</unattend>")
        subprocess.run([binary, self.iso, staging, patched, self.output, drivers, answer], check=True)
        with self.output.open("rb") as disk:
            disk.seek(512 * 2 + 160)
            start, end = struct.unpack("<QQ", disk.read(16))
            disk.seek(start * 512)
            partition = disk.read((end - start + 1) * 512)
        self.assertIn(b"test content for netkvm.sys", partition)
        self.assertIn(b"test content for netkvmp.exe", partition)
        self.assertIn(b"<unattend>test</unattend>", partition)
        self.assertIn(bytes(self.files["Sources/Setup.exe"]), partition)
        self.assertIn(self.files["Sources/Setup.cfg"], partition)
        self.assertIn("WINIOS.V2", self.exfat_root_names())
        target = self.work / "target.img"
        with target.open("wb") as disk:
            disk.truncate(64 << 30)
        actions = [self.binary, self.output, target]
        subprocess.run([*actions, "configure"], check=True)
        self.assertIn("WINIOS.AUTO", self.exfat_root_names())
        subprocess.run([*actions, "disarm"], check=True)
        self.assertNotIn("WINIOS.AUTO", self.exfat_root_names())
        subprocess.run([*actions, "configure"], check=True)
        self.assertIn("WINIOS.AUTO", self.exfat_root_names())
        with target.open("r+b") as disk:
            disk.seek(512)
            disk.write(b"Existing partition data")
        subprocess.run([*actions, "configure"], check=True)
        self.assertNotIn("WINIOS.AUTO", self.exfat_root_names())
        self.assertEqual(wim_path.read_bytes(), before)
        subtree = self.work / "reread"
        subprocess.run([
            imagex, "extract", patched, "1", "/Windows/System32/winpeshl.ini",
            "/Windows/System32/winios-setup.cmd", f"--dest-dir={subtree}"
        ], stdout=subprocess.DEVNULL, check=True)
        self.assertIn(b"cmd.exe", next(subtree.rglob("winpeshl.ini")).read_bytes())
        script = next(subtree.rglob("winios-setup.cmd")).read_bytes()
        self.assertIn(b'pushd "%MEDIA%\\sources"', script)
        self.assertIn(b'set "SETUP_RC=%ERRORLEVEL%"', script)
        self.assertIn(b"goto manual", script)
        self.assertIn(b"/CopyLogs", script)
        self.assertIn(b"/InstallDrivers", script)
        self.assertIn(b"WINIOS.MEDIA", script)
        self.assertNotIn(b"Bypass", script)
        self.assertNotIn(b"diskpart /s", script)
        self.assertIn(b'cmd.exe /d /c call "%~f0" /collect-disk-logs', script)
        self.assertEqual(subprocess.check_output([
            MTYPE, "-i", f"{self.output}@@1048576", "::/Sources/Boot.WIM"
        ]), patched.read_bytes())
