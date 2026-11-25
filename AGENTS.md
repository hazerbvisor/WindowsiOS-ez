# WindowsiOS-ez repository guidance

- Keep work on a development branch and preserve the upstream GPL-2.0-or-later history and third-party notices.
- Use the manual Codemagic workflow in [BUILDING.md](BUILDING.md) for unsigned artifacts. Keep the Mac mini M2, Xcode 27.2, 20-minute limit and iOS 17.0 deployment target unless a documented technical requirement changes them. Trigger a remote build only with owner authorization.
- Keep Windows 11 24H2 ARM64 build 26100 as the starting guest. Preserve Windows security defaults in any new installer. Keep Windows media, activation material, provisioning profiles and pairing credentials out of Git and diagnostic exports.
- Preserve the existing ARM64 translation engine, 4 KiB guest MMU, display, input, audio, networking and OAuth unless a requested change requires touching them.
- Reuse the sparse disk format and preserve source archives, existing VM disks and writable per-VM NVRAM. Treat imported structures as untrusted input.
- Use [TESTING.md](TESTING.md) for verification and update [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) with the evidence. Record Xcode build, simulator and physical-device results separately.
- Keep the UI overhaul on `feat/henge-ui-overhaul`, based on the published DISM build `fefa1a6`. Treat older manifests as valid and keep guest framebuffer modes separate from host scaling. Preserve the existing boot and provisioning paths.
- Keep experimental graphics development on `experimental/metal-gpu`, based on the UI build `ba7fde7`. Keep RAMFB as the default and follow the hardware validation gates in [the MetalGPU roadmap](docs/METALGPU_ROADMAP.md).
