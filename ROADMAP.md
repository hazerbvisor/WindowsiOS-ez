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
3. Verify standard Windows Setup with compatible Secure Boot firmware and TPM 2.0. The current board lacks TPM 2.0 and the app keeps Setup's normal hardware requirements.
4. Confirm Setup applies the chosen edition, creates EFI boot files and accepts the signed ARM64 NetKVM package. Boot the primary disk and complete standard Windows account setup and activation.
5. Add verified guest checkpoints and unattended DISM/BCDBOOT only after the stock Setup path is confirmed. Resume safely from a partially written primary disk and clean up the temporary installer while keeping the original ISO.

See [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) for the locally verified portions.
