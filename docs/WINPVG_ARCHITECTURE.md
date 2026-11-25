# WinPVG — experimental Windows paravirtual GPU

Status: design and implementation gates only. No driver, GPU device, hardware acceleration or IPA is provided by this document.

## Goal

Apply the architectural principle of Henge's macOS PVG to Windows 11 24H2 ARM64 (build 26100) on iPad Air M4: a lightweight guest-to-host GPU protocol whose host implementation executes Metal commands. Do not emulate an Apple GPU, expose physical GPU registers, assume Apple guest drivers work on Windows, or replace the working ARM64 JIT/MMU. Keep RAMFB as the default and recovery path. Existing MetalGPU branches are retained.

## Architecture

```text
Windows test client / later D3D11 runtime + WDDM UMD
     -> ARM64 guest KMDF transport / later WDDM KMD
     -> virtual PCIe device + command/completion rings
     -> validated host command dispatcher + resource registry
     -> serial Metal command worker + asynchronous completion
     -> retained MTLTexture -> existing display presenter
```

The initial client bypasses Direct3D and WDDM entirely. A trusted ARM64 guest transport driver is still needed for PCI MMIO and DMA; a normal user process cannot access them directly. The test client must not be represented as a Windows GPU driver. Full WDDM requires a display/render miniport, a user-mode DDI implementation, DXGI integration, memory management, scheduling, synchronization, and shader translation. Direct3D bytecode is not Metal shader code. Evaluate DXMT's shader compiler and UMD interfaces separately; do not assume its Wine-oriented runtime is a drop-in WDDM UMD. Defer D3D12 and Madeira.

## Milestones (strict order)

| Gate | Implementation | Required evidence |
| --- | --- | --- |
| P0 | Host-only mock GPU, protocol codec, resource table, renderer worker | Portable parser tests, malformed input rejection, host Metal triangle readback |
| P1 | Optional PCI function, reset, BARs, IRQ, guest RAM-backed command/completion rings | Windows PCI enumeration, no boot regression, RAMFB fallback, reset tests |
| P2 | Minimal ARM64 KMDF transport and native test client | Normal trusted driver installation; ping, buffer upload, exact fence completion, driver unload |
| P3 | Native Metal buffer/texture creation, clear, known triangle, present | Physical iPad captures, Metal completion and pixel readback agree; no WARP or CPU rasterizer |
| P4 | Multiple vCPUs, resource lifetime, display mode and memory pressure | Sustained rendering, resize, repeated shutdown/reboot, no hangs, bounded memory |
| P5 | WDDM display-only miniport | Signed ARM64 KMDOD-derived driver, stable scanout, safe fallback; no 3D claim |
| P6 | D3D11 WDDM rendering path | Hardware-only ARM64 D3D11 device creation, real translated shaders, triangle, textures, compute, fences |
| P7 | Desktop and application validation | DWM, Edge and compatible workloads with adapter/feature-level evidence; Prism separately |

Do not advertise a WDDM feature level or ship an enabled-by-default GPU until tested. A Metal-backed framebuffer by itself is not guest GPU acceleration.

## Protocol v0 design

- New opt-in device; do not reuse VirtIO GPU PCI identifiers or impersonate Apple/other vendor IDs. Select and document a lawful experimental PCI ID strategy.
- Fixed little-endian wire format, versioned capabilities, fixed-width integers, no host pointers. Separate commands and completions. Include command length, resource handle, request ID and reset generation. Use monotonically increasing queue counters and bounded queue sizes.
- Start with PING, GET_CAPS, CREATE_BUFFER, DESTROY_RESOURCE, UPLOAD, CREATE_TEXTURE, CLEAR, DRAW_TEST_TRIANGLE, PRESENT and FENCE. The test triangle uses a fixed trusted shader and is not a generic shader API.
- Guest ring memory must be validated against ordinary writable guest RAM. Copy descriptors before decoding, reject overflow, out-of-range DMA, invalid format, stale handle, invalid state transitions and over-budget allocations. Do not block under the emulated device lock.
- Guest page size 4 KiB; host page size 16 KiB. Start with bounded copies into Metal resources. Add shared-memory/zero-copy only after proving normal memory mappings, coherency, atomic semantics, alignment, TLB invalidation and unmap lifetime on all vCPUs.
- Retain resources through asynchronous Metal command-buffer completion; distinguish submission, GPU completion, and display presentation. Use per-generation fences and discard stale completions on reset. Device teardown drains or invalidates work without blocking the vCPU or UI thread.
- Use a hard aggregate graphics budget including CPU staging, texture/buffer allocations, retained in-flight resources and scanout. Initial lab budget 256 MiB, subject to measurement; not a separate physical VRAM reservation. Keep guest RAM/JIT baseline unchanged.

## Proposed files

- `devices/wpvg_protocol.h`: versioned ABI and command definitions.
- `devices/wpvg_device.c`: PCI/MMIO, ring validation, IRQ and reset.
- `devices/wpvg_memory.c`: resource registry, lifetime and budget.
- `devices/wpvg_metal.m`: native Metal execution and completion.
- `devices/wpvg_shader.mm`: later shader translation.
- `drivers/winpvg/`: ARM64 transport, test client, later KMD and UMD.
- `tests/`: portable protocol and lifecycle tests, host Metal tests.

## References and licensing

- Henge: https://github.com/Leviidev/Henge (PVG/Metal architectural reference; inspect actual source and pin a revision before copying).
- Microsoft Windows driver samples: https://github.com/microsoft/Windows-driver-samples (KMDF and KMDOD examples).
- UTM viogpu-wddm: https://github.com/utmapp/viogpu-wddm (WDDM implementation reference).
- UTM DXMT: https://github.com/utmapp/dxmt (shader and rendering translation candidate).

Record exact upstream commits and license obligations before importing code. Do not copy macOS guest binaries or use untrusted driver signing bypasses. If trusted ARM64 driver signing is unavailable, stop at host-only tests and report the blocker.

## Test discipline

Build from an identified source revision. Use a stopped duplicate of the known-good VM and keep the original disk untouched. Record baseline RAMFB boot, device enumeration, driver problem code, guest request/completion counters, Metal submission/completion timestamps, texture publication, host memory pressure and errors. Keep experiment off during installation. No FPS or near-native performance claims before physical-device measurement.

First implementation task: P0 only. Review existing PCI and framebuffer interfaces before P1. Do not modify main or existing MetalGPU branches.
