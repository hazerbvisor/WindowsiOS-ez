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

## MetalGPU and session changes

Run `python3 -m unittest discover -s tests -p test_virtio_gpu.py` to compile and exercise the portable 2D device with the real PCI and split-queue code. For Clang ASan/UBSan also set `VIRTIO_GPU_TEST_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"`. See [the graphics compatibility procedure](docs/METALGPU_COMPATIBILITY.md) for testing PCI enumeration, signed driver installation and RAMFB recovery on a copied VM.

On the RAMFB baseline, hide the toolbar with the chevron in portrait and landscape, including with the toolbar positioned at the bottom. Verify it closes any keyboard and zoom controls and that the display remains interactive. Use the four-finger tap or the input surface's accessibility action to reopen it. Repeat with physical mouse or trackpad, and with auto-hide enabled. In Session Details, choose **Change Windows Resolution**, confirm Windows Settings opens, and try a lower and higher resolution if Windows offers them. Compare the guest dimensions in Windows and Session Details, then verify pointer alignment. Confirm host scaling leaves the displayed guest dimensions unchanged.

With balanced RAM/JIT and RAMFB, test a known-good four-vCPU boot, then 5, 6, 7 and 8 vCPUs independently. Verify the guest processor count after each restart, run a multi-threaded guest workload, and exercise guest restart and app interruptions. Export diagnostics and record times, warnings and any regressions. Use the last known stable count after a failed run.

## Library, media and preparation checks

The portable suite uses Swift when `swiftc` is on PATH or set through `SWIFTC`. For the filesystem fixture tests, set `GENISOIMAGE`, `UDFREAD_HEADER` and `UDFREAD_LIBRARY` to genisoimage, the pinned libudfread source header with `blockinput.h` alongside it, and a host static archive built from the revision in `scripts/build_ios_deps.sh`. Linux host SHA-256 tests link against OpenSSL. The fixtures cover UDF, Joliet, ISO9660, fallback from unreadable UDF, multi-extent copying, malformed extents, architecture, build and checksum ordering.

On the signed physical device, run the prepared-disk smoke test and then verify the following.

1. Import a known-good machine folder in **Virtual Machines**. Verify the legacy Files path remains selectable. Rename the managed VM, edit its hardware, duplicate it, export a stopped copy to Files and reimport it. Check the copies have separate writable firmware and that declining deletion preserves them.
2. Verify the power-button action follows Windows’ configured power policy. Shut the guest down from Windows. Reopen the app before launching another VM. Verify active disk images are protected from management operations.
3. Run the independent RAM and JIT matrix below. Start with the Compatibility preset. Quit the app between rows so the next VM can request its own mapping. Select the VM before enabling the debugger.
4. Import a legitimate 26100 ARM64 ISO and verify other builds, x64 and malformed images are rejected. Test the supplied authoritative checksum and a mismatch. Download the same ISO with a deliberately incorrect checksum, relaunch the app, and use **Export ISO**. Provide the correct checksum in Downloads and use **Revalidate ISO**. Confirm no new network transfer starts. Verify filesystem and parser details can be shared. Test pause, backgrounding, network interruption, relaunch and resume with a server that supports byte ranges. Verify explicit removal requires confirmation.
5. Select the ISO, configure memory, JIT cache and storage, then press **Install**. Leave Advanced Settings at its defaults. Test reopening the draft before and during preparation. Confirm the app publishes one VM with independent firmware and a sparse image. Repeat with a second VM and check that its firmware changes do not affect the first. Compare logical capacity with allocated bytes.
6. Start **DISM Provisioning** on the new VM. Record whether EDK2 finds the installer and whether WinPE locates the exFAT payload, matches the target GUID and drives DISM to 100%. The installer and 128 MiB diagnostics disk are separate devices. With compatible firmware, check Secure Boot in Windows System Information and TPM 2.0 in Windows Security after first boot. Verify Windows accepts the bundled NetKVM catalog, the NIC starts and Windows can obtain a network connection. Record the driver version and Windows signature state.
7. After provisioning has completed and the guest has shut down, use **Use Installed Windows Disk**. Verify it rejects a blank disk. For an installed disk, check the app detects the ARM64 boot manager and BCD. Relaunch the app, boot the primary disk, and complete standard OOBE and activation. Verify removal of the generated installer leaves the separate ISO intact. Save and reimport a local TPM-backed VM backup and verify the original TPM state survives.

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

For an experimental Graphical Setup VM created on an older build, use **Update Windows Setup Disk** if you need to retest that method. It stages a replacement and keeps a backup of the previous image. Select the unallocated primary disk by its configured size. After Setup returns a result, use **R** to restart once Setup has staged Windows and you have saved the logs you need. On failure choose **M** for interactive Setup without the optional answer file or driver flags. Use the stopped VM's **Export Installation Logs** action to save `launcher.log`, Panther logs and SetupAPI text logs. Review logs for guest details before sharing.

Repeat the host WinPE/source-plan test with each media file by setting `WINDOWS_24H2_ARM64_ISO` and its measured or authoritative `WINDOWS_24H2_ARM64_SHA256`. The source-plan test validates ARM64 `sources/setup.exe` and reaches installer progress before cancelling to avoid duplicating the full ISO. The separate FAT32 diagnostics test verifies persistence and read-only export of a sample signed-decimal Setup exit code.

After a device Setup run, check whether Windows enumerated TPM 2.0, report Secure Boot state and confirm the NetKVM provider and driver version in Device Manager. Check the SetupAPI log for driver selection and signature acceptance.

## NVMe and EFI stall investigation

Run `python3 -m unittest discover -v -s tests -p 'test_nvme_storage.py'` to exercise the real controller against sparse host files. The test covers CQ backpressure with shared submission queues, wraparound and phase, concurrent doorbells, completion timeout reporting, INTx and MSI-X routing to a test ITS endpoint, invalid DMA and NVRAM persistence across reopen. This harness does not run the Windows storage driver or the iPad GIC path.

For a signed graphical retest, use a separate disposable VM. Keep the original affected VM and ISO for comparison. First reproduce with Compatibility, then repeat with the reported 10 GiB and four vCPUs. At **Finishing up**, if access to the ESP stalls, let the VM run for at least two diagnostic snapshots, then copy `nvme_status.txt` from the app's Files folder before any reboot. Save `boot_status.txt`, Panther and SetupAPI logs, the exact BCD entry from `setuperr.log`, and the path of the stalled command. If `mountvol F: /L` returns, record its volume GUID. Record which backing disk hosts the ESP if WinPE has already provided that information. Avoid repair or repartitioning commands on the original disk while collecting evidence.

- A timed-out `active_request` at `pread`, `pwrite` or `flush` points to the host file I/O operation. The report identifies the controller, queue, opcode and starting LBA.
- An SQ with outstanding entries, CQ backpressure and unacknowledged completions indicates the guest has not advanced the CQ head. `last_signal=msix_unrouted` shows an interrupt had no route. `msix_routed` means the message reached the interrupt controller, which can still mask it or wait for the guest to acknowledge it.
- Empty NVMe queues and no active requests during repeated stalled reads shift the investigation toward Windows volume handling or BCD. `nvram_persist_errno` reports firmware-variable failures separately.

## Default DISM provisioning retest

1. On a manually built and signed device build, create a new 64 GiB VM with Compatibility hardware and a verified Windows 11 24H2 ARM64 ISO. Leave the installation method at **DISM provisioning**. Use separate new VMs for Consumer WIM and IoT Enterprise LTSC. Use ARM64 build 26100 ESD-in-ISO media for a separate ESD test when available.
2. Launch provisioning. In the guest console confirm WinPE starts, its GUID matches exactly one disk, and DiskPart formats partitions 1 and 3. Confirm that DISM's real percentage reaches 100%, the signed offline NetKVM injection completes, BCDBOOT succeeds, and BCDEDIT can open the BCD store. Save the selected image index and edition. The default prefers Windows 11 Pro if present. A single-edition LTSC image should use its only index.
3. Wait for guest shutdown. Return to the library and choose **Use Installed Windows Disk**. Relaunch the app if required by the existing single-session engine. Start the VM from its primary disk and complete standard Windows account setup. Record TPM, Secure Boot, network driver and activation state.
4. Interrupt a second disposable run after DISM starts. Confirm the existing target remains available and another automatic DISM launch refuses it. Use **Export Installation Logs** from the stopped VM. Save `provision-*.log`, `identify-*.log`, `partition-*.log`, `dism-*.log`, `bcdboot-*.log` and `bcd-*.log` as available. Review the files for personal information before sharing. A fresh VM can reuse the same ISO for another automatic attempt.

## Henge-inspired UI and input retest

Run `python3 -m unittest discover -v -s tests -p test_session_input.py` with Swift and a Linux C compiler. The tests check source-specific held buttons, proportional coordinates after rotation, zoom bounds, and the actual xHCI report queue's button coordinates and releases under saturation. These tests do not drive UIKit or GameController.

1. Sign and install the new build over the baseline app using the same bundle ID and profile. Start a copy of an existing Balanced VM without importing or installing Windows again. Verify the disk, firmware, TPM state and hardware settings survive. Check that the latest desktop preview belongs to the selected VM. Confirm system, light and dark appearance on iPad and iPhone.
2. In landscape, hide the control capsule and reveal it with four fingers. Check the VoiceOver **Show Windows controls** action. In portrait without external input, verify the lower touch trackpad and left and right buttons. With a physical keyboard or mouse attached, verify that Windows uses the available screen area.
3. Move a Bluetooth mouse and Magic Keyboard trackpad between the guest display, touch trackpad and native controls. Check that the iPadOS pointer hides only over guest surfaces and returns over the toolbar, Details and Machines. Repeat with the pointer visibility preference disabled.
4. Test left, right and middle physical buttons. Hold the button while moving windows, resizing, dragging a file, highlighting text and moving a scrollbar. Check touchscreen long-press drag, relative tapping and two-finger scrolling. Disconnect a mouse and open Details mid-drag, then check there is no stuck button. Repeat after app backgrounding and a rotation.
5. Use a hardware keyboard with the software keyboard hidden. Validate Ctrl+C, Ctrl+V, Ctrl+X, Ctrl+Z, Alt+Tab, Alt+F4, Ctrl+Shift+Esc, Windows+E and Windows+R. Hold Shift while the app loses focus and check it is released on return. Test the optional keyboard bar in Windows and WinPE.
6. Compare the guest resolution in Session Details with Windows Display settings. Exercise Fit, Fill, Pixel Perfect and Zoom and retest pointer coordinates. The presenter settings use real timer limits and the screen refresh ceiling. Check 30 and 60 FPS plus up to 120 FPS on supporting hardware. The firmware chooses the guest mode. No guest Retina or custom mode is exposed by this build.
7. Compare VM allocated bytes with its logical sparse capacity, then inspect the Storage breakdown. Test explicit keyboard paste and the Unicode shared text-file action. Verify a new ISO import, the Files Imports inbox and default DISM provisioning. Smoke-test boot, networking, audio, Safari guest sign-in and WebDAV sharing.

For the optional DXMT dependency probe, on an authorized Xcode 27.2 macOS build machine with Meson/Ninja, Metal tools and target iPhoneOS LLVM 15.0.7 installed, run `DXMT_IOS_LLVM_PREFIX=/path/to/target/llvm bash scripts/build_dxmt_probe.sh`. This verifies the produced dylib architecture, iOS platform and DX11 exports, not runtime functionality. Do not add the dylib to an IPA until licensing, dependencies and signing are validated. Test the new opt-in on a copied installed VM; check that blank-disk installation cannot enable it, RAMFB boots on opt-out, driver ZIP checksum errors are reported, and the guest installer does not run without explicit user action.

After the native dependency probe succeeds, run `bash scripts/package_dxmt_smoke.sh` to build a separate unsigned developer IPA using the pinned upstream DXMT offscreen suite. It verifies iPhoneOS/ARM64 and rejects unresolved non-system dylibs. Sign the app and nested dylib using the normal developer signing workflow. Launch using a supported developer launch tool, not the ordinary Windows VM interface. Retrieve `Documents/dxmt_results.txt` and `Documents/dxmt_stderr.txt`; record actual PASS/FAIL, device creation, CRC and fence results. The probe has no VirtIO/Neptune connection and does not prove guest acceleration. No build has been run here.

Native Neptune adapter build: after the DXMT probe, supply a target iPhoneOS arm64 prefix containing libepoxy and epoll-shim and run `NEPTUNE_IOS_DEPS_PREFIX=/path/to/ios/deps bash scripts/build_neptune_backend.sh` on an authorized Xcode build machine. Verify the dependency report and code signing before loading the dylibs. This does not run a guest; the next required tests include malformed command rejection, actual capset/context handshake, blob overlap and unmap, exact fence retirement, dynamic BAR mappings with guest atomics and native scanout. Keep the driver disabled until these gates pass.

### MetalGPU lab IPA
On Apple Silicon macOS with the required Xcode 27.2, target LLVM 15.0.7, and a physical-iOS ANGLE/libepoxy/epoll-shim prefix, run `WINIOS_NEPTUNE_LAB=1 DXMT_IOS_LLVM_PREFIX=/path/to/llvm NEPTUNE_IOS_DEPS_PREFIX=/path/to/deps bash scripts/build_unsigned_ipa.sh`. The separate artifact is `build/artifacts/metalgpu-lab/WindowsiOS-ez.ipa`. Sign the app and all embedded dylibs, then test a **copy** of an installed VM: one vCPU, graphics opt-in. Verify diagnostics show Neptune-LAB before installing the signed 3D package from Session Details. Check Device Manager, dxdiag DDI/feature level and DWM, then run an actual DX11 rendering test and collect host fence/scanout diagnostics. If driver initialization or display fails, shut down and relaunch with graphics off. Do not promote this build or report acceleration without passing these tests. No build has been run here.

### iPad display modes
Run `cc -std=c11 -Wall -Wextra -Werror tests/test_gpu_modes.c -o /tmp/winios-gpu-modes && /tmp/winios-gpu-modes`. With an installed VM and VirtIO-GPU enabled, choose a guest resolution in Machine Settings, save, relaunch, and select the advertised custom mode in Windows Display settings. Verify 1180x820, 1770x1230 and 2360x1640 on 11-inch iPad; verify the 13-inch presets separately. Check that host fit scaling preserves aspect and that RAMFB install/recovery still uses firmware modes. Both signed v0.6 drivers populate a custom slot from GET_DISPLAY_INFO; the EDID independently advertises common PC timings. These are not hot-switch or automatic Windows mode setting features.

### Precompiled MetalGPU CI variant
The temporary build branch downloads official UTM artifact 14412813611 from run 44272292460 (pinned UTM commit 3d4b8d8), verifies its published archive SHA-256, physical iOS architecture and renderer symbols, and uses its native DXMT and virglrenderer directly. ANGLE and transitive dylibs are packaged as frameworks so libepoxy's dynamic library lookup works. No LLVM/ANGLE/DXMT/virglrenderer source builds occur in this mode. The normal WindowsiOS installer dependencies and app still compile. Artifact expiration or cross-repository access failure is an explicit build error, not a fallback to compiling the full graphics stack. Sign the app and embedded frameworks together. Device acceleration remains unverified.

### Experimental SMP lab
MetalGPU permits 1–8 vCPUs in the installed-VM opt-in. GPU command processing and deferred reset stay on CPU0. Aperture accesses hold the device lock; mapping changes and CPU aperture writes advance an exclusive epoch. Test 1, 2, 4 and 8 vCPUs on backed-up VMs, including reset, D3D11 rendering, concurrent aperture access and RAMFB recovery. Existing 2D regressions are not SMP/Metal validation. Physical-device results remain unverified.

### 10 GiB experimental budget
Experimental preset: guest 7680 MiB, JIT 1536 MiB, Neptune tracked resource/backing budget 1024 MiB (on demand); selecting preset sets eight vCPUs, adjustable 1–8. Both memory entitlements stay enabled in the manifest and XcodeGen. Additional memory can be consumed by JIT metadata, GPU driver/renderer and the OS; 10 GiB is not a process footprint cap. Explicit MetalGPU RAM launch checks include actual mapped JIT, GPU budget and 768 MiB physical headroom. Test on the iPad with runtime entitlement diagnostics, physical footprint and memory warnings; no device performance or stability result is claimed.

### MetalGPU protocol compatibility candidate
Pinned upstream UTM driver sends typed SET_SCANOUT and fenced TRANSFER_HOST_3D. Route typed scanout through the renderer's retained Metal handle and defer ring-fenced transfers until actual renderer retirement. Neptune context fences use the guest fence ID (ARM_EVENT_FENCE pairs that ID). Report per-opcode errors and tracked Neptune resources, native dimensions, fence submissions/retirements. Validate with 1 then 6 vCPUs on disposable guest copies; confirm desktop, dxdiag, scanout updates and compare diagnostic errors. Existing typed format, geometry, context and resource caps remain; no claim of complete protocol or physical GPU validation.

### Container shared-memory candidate
The second iPad diagnostic showed Metal-native scanout and four retired fences, but repeated CTX_CREATE/DESTROY and dependent blob/attach errors. Pinned UTM renderer's Apple anonymous file code uses named shm_open, which can require an app-group entitlement not provisioned for this app. Rebuild only virglrenderer using explicitly selected unlinked files inside NSTemporaryDirectory; preserve signed entitlements and pinned precompiled DXMT/ANGLE. Host test checks shared mapping, file unlink, FD_CLOEXEC, and invalid paths. Align context teardown with upstream semantics, fail abandoned fences, collect exact capset and renderer return codes. On-device causality and full graphics stability remain unverified; compare new context diagnostics and error counters.

### Native presentation candidate
Device report after shared-memory fix: 85/85 context creations, 456/456 retired fences, three errors in 1246 commands, 22 scanout updates. Guest lock screen becomes visible on stop when native scanout is cleared to RAMFB. Test stable ANGLE external share root, periodic native re-sampling and retirement-triggered updates; presenter diagnostics distinguish submitted, completed, actually presented, invalid texture and drawable failures. Test native lock-screen interaction, resize, 1/6 vCPU, sustained rendering and stop/restart. No claim of explicit cross-API GPU synchronization or physical-device verification.
