// SPDX-License-Identifier: GPL-2.0-or-later
import CryptoKit
import Foundation
import SwiftUI
import UIKit

/// Only the display-only package is offered until the host implements 3D capsets.
enum MetalGPUDriverPackage {
    static let filename = "viogpudo-arm64-signed.zip"
    static let bytes = 267581
    static let sha256 = "1b7fe46d26810eefee6ae95117d5971e9e442975839e044874a44db913fea08d"
    static let source = URL(string: "https://github.com/utmapp/viogpu-wddm/releases/download/v0.6/" + filename)!
    static var sharedURL: URL {
        URL.documentsDirectory.appendingPathComponent("Shared", isDirectory: true).appendingPathComponent(filename)
    }

    static func download() async throws {
        let (temp, response) = try await URLSession.shared.download(from: source)
        defer { try? FileManager.default.removeItem(at: temp) }
        guard let http = response as? HTTPURLResponse, http.statusCode == 200,
              try regularFileSize(temp) == UInt64(bytes) else {
            throw VMError.message("The driver download is incomplete or has an unexpected size.")
        }
        let data = try Data(contentsOf: temp)
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        guard digest == sha256 else { throw VMError.message("The driver package checksum did not match. Nothing was installed.") }
        let target = sharedURL
        try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
        // Never replace a different user file in the shared directory.
        if FileManager.default.fileExists(atPath: target.path) {
            guard try regularFileSize(target) == UInt64(bytes), try Data(contentsOf: target) == data else {
                throw VMError.message("A different file already exists in Shared. Move it aside and retry.")
            }
            return
        }
        try data.write(to: target, options: .withoutOverwriting)
    }

    // Explicitly run by the owner in elevated Windows PowerShell. Windows retains catalog trust checks.
    static let installCommand = #"""
    $ErrorActionPreference='Stop'; $d=Join-Path $env:TEMP ('MetalGPU-'+[guid]::NewGuid()); New-Item -ItemType Directory $d | Out-Null; $z=Join-Path $d 'driver.zip'; Invoke-WebRequest -UseBasicParsing 'http://10.0.2.4/viogpudo-arm64-signed.zip' -OutFile $z; if ((Get-FileHash $z -Algorithm SHA256).Hash -ne '1b7fe46d26810eefee6ae95117d5971e9e442975839e044874a44db913fea08d') { throw 'Driver checksum mismatch' }; Expand-Archive -LiteralPath $z -DestinationPath $d; $inf=Get-ChildItem $d -Recurse -Filter viogpudo.inf; if (@($inf).Count -ne 1) { throw 'Expected one display driver INF' }; pnputil /add-driver $inf.FullName /install; if ($LASTEXITCODE -notin 0,3010) { throw "PnPUtil failed: $LASTEXITCODE" }
    """#
}

struct MetalGPUDriverView: View {
    @State private var busy = false
    @State private var ready = false
    @State private var message: String?
    var body: some View {
        Form {
            Section("Signed display driver") {
                LabeledContent("Package", value: "viogpudo v0.6 · ARM64")
                LabeledContent("Acceleration", value: "Display only")
                Button(ready ? "Verify package again" : "Download to Shared", systemImage: "arrow.down.circle") {
                    busy = true
                    Task {
                        defer { busy = false }
                        do {
                            try await MetalGPUDriverPackage.download()
                            ready = true
                            message = "Checksum verified. Package available in Shared."
                        } catch {
                            ready = false
                            message = error.localizedDescription
                        }
                    }
                }.disabled(busy)
                if busy { ProgressView() }
                if let message { Text(message).font(.footnote) }
                Link("Upstream signed release", destination: MetalGPUDriverPackage.source)
            }
            Section("Install inside Windows") {
                Text("First back up the stopped VM, enable VirtIO-GPU, save, and boot Windows. Confirm PCI\\VEN_1AF4&DEV_1050 in Device Manager. Download the package above, then open Windows PowerShell as administrator. Copy the command below and use the session Clipboard → Type into Windows action. Review it before pressing Enter. It verifies SHA-256 again and uses the normal Windows driver installer.")
                Button("Copy PowerShell install command", systemImage: "doc.on.doc") {
                    UIPasteboard.general.string = MetalGPUDriverPackage.installCommand
                }
                Text("If the shared WebDAV endpoint is unavailable, download the signed ZIP from the upstream release in Windows, verify its SHA-256, extract it and install viogpudo.inf with Device Manager. Restart Windows if requested and confirm driver status. Secure Boot and signing checks stay enabled.")
                Text("DXMT and the 3D driver are not available yet. Do not install viogpu3d until the accelerated transport is implemented.")
                    .foregroundStyle(.secondary)
            }
            Section("Recovery") {
                Text("Shut down Windows, close and reopen WindowsiOS-ez, disable VirtIO-GPU in the machine settings and save. RAMFB boots the same disk. If necessary restore the stopped backup.")
            }
        }
        .navigationTitle("Graphics driver")
    }
}
