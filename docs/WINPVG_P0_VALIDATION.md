# WinPVG P0 status and validation

The P0 prototype now contains a portable packet validator and mock ledger, a real Metal lab backend, a standalone Metal smoke test, and a packet-to-Metal integration test. It is not linked into the app, not a PCI device, and not a Windows GPU driver.

## Run the tests

From the repository root on a host with clang:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/test_wpvg_lab.c -o /tmp/test_wpvg_lab
/tmp/test_wpvg_lab
```

On an Apple Silicon Mac with Xcode command-line tools and a Metal device:

```sh
xcrun clang -fobjc-arc -fblocks -Wall -Wextra \
  -framework Foundation -framework Metal \
  tests/test_wpvg_metal.m -o /tmp/test_wpvg_metal
/tmp/test_wpvg_metal

xcrun clang -fobjc-arc -fblocks -Wall -Wextra \
  -framework Foundation -framework Metal \
  devices/wpvg_metal.m tests/test_wpvg_integration.m \
  -o /tmp/test_wpvg_integration
/tmp/test_wpvg_integration
```

Expected output includes the Metal device name and a PASS line for each successful test. Actual execution and output must be recorded; source alone is not evidence of passing. No Apple hardware execution or Xcode compilation was performed in this change.

For physical iPad verification, create an opt-in test entry point in the app (not the production VM startup path), run off the UI thread and export the exact command statuses and pixels. Do not try to execute the macOS command-line binaries on iPadOS.

## Implementation boundaries

- Single in-flight packet, one private serial worker, real GPU completion before reporting success. Busy requests are rejected rather than queued without a limit. A callback may submit new work but must not synchronously wait on this worker.
- Fixed trusted shaders only, private RGBA8 texture, shared Metal buffer. No guest-supplied shader compiler, scanout, display presentation or PCI integration.
- Mock ledger permits a 256 MiB logical budget; the Metal lab applies an additional 32 MiB per-object cap, reserves 1 MiB headroom and accounts for reported Metal allocations. Actual peak process memory and unreported driver allocations are not bounded by these counters.
- Reset is rejected while GPU work or readback is active. A GPU execution failure poisons the instance until idle reset. Consumers must match generation and request; callbacks may arrive after an idle reset.
- Worker waits for GPU completion; a wedged GPU could wedge the lab worker. A production device requires nonblocking GPU completion, watchdog/reset policy, resource lifetime and safe retirement. No lock held across GPU wait.
- No generic capabilities negotiation or PRESENT command yet. The current lab supports only the eight opcodes in wpvg_lab.h.

## Next gate

First compile and execute these tests on Apple Silicon. Then add an isolated iPad app self-test and validate on M4. After P0 evidence is recorded, design the experimental PCI BAR and guest RAM ring path against the existing PCI/MMU interfaces. Keep RAMFB unchanged and retain the current VM disk and all MetalGPU branches.
