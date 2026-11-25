# WinPVG Direct3D 11 translation prototype

Status: source implemented, not compiled or run on Apple hardware. This is a trusted-fixture shader laboratory, not an installed Windows driver or a full D3D11 implementation.

## Pinned dependency

Use https://github.com/utmapp/dxmt/tree/1fe2e752c98207518a99c9322482f63ebb9f0232, the same revision selected by the older MetalGPU architecture. Its `src/airconv/airconv_public.h` defines AIRCONV_VERSION 32 and the SM50Initialize/Compile/GetCompiledBitcode/Destroy APIs. SM50Compile serializes a metallib via MetallibWriter. The native umbrella's export list does NOT export these functions: link the airconv static target and its transitive dependencies instead. Do not attempt to resolve nonexistent dylib exports.

WinPVG uses a new adapter and no copied DXMT source. Build with upstream license notices, audit the static-link license obligations before distribution, and keep the dependency opt-in.

## What this test proves if it passes

```text
FXC SM5 HLSL -> actual DXBC bytes -> airconv SM50Initialize
  -> reflection restrictions -> SM50Compile -> metallib
  -> MTLDevice newLibraryWithData -> translated Metal shader functions
  -> render pipeline -> native draw -> GPU completion -> pixel readback
```

Both vertex and fragment shaders originate from HLSL. The test runs two different pixel shaders with the same translated vertex shader. It checks that the center changes from red to green while the blue corner stays unchanged. No fixed Metal shader substitute is used. It also tests empty bytecode and wrong shader stage rejection.

## Prepare fixtures

On Windows with the Windows SDK's FXC executable on PATH, from this repo root:

```bat
fxc /nologo /T vs_5_0 /E vs_main /Fo vertex.cso tests\shaders\wpvg_d3d11.hlsl
fxc /nologo /T ps_5_0 /E ps_red /Fo red.cso tests\shaders\wpvg_d3d11.hlsl
fxc /nologo /T ps_5_0 /E ps_green /Fo green.cso tests\shaders\wpvg_d3d11.hlsl
```

Do not use DXC's SM6 DXIL output. Transfer these three trusted files to the Apple host.

## Build recipe (candidate, not executed)

On a Metal-capable Apple Silicon Mac use Xcode, Meson 1.3+, and ARM64 LLVM 15.0.7 static libraries per pinned DXMT docs/DEVELOPMENT.md. Clone the pinned revision and initialize submodules. Use a disposable DXMT checkout for the following build-only additions; never modify the WinPVG main branch.

Copy devices/wpvg_shader.mm and tests/test_wpvg_dxbc.mm into the DXMT checkout root. Append the following to that checkout's root meson.build after subdir('src'):

```meson
if not dxmt_crossbuild and apple_subsystem == 'macos'
  add_languages('objcpp', required: true)
  wpvg_frameworks = dependency('appleframeworks', modules: ['Foundation', 'Metal'])
  executable('test_wpvg_dxbc',
    ['wpvg_shader.mm', 'test_wpvg_dxbc.mm'],
    dependencies: [airconv_dep_darwin, dxbc_parser_native_dep, wpvg_frameworks],
    objcpp_args: ['-std=c++20', '-fobjc-arc', '-fblocks'],
    link_language: 'cpp',
  )
endif
```

Configure the checkout with `meson setup build -Dnative_llvm_path=/absolute/path/to/llvm --buildtype release`, then `meson compile -C build test_wpvg_dxbc`. Run `./build/test_wpvg_dxbc /path/vertex.cso /path/red.cso /path/green.cso`. The build recipe requires validation against Xcode and the pinned dependency; missing symbols or headers are failures, not reasons to stub the translator.

Expected success line: WinPVG DXBC-to-Metal PASS on [device name]: red and green translated shaders.

## Before exposing translation to Windows

1. Verify P0 and the DXBC test on real Metal hardware. A macOS VM without a usable Metal device is not sufficient.
2. Build the airconv/LLVM closure for iphoneos ARM64 (not the macOS binary), audit platform-specific AIR generation, add a separate signed iPad app self-test, and record GPU output and process memory.
3. The current parser only validates the DXBC envelope. The third-party converter is not established as safe for untrusted guest bytecode; resource reflection checks are not sandboxing. Keep it inaccessible from guest commands until audited, fuzzed and appropriately isolated with resource/time limits. The current lab is trusted local fixtures only.
4. Implement versioned shader creation/bind/destroy commands, per-stage resource binding, pipeline state, caches and bounded execution. Keep the fixed triangle lab independent until this path is verified.
5. Implement the PCI transport and signed ARM64 WDDM KMD/UMD. The driver must translate D3D DDI commands, DXGI present, memory/scheduling and sync semantics; shader conversion is only one part of Direct3D.
6. Do not advertise hardware D3D feature levels until a native guest application creates a hardware-only D3D11 device and successfully renders. D3D12/DXIL is deferred.

No Neptune, VirGL, ANGLE or QEMU is required by this standalone test. No claim of Windows acceleration, iPad compatibility or performance is made.
