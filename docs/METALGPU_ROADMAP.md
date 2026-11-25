# MetalGPU roadmap

| Gate | Status | Exit evidence |
| --- | --- | --- |
| Investigation | Source audit complete for the selected pins | Dependency map, driver archive provenance and license review |
| A. Virtual device | Off-by-default implementation. Portable tests pass. | Windows 11 build 26100 enumerates the PCI function on the iPad, can reboot with it present, and can boot the same disk with RAMFB |
| B. Driver | ARM64 package contents and hashes checked. Guest test pending. | Display-only miniport loads under normal signing. Then validate the 3D miniport against a compatible implementation of contexts and blobs |
| C. DX11 | Pending Gate A/B evidence and renderer integration | Native ARM64 test requests hardware D3D11 and refuses WARP, records adapter LUID and feature level, renders a triangle, and correlates the guest fence with Metal execution |
| D. DX11 stability | Pending C | Validate textures, compute, source and destination copies, resize, command ordering and continuous present under sustained load and shutdown/reset |
| E. Extended Neptune | Pending D | Compare wire v2 against required Madeira calls. Preserve negotiated v2 behavior. Test versioned extensions, recorded vectors, rejecting malformed packets and cross-ring ordering |
| F. DX12 | Pending E | Run Windows ARM64 device → buffer copy → VS/PS triangle → textured cube → compute → barriers/fences → continuous present and reset. Confirm native Madeira on iPad |
| G. App coverage | Pending hardware backends | Start with Microsoft D3D samples and the desktop. Progress to Edge, Blender and compatible benchmarks before Roblox Studio or games. Record API/feature level and driver status for each app |
| H. Optimization | Pending baseline performance data | Reproduce transfer and shader cost, then compare shared buffers, texture presentation, batching, pooling and caches without compromising lifetime and ordering |

Implement the next DX11 transport changes in reviewable steps. Start with a safe shared-memory PCI aperture and explicit window bounds, then context/blob ownership and async fences, then the pinned in-process Neptune + DXMT build. Require teardown and RAMFB recovery for every step. Validate heap usage from live allocated resources. Expose configurable GPU budgets only when the renderer enforces them across resources, shared blobs and transient GPU objects. Expose only verified capabilities in guest configuration and in the settings UI.

## Benchmark protocol

Prepare one signed app revision and one Windows build and driver revision per run, using an unchanged test VM and the same RAM, JIT, vCPU count and display mode. Test at a fixed resolution and then repeat at a higher common resolution. Record refresh cap, power state, iPadOS version and thermal state. Compare an explicitly requested WARP run against a hardware-only D3D11 run, then add an equivalent D3D12 run when Gate F passes. Use matching workloads for triangle, textured scene, buffer copies and compute. Use an identical warm-up and capture window for each pair and include a separate cold-pipeline run.

Capture individual guest frame times and present times. Report mean FPS, 1% lows computed from the slowest 1% of frame times, CPU and GPU frame times, command/transfer bytes and latency, host footprint, graphics allocations, cold shader compilation latency and thermal state. Keep guest present counts separate from iPad presentation refresh. Record errors and resets and include a sustained stability pass. Preserve raw aggregate/timing data and test program revisions without collecting guest buffer contents. No benchmark values have been measured for this branch.
