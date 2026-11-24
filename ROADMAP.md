# Roadmap

| Milestone | Work | Validation gate |
| --- | --- | --- |
| 0. Baseline | Preserve history, document the audit, remove personal build settings and package an unsigned device IPA | Manual Xcode 27.2 Release build, artifact checks, signing smoke test and prepared-disk boot |
| 1. Native foundation | VM library, independent settings and firmware, manual import, RAM presets and diagnostics | Verify old machine folders still boot. Test import, config persistence and lifecycle on iPhone and iPad |
| 2. Standalone JIT | Authorized JIT setup, runtime self-test, allocation diagnostics, cache sizing and explicit fallback | Verify entitlement behavior and JIT on physical devices. Exercise cache edges, flushes and sustained guest load before declaring larger configurations compatible |
| 3. Installation | ARM64 26100 media validation and acquisition, sparse disk and NVRAM preparation, WinPE provisioning with persistent checkpoints | Boot WinPE from a locally constructed installation disk, apply Windows, install signed drivers, boot to standard OOBE, and recover safely from interruption |
| 4. Stabilization | Physical-device regression testing, memory and storage refinements, release documentation | Complete the matrix in [TESTING.md](TESTING.md) on the target hardware |

## Installation dependencies

1. Validate ISO/UDF and Windows PE/WIM metadata before accepting media, with a Files import workflow for legitimately obtained 24H2 media.
2. Define compatible, redistributable EDK2 code and variable templates and validate their provenance. Copy writable variables for each VM.
3. Create a sparse NVMe target and construct a separate bootable installer disk with native filesystem tooling. Attach it to the existing second NVMe slot.
4. In WinPE, partition the target, use DISM to apply the selected 26100 ARM64 edition, install signed ARM64 drivers, and use BCDBOOT to establish the EFI boot path. Use startup and shutdown signals to advance persistent stages.
5. Boot the installed disk and let Windows handle account setup and activation. Verify Secure Boot and TPM requirements before advertising supported default installation. Keep imported media reusable.

The planned presets use 4608/384, 6144/512, 8192/1024 and 10240/1536 MiB of guest RAM and JIT cache respectively. Custom vCPUs must respect the engine cap. The 10240/1536 MiB preset requires physical validation. The initial 64 GiB virtual disk size and the planned 128 GiB, 256 GiB and custom options describe logical capacity. Real allocated space grows as the guest writes.
