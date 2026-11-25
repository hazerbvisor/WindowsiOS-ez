# MetalGPU architecture

The base is `ba7fde7`, the built Henge-inspired UI revision. The ARM64 JIT engine continues to run Windows and dispatch device MMIO. The target remains Windows 11 24H2 ARM64 build 26100 on an iPad Air M4. The unsigned ARM64 iPhoneOS Release build still uses the existing Xcode 27.2 workflow.

## Pinned reference sources

| Component | Audited revision | Implementation reference |
| --- | --- | --- |
| [UTM](https://github.com/utmapp/UTM/tree/3d4b8d8cb1ce93a8b1a9610695c12487685f0c15) | `3d4b8d8cb1ce93a8b1a9610695c12487685f0c15` | `UTM/Configuration/UTMQemuConfiguration+Arguments.swift`, `scripts/build_dependencies.sh`, `patches/sources` |
| [UTM QEMU fork](https://github.com/utmapp/qemu/tree/6601422e1fff2da1376faafb1e4c2c5cdb2d8003) | `6601422e1fff2da1376faafb1e4c2c5cdb2d8003` (`v10.0.12-utm`) | `hw/display/virtio-gpu-pci.c`, `virtio-gpu.c`, `virtio-gpu-virgl.c` |
| [viogpu-wddm](https://github.com/utmapp/viogpu-wddm/tree/d25bb08c4a9cfb034f4db9ea9e2982a8be02e86b) | `d25bb08c4a9cfb034f4db9ea9e2982a8be02e86b` (`v0.6`) | `viogpudo/viogpudo.cpp`, `viogpu3d/viogpu_adapter.cpp`, `viogpu3d/viogpu_vidpn.cpp`, `shared/viogpum.h` and driver INFs |
| [Neptune](https://github.com/osy/neptune-protocol/tree/b0152e59f068f04138520fe471a96263c8888860) | `b0152e59f068f04138520fe471a96263c8888860` | `docs/neptune_command_serialization.txt`, `npt_registry*.json`, `transport/npt_protocol_transport.h` |
| [UTM virglrenderer fork](https://github.com/utmapp/virglrenderer/tree/dd0652b13e589c4c124ea79123a3adcd85173f4f) | `dd0652b13e589c4c124ea79123a3adcd85173f4f`, pinned by UTM | `meson.build`, `src/neptune/npt_{context,cs,renderer,ring,library}.c` and generated protocol headers |
| [UTM DXMT fork](https://github.com/utmapp/dxmt/tree/1fe2e752c98207518a99c9322482f63ebb9f0232) | `1fe2e752c98207518a99c9322482f63ebb9f0232`, pinned by UTM | `meson.build`, `src/dxmt-native/meson.build`, `src/{d3d11,d3d12,nativemetal}` |
| [Madeira](https://github.com/willfaust/Madeira/tree/fbad430410b4075192166172447c859714726de9) | `fbad430410b4075192166172447c859714726de9` | `madeira-d3d12/README.md`, `src/pe/madeira_d3d12.c`, `src/unix/madeira_ir_unix.mm`, `app/Madeira/WineServerBridge.m` |
| [Windows Driver Samples](https://github.com/microsoft/Windows-driver-samples/tree/d49bb7d72f3651952b2b6e4ad28d14fe2247eb69) | `d49bb7d72f3651952b2b6e4ad28d14fe2247eb69` | `video/KMDOD` display-only miniport and WDK solution |

The checked-in 2D implementation follows [VirtIO 1.2, section 5.7](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html). The pins identify source references. There are no new renderer dependencies in the IPA build.

## Implemented device path

```text
Windows display-only miniport
    → virtio-gpu PCI 1af4:1050
    → WindowsiOS split virtqueues
    → validated 2D resources in host memory
    → committed BGRA scanout
    → existing Metal display presenter
```

The device uses the existing WindowsiOS ECAM and PCI memory windows. It adds a modern PCI function at the next free slot, leaving the existing NetKVM, NVMe, USB and audio slot assignment intact. It uses subsystem IDs `1af4:1100`, BAR 4 for the modern common/notify/ISR/device capabilities, and BAR 2 for three MSI-X entries and their pending bits. Queues 0 and 1 carry control and cursor requests. MSI-X routes through the existing GICv3 ITS. INTx remains available. No IOMMU, indirect descriptors or packed rings are advertised.

The device announces `VIRTIO_F_VERSION_1` and optional EDID. It serves one scanout with a 1024×768 preferred mode and zero accelerated capsets. Implemented operations allocate/free resources, attach/detach guest backing, transfer bounded rectangles, set a scanout, flush damage and update the cursor. The device echoes fences after 2D processing and used-ring publication can follow immediately. A fence here reports command completion. It does not report that an iPad display frame has been presented.

Queue processing, resource changes and reset use the existing device lock. Access is restricted to writable guest RAM, bounds and alignment are checked before processing a ring, descriptors and scatter lists are bounded, and host completion indices are kept separately from guest writes. Invalid ring addresses and indices request a device reset. Responses and used-ring publication use the existing DMA code invalidation hook. The device accepts up to 64 resources, up to 8192 backing entries per resource, and up to 32 MiB per 2D surface. The 256 MiB CPU-side graphics budget allows up to 160 MiB of resource pixels and backing metadata and leaves room for up to three 32 MiB scanout snapshots during publication. Resources allocate on demand. These figures exclude the Metal presentation texture, baseline ramfb, guest RAM and JIT. CPU0 drains pending batches. Current scanout copies run under the device lock and have no GPU worker or zero-copy path.

Scanout publication is atomic with respect to the presenter reading the CPU buffer. Unsetting the scanout, freeing the displayed resource or resetting the device exposes RAMFB again. A process started without the developer opt-in attaches no VirtIO-GPU function. Installation continues to use RAMFB even with the opt-in present.

## Direct3D 11 dependency map

```text
Windows ARM64 Direct3D 11 → viogpu3d.sys + Neptune user-mode DDI
  → VirtIO-GPU context and resource blobs → Neptune capset 7
  → selected UTM virglrenderer Neptune modules
  → ARM64 libdxmt-native and DXGI → iPhoneOS Metal
  → local Metal scanout
```

UTM supplies `hostmem=8G,blob=true,neptune=true` for this path. The hostmem value describes a guest-visible PCI address aperture. The real renderer needs separate committed-memory accounting. QEMU's GPU PCI function places the shared-memory capability in BAR 4 and relocates modern/MMIO and MSI-X BARs. Its virgl integration maps renderer-created blob storage into a QEMU `MemoryRegion`, submits context commands and holds fenced requests until callbacks identify a completed context ring. WindowsiOS's current PCI dispatch exposes an access callback per BAR. It cannot directly install and revoke RAM mappings for individual blob lifetimes. This is the first compatibility gap after the 2D driver test.

The Windows 3D driver requires `VIRTIO_GPU_F_VIRGL`, `VIRTIO_GPU_F_RESOURCE_BLOB` and `VIRTIO_GPU_F_CONTEXT_INIT`. The protocol bit named VIRGL is required for the 3D transport. It does not select a Vulkan graphics backend. The Neptune UMD uses capset 7, submission, guest-visible resource blobs and feedback rings. The pinned renderer and Neptune registry both declare wire version 2. The little-endian 24-byte packet header carries the command type, flags, object ID and total byte length. The renderer dispatches generated DXGI/D3D COM calls and handcrafted transport commands for replies, shared rings, event delivery and cross-ring ordering. Compatibility also depends on the capset payload and the Windows UMD's generated protocol tables through the actual handshake.

The UTM scripts use a thread renderer off macOS and already cross-build DXMT's native Metal target for iPhoneOS. DXMT uses an iPhoneOS Metal/AIR target and the UTM build pins LLVM 15.0.7. The virglrenderer Meson file has separate Neptune and Venus options. The candidate WindowsiOS build should use Neptune on-device, disable Venus and use the DXMT device/DXGI exports and its native embedder event/FD interface. It still needs an Xcode Release link and launch test to prove the usable dependency closure. The present implementation does not include that renderer, a hostmem aperture, graphics contexts or asynchronous GPU completion.

The next transport design should use a bounded worker and ordered completion back into the emulated device, with explicit generation ownership for context reset. Map shared blobs only when guest/host page alignment, CPU/GPU visibility and unmap lifetime can all be guaranteed. Other resources can use bounded copies. Pass a retained compatible texture to the existing display layer once this renderer can safely export one. RAMFB remains available for boot and recovery.

## Direct3D 12 boundary

Neptune already has generated D3D12 commands and renderer overrides for devices, pipelines, resources and fences. DXMT has an optional D3D12 build path, outside this project's backend choice. The selected D3D12 backend is Madeira. The pinned Madeira runtime uses ARM64EC PE frontends, a native converter bridge and a Wine/FEX-oriented integration. Reuse requires a native iPadOS backend with a compatible D3D12/COM ABI, a verified shader dependency and a genuine Windows WDDM UMD path. Its existing A15 test reports concern Madeira's own environment. Any extension should follow a wire compatibility matrix and use a version and capability only for semantics that need new bytes or behavior. See [the roadmap](METALGPU_ROADMAP.md).
