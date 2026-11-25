# MetalGPU Windows ARM64 driver installer

Experimental installer for WindowsiOS-ez's MetalGPU laboratory build.

1. Back up your stopped VM, enable VirtIO-GPU in the lab IPA, and boot Windows. The currently released lab IPA supports only one vCPU.
2. In Windows, download **WindowsiOS-ez-MetalGPU-Driver.cmd** from this release. Right-click it and select **Run as administrator**.
3. Read the prompt and type INSTALL. The installer downloads the original signed UTM v0.6 ARM64X driver, checks its SHA-256 and size, and uses Windows PnPUtil. No driver-signing or Secure Boot changes are made.
4. Restart Windows, then check Device Manager and dxdiag. The installer does not establish that hardware acceleration works.

For offline installation, put the original [signed ZIP](https://github.com/utmapp/viogpu-wddm/releases/download/v0.6/viogpu3d-arm64x-signed.zip) beside the CMD file. The same hash check applies.

This release contains our installer script, not a modified or re-signed driver. The original driver is maintained by [UTM](https://github.com/utmapp/viogpu-wddm/releases/tag/v0.6). The CMD is not Authenticode-signed. Review its text before running. If Windows or organizational policy blocks it, use the official signed ZIP and normal Device Manager installation instead.

Pinned package: 2,784,710 bytes; SHA-256 `723131a1d0a8b7f01037b7efe3b6ac7b90f034734460885fba0edd7d8c75425f`.

Recovery: shut down the VM, disable VirtIO-GPU, and boot using RAMFB, or restore your backup. This is not a stable GPU release.
