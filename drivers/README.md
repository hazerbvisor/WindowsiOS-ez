# MetalGPU driver installer R2

Removes PowerShell, WMI/CIM and Get-PnpDevice checks that may hang before showing output in the VM. Prints status immediately.

Download WindowsiOS-ez-MetalGPU-Driver.cmd, right-click Run as administrator, and press Y after backing up the VM. Downloads the unchanged official signed UTM v0.6 ARM64X package, verifies SHA-256, extracts using Windows tar, and invokes PnPUtil. Windows retains catalog and driver signing checks. No Secure Boot changes.

For offline installation place the original [signed ZIP](https://github.com/utmapp/viogpu-wddm/releases/download/v0.6/viogpu3d-arm64x-signed.zip) beside the CMD.

The installer is not Authenticode-signed. Targets Windows ARM64 with experimental VirtIO-GPU enabled; automatic architecture/device checks were removed to avoid stalls. Windows decides driver compatibility. Restart and check Device Manager afterward. Actual GPU acceleration and physical VM execution of R2 are unverified.

Upstream: https://github.com/utmapp/viogpu-wddm/releases/tag/v0.6
