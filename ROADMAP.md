# Roadmap

| Milestone | Current work and next validation gate |
| --- | --- |
| 0. Baseline | The workflow and artifact checks are in place. Run a manual Xcode 27.2 Release build, check the IPA and signing, then boot a prepared disk on the target device. |
| 1. Native foundation | Verify the library, independent firmware and prepared import/export on iPhone and iPad. Add complete engine teardown before exposing in-process machine switching or restart. |
| 2. Standalone JIT | Validate the existing authorized debugger workflow and memory diagnostics on the target device. Integrate authorized local pairing when the helper and signing requirements are validated. Larger caches need proof of mapping placement, executable permission, cache flushing and sustained guest load. |
| 3. Installation | Verify media validation and persistent disk preparation on-device. Build and test the bootable WinPE installer disk, then validate provisioning and standard OOBE using the actual guest. |
| 4. Stabilization | Complete the physical matrix in [TESTING.md](TESTING.md), use measured results to address bottlenecks, and document release compatibility. |

## Windows deployment gates

1. Build and sign the app for a real iPad. Boot a prepared Windows 11 24H2 image with Compatibility before testing Setup or larger caches.
2. Build the GPT installer from legitimate 26100 media. Verify that ARM64 EDK2 boots its FAT32 partition and that WinPE locates the exFAT payload. Test the native launcher with a WIM and supported ESD media.
3. Verify DISM provisioning on a new VM with compatible Secure Boot firmware and TPM 2.0. Confirm the GPT identity check chooses the primary disk, DISM applies the selected edition and accepts the signed ARM64 NetKVM package, and BCDBOOT and BCDEDIT finish.
4. Boot the primary disk and complete standard Windows account setup and activation. Interrupt a disposable VM during deployment and confirm that another launch preserves the partially installed disk. Verify export of the failure logs.
5. Test the experimental Graphical Setup method and its optional edition-only answer file separately. Confirm primary EFI checks, installer cleanup and the intact source ISO for both methods.

See [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) for the locally verified portions.

## Windows interface and input

1. Validate the Machines, Install and Settings redesign and open an existing VM without reinstalling. Verify GameController mouse buttons, trackpad dragging, host cursor visibility, keyboard handling and portrait/landscape input on the target iPad.
2. Validate framebuffer presentation, storage accounting and the session details against device measurements. The bundled GOP exposes 640×480, 800×600 and 1024×768. Configurable guest modes beyond that require compatible firmware or a Windows display driver and separate boot validation.
3. Add authenticated Windows guest integration before offering automatic clipboard synchronization. Evaluate safe offline disk compaction and TRIM, then separate external display routing. Snapshots, instant resume, persistent JIT caching and GPU virtualization remain separate projects.
