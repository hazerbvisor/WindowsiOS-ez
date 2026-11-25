# MetalGPU compatibility

| Layer | Verified here | Required next |
| --- | --- | --- |
| VirtIO-GPU 2D | Portable host compilation, deterministic queue/scanout tests and sanitizers | Xcode 27.2 ARM64 Release compilation and Windows PCI enumeration |
| viogpudo v0.6 | Upstream published digest matched the downloaded ZIP. The package contains an ARM64 SYS, INF and CAT. The display-only miniport declares WDDM 1.2. | Windows catalog trust, successful start and sustained display changes on iPad |
| viogpu3d v0.6 | Upstream published digest matched the downloaded ZIP. An ARM64 SYS, INF, CAT and Neptune DLLs are present. The source has a WDDM 2.2 initialization path and ARM64/ARM64X packaging. | Required device features, compatible user-mode handshake and separately measured native ARM64 and Prism behavior |
| Neptune and DXMT | Source audit, wire version 2, pinned native iPhoneOS build target in DXMT | Native renderer build, signing/load, WDDM adapter creation and a physical Metal command |
| Madeira | Source audit and upstream test reports examined | DX11 validation first, host ABI extraction, converter terms, versioned D3D12 capability coverage and guest validation |

The two archives were downloaded from [viogpu-wddm v0.6](https://github.com/utmapp/viogpu-wddm/releases/tag/v0.6). OpenSSL verified the catalogs' PKCS#7 signatures without a trust-chain check and reported Microsoft Windows Hardware Compatibility Publisher as the signer. Windows kernel policy acceptance remains a guest test. No Windows driver was installed or bundled here.

| Driver archive | Verified SHA-256 |
| --- | --- |
| `viogpudo-arm64-signed.zip` | `1b7fe46d26810eefee6ae95117d5971e9e442975839e044874a44db913fea08d` |
| `viogpu3d-arm64x-signed.zip` | `723131a1d0a8b7f01037b7efe3b6ac7b90f034734460885fba0edd7d8c75425f` |

Both INF families match `PCI\VEN_1AF4&DEV_1050`. The 3D miniport declines initialization without the renderer features listed in [the architecture audit](METALGPU_ARCHITECTURE.md). Use the display-only INF for the first device test. The presence of the ARM64EC and x86 Neptune UMDs establishes package contents. Prism app support, Windows LTSC coverage and OS signature trust remain guest tests.

## First device pass

1. Make a stopped backup or duplicate of a known bootable installed VM with its firmware, NVRAM, TPM state and disk. Use four vCPUs and the known-working Balanced combination for the initial graphics pass. Leave the installer disconnected.
2. Build and sign using the existing owner-started workflow or start a signed development build from Xcode. Record the commit, iPad model, OS, guest build and signed entitlement state. Do a RAMFB baseline boot first.
3. Relaunch from a development scheme with `WINIOS_VIRTIO_GPU=1` in the Run environment. Continue the existing authorized JIT workflow and wait for the self-test. Inspect Device Manager or `pnputil /enum-devices /connected /deviceids` for hardware ID `PCI\VEN_1AF4&DEV_1050`. Export Settings → Diagnostics and record the `graphics:` and `graphics_queues:` entries.
4. Verify the display-only archive's hash, extract it inside Windows and keep normal Windows signing and Secure Boot policies enabled. From an elevated prompt in the extracted folder, use `pnputil /add-driver ".\drivers\viogpudo\viogpudo.inf" /install`. Record Windows' package result, device problem code, driver version, and whether scanout, cursor, resolution changes, shutdown and restart work. Use Device Manager and `%windir%\INF\setupapi.dev.log` if Windows rejects the driver. On changed source, the reference WDK build is `build.bat viogpudo ARM64 Win11 Release` on Windows. A new kernel package requires a trusted signing workflow.
5. To recover, shut the VM down or stop from Session Details if Windows is inaccessible. Relaunch without the experimental environment variable. RAMFB can boot the same disk and NVRAM. Remove the display-only package from Windows if needed after recording its published driver name. Keep the original VM copy and installation media.

The running session's **Change Windows Resolution** action opens Windows Settings. Windows chooses from modes exposed by its active adapter. The pinned RAMFB/GOP firmware has three built-in modes (640×480, 800×600 and 1024×768). Which choices an installed Windows driver exposes remains a guest check. The host scaling picker affects presentation only.

For 5–8 vCPU tests, return to RAMFB first. Change one setting at a time and confirm Windows' logical processor count, processor activity under load, restart, suspend/resume and recovered disk access. Record the memory warning count and compare boot times with the same four-vCPU baseline. Existing VMs retain their saved count. More vCPUs do not guarantee more performance.
