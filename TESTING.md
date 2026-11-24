# Verification

## Local tests

Run these from the repository root.

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
for script in scripts/*.sh; do bash -n "$script" || exit; done
python3 -m py_compile scripts/*.py
git diff --check
```

The Python suite compiles the C sparse CLI with strict warnings, exercises a 64 GiB image round trip, tests malformed records, races two concurrent imports and verifies an existing disk is preserved. It also checks unsigned IPA and generated-setting validation against synthetic fixtures. On Apple Silicon macOS run `make test-core` and `./scripts/build_unsigned_ipa.sh` as separate checks.

## Physical-device baseline

Use a physical iPad Air M4 on iPadOS 27.0.1 and record the actual model, detected physical RAM, OS build, Git commit, Xcode version and sideloader. Use Windows 11 24H2 ARM64 build 26100 for each pass.

1. Verify the Codemagic checksums, sign the IPA, confirm the signed entitlements and install it with the device's sideloading workflow. Enable Developer Mode where required. Check that the signed app launches.
2. In Files, stage your lawfully prepared Windows disk and its matching EDK2 `code.fd` and `vars.fd` under the app's `Machines/Win11` folder. Use `windows11.hvsparse` or `windows11.img`. Keep a backup of the disk and the writable variable store. For a sparse import test, use a fresh machine directory with no raw disk.
3. Use the app's StikDebug action with an authorized debugger setup. Check that the runtime reports that its JIT self-test passed. For the upstream desktop fallback, use `xcrun devicectl list devices` on a paired Mac, copy the target CoreDevice ID and run `./scripts/launch_on_duo.sh DEVICE_ID SIGNED_BUNDLE_ID` with the guest shut down. Check the console for execution at both RX test offsets.
4. Launch the prepared VM and verify the desktop and Windows build using Win+R then `winver`. Shut Windows down from the guest and relaunch the app. Confirm the firmware and guest edits persisted.
5. Verify portrait and landscape display, touch and physical keyboard input, trackpad or mouse, audio output and mic permission, networking, guest file sharing and the guest-to-Safari OAuth path. The framebuffer test checks presentation.
6. Export `boot_status.txt` through Files after checking it for details you wish to redact. Record physical RAM, process headroom, virtual and resident size, physical footprint, memory warnings, actual guest RAM and JIT capacity. Share screenshots of the guest only after checking for personal content. Keep pairing records and signing credentials local.
7. Test sparse import with a broken archive and with a valid raw disk already present. Verify the archive stays in place and the existing disk remains unchanged. Collect file logical size and allocated size separately.

Once the larger configurations and installer are implemented, test each preset in order, exercise sustained guest load and JIT flushes, and record allocator rejection and fallback behavior. Test provisioning with an interruption at each stage, including during download, disk preparation, DISM, EFI setup and the first guest boot. Record the last completed stage and resulting disk state.

Report status separately as implemented, static validation passed, compilation passed, simulator tested, physical device tested or unverified. A locally validated archive fixture establishes verifier behavior. An actual device build and a device run establish the corresponding binary and runtime results.
