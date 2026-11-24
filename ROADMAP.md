# Roadmap

| Milestone | Current work and next validation gate |
| --- | --- |
| 0. Baseline | The workflow and artifact checks are in place. Run a manual Xcode 27.2 Release build, check the IPA and signing, then boot a prepared disk on the target device. |
| 1. Native foundation | Verify the library, independent firmware and prepared import/export on iPhone and iPad. Add complete engine teardown before exposing in-process machine switching or restart. |
| 2. Standalone JIT | Validate the existing authorized debugger workflow and memory diagnostics on the target device. Integrate authorized local pairing when the helper and signing requirements are validated. Larger caches need proof of mapping placement, executable permission, cache flushing and sustained guest load. |
| 3. Installation | Verify media validation and persistent disk preparation on-device. Build and test the bootable WinPE installer disk, then validate provisioning and standard OOBE using the actual guest. |
| 4. Stabilization | Complete the physical matrix in [TESTING.md](TESTING.md), use measured results to address bottlenecks, and document release compatibility. |

## Remaining native installer work

1. Stream the boot files from the verified UDF ISO into a GPT installer disk. ARM64 EDK2 must be able to boot its FAT32 partition. Store install WIM files over 4 GiB on an appropriate second partition, or use a validated split-WIM flow.
2. Attach that installer disk read-only to the engine’s existing secondary NVMe slot and boot Windows PE. Define a deterministic device and partition identity protocol so provisioning targets the intended VM disk.
3. Partition the target, use DISM to apply the selected 26100 ARM64 edition, inject signed ARM64 drivers, and use BCDBOOT to establish the EFI path. Store durable stage markers and verify them before restarting or reporting progress.
4. Validate the NVMe, firmware, Secure Boot and TPM requirements against standard Windows 11 setup. Boot the installed disk and allow Windows to handle account setup and activation.

The implemented media layer rejects other builds. The wizard currently creates the sparse disk and copies EDK2 firmware and variables from Files. It saves a recoverable preparation stage. See [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) for the current gates.
