# Physical iPad shader lab

Status: build recipes and standalone app source committed. No Xcode build, unsigned IPA, signing, or physical-device PASS has been produced by this change.

This is a separate test application (io.github.hazerbvisor.winpvg.lab), not WindowsiOS. It uses normal app signing and no JIT, hypervisor, kernel or private GPU entitlements. It only accepts the three trusted shader fixtures packaged with the app. The test checks malformed/empty input, incorrect stage, translation of vertex and two pixel shaders, Metal pipeline creation, GPU completion and exact color readback. No live Windows VM or guest-supplied shaders.

## Build in order

1. In Windows with FXC from the Windows SDK:
   `powershell -File scripts/build_winpvg_fixtures.ps1 -OutputDirectory C:\wpvg-fixtures`
   Transfer the three CSO files to the build Mac. Record their SHA-256 hashes.
2. On an ARM64 Mac with Xcode Metal toolchain, Meson, Ninja, CMake, Python 3 and git:
   `bash scripts/build_winpvg_ios_llvm.sh`
   This is the candidate LLVM 15.0.7 iPhoneOS build, using a native macOS TableGen executable.
3. On that Mac, from this repository root:
   `WPVG_IOS_LLVM_PREFIX="$PWD/work/winpvg-llvm/install-iphoneos-arm64" WPVG_FIXTURES=/absolute/path/to/fixtures bash scripts/build_winpvg_ios_lab.sh`

An existing verified LLVM 15.0.7 ARM64 iPhoneOS static dependency prefix may be used instead of step 2. The build rejects a macOS LLVM archive even when it is ARM64. The pinned DXMT source has target-specific AIR triples for iOS (Metal 3.2 uses iOS 18.0). The build uses iPhoneOS, not simulator, with a minimum version of 18.0. The script checks the linked app platform and dynamic dependencies and emits work/winpvg-ios/WinPVGLab-unsigned.ipa if successful. Both recipes are untested; diagnose actual compiler/linker errors rather than replacing missing functions with stubs.

## Test on the iPad

Sign the separate IPA normally using a valid profile and install it. Open WinPVG Lab and tap Run DXBC → Metal test. Export winpvg-dxbc-result.txt from the app's Documents folder using Files. A successful report names the Metal device and confirms the two distinct expected shader colors and blue background; failures include the translation, pipeline or GPU error when available. Record the app revision, OS, fixture hashes and elapsed time. A hung GPU/compiler can leave the lab busy; do not infer success from an app launch.

Do not move to guest-controlled shader execution or advertise a GPU until actual device validation. Next implementation task after verified results: connect trusted translated pipeline handles into the WinPVG worker, then PCI/KMDF and WDDM. Keep RAMFB unchanged.

Before distributing a built IPA, complete the DXMT, LLVM and all transitive license/notice obligations. The test app is not a driver and cannot accelerate Windows on its own.
