# Implementation status

| Work | Status and evidence |
| --- | --- |
| Audit, baseline and reference | Implemented and documented in [COMPATIBILITY.md](COMPATIBILITY.md). |
| Manual Codemagic workflow | Implemented. Official Codemagic YAML schema validation passed. Actual arm64 iOS build, cache timing, IPA emission and checksum are unverified. |
| External signing and memory entitlement request | Implemented. XcodeGen and the tracked manifest agree in static validation. Generated Xcode settings and effective entitlements on a signed device build are unverified. |
| Memory diagnostics | Existing process headroom retained. Added physical memory, process virtual/resident/footprint measurements, UIKit memory-warning count and optional running-signature entitlement checks in `boot_status.txt`. Objective-C compilation and device behavior are unverified. |
| Prepared sparse disk import | Implemented. Portable C compilation and sparse format, round-trip and no-overwrite tests passed. iOS integration is unverified. |
| RAM/JIT tuning | Pending. The current app retains its upstream automatic RAM selection, the 384 MiB JIT reservation and four vCPUs. Presets, allocation recovery and custom sizing need lifecycle and device validation. |
| VM library | Pending. The existing session continues to recognize a prepared machine in Files. Independent machines, native management and export are pending. |
| Authorized JIT | Existing external StikDebug and LLDB workflow retained, including an executable self-test. Integrated pairing, runtime readiness UI improvements and device testing are pending. |
| Windows media, provisioning and wizard | Pending. A bootable WinPE NVMe image still requires desktop tooling. Native ISO/WIM validation, acquisition and recovery, firmware preparation, installer construction, provisioning checkpoints and OOBE verification are pending. |
| Stabilization | Pending. No simulator or physical-device run performed for this branch. Windows boot, JIT activation, audio, networking and high-memory settings remain unverified. |

Nine portable automated tests passed in Linux. Shell syntax, Python syntax, XcodeGen YAML and entitlement manifest consistency, Codemagic schema and Git whitespace checks passed. `make test-core` did not compile because the environment lacks Clang and the Apple ARM64/Mach toolchain. The unsigned IPA script rejected the current Linux host at its explicit platform check.

The manual Xcode 27.2 build and signed baseline boot are the next validation gates. See [BUILDING.md](BUILDING.md), [TESTING.md](TESTING.md) and [ROADMAP.md](ROADMAP.md).
