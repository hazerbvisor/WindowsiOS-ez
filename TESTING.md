# Verification

## Local tests

Run these from the repository root.

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
for script in scripts/*.sh; do bash -n "$script" || exit; done
python3 -m py_compile scripts/*.py
git diff --check
```

The Python suite compiles the C sparse CLI with strict warnings, exercises a 64 GiB image round trip and sparse copy, tests malformed records, races two concurrent imports and verifies an existing disk is preserved. It also checks unsigned IPA and generated-setting validation against synthetic fixtures. On Apple Silicon macOS run `make test-core` and `./scripts/build_unsigned_ipa.sh` as separate checks.

## Physical-device baseline

Use a physical iPad Air M4 on iPadOS 27.0.1 and record the actual model, detected physical RAM, OS build, Git commit, Xcode version and sideloader. Use Windows 11 24H2 ARM64 build 26100 for each pass.

1. Verify the Codemagic checksums, sign the IPA, confirm the signed entitlements and install it with the device's sideloading workflow. Enable Developer Mode where required. Check that the signed app launches.
2. In Files, stage your lawfully prepared Windows disk and its matching EDK2 `code.fd` and `vars.fd` under the app's `Machines/Win11` folder. Use `windows11.hvsparse` or `windows11.img`. Keep a backup of the disk and the writable variable store. For a sparse import test, use a fresh machine directory with no raw disk.
3. Use the app's StikDebug action with an authorized debugger setup. Check that the runtime reports that its JIT self-test passed. For the upstream desktop fallback, use `xcrun devicectl list devices` on a paired Mac, copy the target CoreDevice ID and run `./scripts/launch_on_duo.sh DEVICE_ID SIGNED_BUNDLE_ID` with the guest shut down. Check the console for execution at both RX test offsets.
4. Select the prepared VM in the library, launch it and verify the desktop and Windows build using Win+R then `winver`. Shut Windows down from the guest and relaunch the app. Confirm the firmware and guest edits persisted.
5. Verify portrait and landscape display, touch and physical keyboard input, trackpad or mouse, audio output and mic permission, networking, guest file sharing and the guest-to-Safari OAuth path. With a focused Windows text field, use the system paste action to type a single line of ASCII and verify it stops when you leave the session. The framebuffer test checks presentation.
6. Export `boot_status.txt` through Files after checking it for details you wish to redact. Record physical RAM, process headroom, virtual and resident size, physical footprint, memory warnings, actual guest RAM and JIT capacity. Share screenshots of the guest only after checking for personal content. Keep pairing records and signing credentials local.
7. Test sparse import with a broken archive and with a valid raw disk already present. Verify the archive stays in place and the existing disk remains unchanged. Collect file logical size and allocated size separately.

Exercise sustained guest load and record JIT flushes, memory pressure and allocation failures. Test interruptions during download, disk preparation, Windows Setup and first guest boot. Record the last completed stage and resulting disk state.

Report status separately as implemented, static validation passed, compilation passed, simulator tested, physical device tested or unverified. A locally validated archive fixture establishes verifier behavior. An actual device build and a device run establish the corresponding binary and runtime results.

## Library, media and preparation checks

The portable suite uses Swift when `swiftc` is on PATH or set through `SWIFTC`. For the filesystem fixture tests, set `GENISOIMAGE`, `UDFREAD_HEADER` and `UDFREAD_LIBRARY` to genisoimage, the pinned libudfread source header with `blockinput.h` alongside it, and a host static archive built from the revision in `scripts/build_ios_deps.sh`. Linux host SHA-256 tests link against OpenSSL. The fixtures cover UDF, Joliet, ISO9660, fallback from unreadable UDF, multi-extent copying, malformed extents, architecture, build and checksum ordering.

On the signed physical device, run the prepared-disk smoke test and then verify the following.

1. Import a known-good machine folder in **Virtual Machines**. Verify the legacy Files path remains selectable. Rename the managed VM, edit its hardware, duplicate it, export a stopped copy to Files and reimport it. Check the copies have separate writable firmware and that declining deletion preserves them.
2. Verify the power-button action follows Windows’ configured power policy. Shut the guest down from Windows. Reopen the app before launching another VM. Verify active disk images are protected from management operations.
3. Run the independent RAM and JIT matrix below. Start with the Compatibility preset. Quit the app between rows so the next VM can request its own mapping. Select the VM before enabling the debugger.
4. Import a legitimate 26100 ARM64 ISO and verify other builds, x64 and malformed images are rejected. Test the supplied authoritative checksum and a mismatch. Download the same ISO with a deliberately incorrect checksum, relaunch the app, and use **Export ISO**. Provide the correct checksum in Downloads and use **Revalidate ISO**. Confirm no new network transfer starts. Verify filesystem and parser details can be shared. Test pause, backgrounding, network interruption, relaunch and resume with a server that supports byte ranges. Verify explicit removal requires confirmation.
5. Select the ISO, configure memory, JIT cache and storage, then press **Install**. Leave Advanced Settings at its defaults. Test reopening the draft before and during preparation. Confirm the app publishes one VM with independent firmware and a sparse image. Repeat with a second VM and check that its firmware changes do not affect the first. Compare logical capacity with allocated bytes.
6. Choose **Start Windows Setup** on the blank machine. Record whether EDK2 finds the installer and whether WinPE locates the exFAT payload. Select an edition in the wizard for the initial blank-disk path. Choose the unallocated primary drive with the configured capacity. The Setup medium and 128 MiB diagnostics disk are separate devices. Interrupt a disposable run after partitioning and verify the next launch falls back to interactive Setup. Capture any Secure Boot or TPM rejection. With compatible firmware, check Secure Boot in Windows System Information and TPM 2.0 in Windows Security. Verify Windows accepts the bundled NetKVM catalog, the NIC starts and Windows can obtain a network connection. Record the driver version and Windows signature state.
7. After Setup has completed and the guest has shut down, use **Use Installed Windows Disk**. Verify it rejects a blank disk. For an installed disk, check the app detects the ARM64 boot manager and BCD. Relaunch the app, boot the primary disk, and complete standard OOBE and activation. Verify removal of the generated installer leaves the separate ISO intact. Save and reimport a local TPM-backed VM backup and verify the original TPM state survives.

For the Linux installer tests, stage FatFs with `scripts/prepare_fatfs.sh` and set `FATFS_SOURCE_DIR`, `MTYPE` and `FSCK_EXFAT` along with the UDF fixture variables. Add `WIMLIB_HEADER`, `WIMLIB_LIBRARY` and `WIMLIB_IMAGEX` for the real-container WinPE test. These fixtures contain synthetic files, not proprietary Windows data.

To run the genuine-media tests, supply a lawfully obtained complete ISO through `WINDOWS_24H2_ARM64_ISO` and its known test digest through `WINDOWS_24H2_ARM64_SHA256`. The Swift test validates the complete file and requires ARM64 build 26100. With the wimlib variables set, the installer test extracts the real boot.wim, checks its architecture and build, and verifies that its boot index contains the Setup launcher.

The 24H2 IoT Enterprise LTSC evaluation image served by Microsoft was 5,042,194,432 bytes and gave SHA-256 `3dcdba9c9c0aa0430d4332b60c9afcb3cd613d648a49cbba2d4ef7b5978f32e8`. Microsoft’s linked PDF lists `ccec358a760c3c581249f091ed42d04f37b2b99c347b7a58257c3cc272d7982c`. This discrepancy limits the artifact to local parser and provisioning diagnostics. To reproduce the rejection, set `WINDOWS_24H2_MISMATCHED_PUBLISHED_SHA256` to the PDF value. The test must report SHA-256 mismatch before filesystem detection. The downloaded artifact was never redistributed. [Microsoft Evaluation Center](https://www.microsoft.com/en-us/evalcenter/download-windows-11-iot-enterprise-ltsc-eval) · [Microsoft verification PDF](https://cdn-dynmedia-1.microsoft.com/is/content/microsoftcorp/microsoft/final/en-us/microsoft-brand/documents/Windows11IoTEnterpriseLTSC2024EvalHashValues.pdf)


For the host TPM integration test, set `TPMS_INCLUDE` to the directory containing the libtpms headers and `TPMS_LIBRARY` to a host libtpms library. The suite also checks the emitted ACPI table and FDT without needing an iOS SDK.


The ARM64 UEFI integration test uses `QEMU_AARCH64`, `EDK2_FIRMWARE`, `EFI_CLANG` and `EFI_LLD`, with `QEMU_DATA_DIR` when the ROMs are in a separate path. After running `python3 scripts/prepare_installer_assets.py`, set `WINDOWSIOS_INSTALLER_ASSETS` to the staged InstallerAssets directory to exercise the exact bundled pair as QEMU pflash banks. The probe boots a synthetic EFI application from the generated NVMe installer. The same assets setting checks the bundled firmware header and the real NetKVM files in Swift.

## Document picker and Imports verification

1. On iPadOS 27.0.1, import a multi-gigabyte ISO from **On My iPad** and from iCloud Drive or another Files provider. Record requested, presented, callback, closed and coordinated read events in **Diagnostics → Export Picker Diagnostics**. Check that the picker closes before copying and that the copy and SHA-256 stages report byte progress. Do a separate cancel and swipe dismissal in each context, including the wizard's Downloads sheet and Advanced Settings.
2. Exercise a firmware FD, a NetKVM folder, a pairing plist, a prepared VM folder, and standalone IMG or HVSPARSE. Verify the selected target and inspect user-friendly errors for wrong formats. Confirm source file size, checksum or modification date stays unchanged. Check that pairing diagnostics contain no record or path.
3. In Files, copy an ISO into **On My iPad → WindowsiOS-ez → Imports**. Return to Downloads and verify the ISO appears. Check **Refresh Imports**, import from the row, and verify the original remains in Files. Test moving a file into the folder in Split View while the app is active.
4. Export a retained failed download and a non-TPM VM backup to Files. Confirm success and cancel both dismiss the picker. Revalidate the retained ISO without a network transfer.

To enable the host 2.25 GiB ISO streaming test, set `WINDOWSIOS_TEST_LARGE_ISO=1` on Linux with GNU dd and at least 2.5 GiB of free workspace storage. The test creates a sparse synthetic source and checks that the managed copy validates with the same digest while the original digest stays unchanged. Files provider UI and iPad picker callbacks require the device pass above.


## Critical installer and JIT retest

Start each row in a fresh signed app process on the same iPad. Select the VM, attach the authorized debugger and check the executable self-test at both RX offsets. After a successful boot record requested and mapped JIT, guest RAM reservation and allocation, process headroom, footprint, memory warnings and cache flushes. Export `boot_status.txt` after a failed launch as well. Check runtime entitlement values for debugging, increased memory and extended virtual addressing.

| Run | Guest RAM | JIT cache |
| --- | ---: | ---: |
| Compatibility | 4608 MiB | 384 MiB |
| JIT only | 4608 MiB | 512 MiB |
| RAM only | 6144 MiB | 384 MiB |
| Performance | 8192 MiB | 1024 MiB |
| Experimental | 10240 MiB | 1536 MiB |

Host tests check these size combinations and mapping boundaries. They do not execute the TXM allocator or measure iPad memory pressure.

For a VM created on an older app build, select **Update Windows Setup Disk** from the VM menu. It stages a replacement Setup disk and keeps a backup of the previous image. Run Setup with ARM64 Consumer media and IoT Enterprise LTSC media in separate disposable VMs. Select the unallocated primary disk by its configured size. After Setup returns a result, use **R** to restart once Setup has finished staging Windows and logs you need are collected. On failure choose **M** for interactive Setup without the optional answer file or driver flags. Use the stopped VM's **Export Windows Setup Logs** action to save `launcher.log`, Panther logs and SetupAPI text logs. Review logs for guest details before sharing.

Repeat the host WinPE/source-plan test with each media file by setting `WINDOWS_24H2_ARM64_ISO` and its measured or authoritative `WINDOWS_24H2_ARM64_SHA256`. The source-plan test validates ARM64 `sources/setup.exe` and reaches installer progress before cancelling to avoid duplicating the full ISO. The separate FAT32 diagnostics test verifies persistence and read-only export of a sample signed-decimal Setup exit code.

After a device Setup run, check whether Windows enumerated TPM 2.0, report Secure Boot state and confirm the NetKVM provider and driver version in Device Manager. Check the SetupAPI log for driver selection and signature acceptance.

## NVMe and EFI stall investigation

Run `python3 -m unittest discover -v -s tests -p 'test_nvme_storage.py'` to exercise the real controller against sparse host files. The test covers CQ backpressure with shared submission queues, wraparound and phase, concurrent doorbells, completion timeout reporting, INTx and MSI-X routing to a test ITS endpoint, invalid DMA and NVRAM persistence across reopen. This harness does not run the Windows storage driver or the iPad GIC path.

For a signed device retest, use a disposable VM and update its generated Setup disk before launching. Keep the original affected VM and ISO for comparison. First reproduce with Compatibility, then repeat with the reported 10 GiB and four vCPUs. At **Finishing up**, if access to the ESP stalls, let the VM run for at least two diagnostic snapshots, then copy `nvme_status.txt` from the app's Files folder before any reboot. Save `boot_status.txt`, Panther and SetupAPI logs, the exact BCD entry from `setuperr.log`, and the path of the stalled command. If `mountvol F: /L` returns, record its volume GUID. Record which backing disk hosts the ESP if WinPE has already provided that information. Avoid repair or repartitioning commands on the original disk while collecting evidence.

- A timed-out `active_request` at `pread`, `pwrite` or `flush` points to the host file I/O operation. The report identifies the controller, queue, opcode and starting LBA.
- An SQ with outstanding entries, CQ backpressure and unacknowledged completions indicates the guest has not advanced the CQ head. `last_signal=msix_unrouted` shows an interrupt had no route. `msix_routed` means the message reached the interrupt controller, which can still mask it or wait for the guest to acknowledge it.
- Empty NVMe queues and no active requests during repeated stalled reads shift the investigation toward Windows volume handling or BCD. `nvram_persist_errno` reports firmware-variable failures separately.
