// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct VMFirmware: Sendable {
    let code: URL
    let variables: URL

    func validate() throws {
        try VMRepository.validateFirmware(code, isVariables: false)
        try VMRepository.validateFirmware(variables, isVariables: true)
    }
}

struct InstallerAssets: Sendable {
    static let networkDriverVersion = "virtio-0.1.285-w11-arm64"
    static let bundled = InstallerAssets(root: Bundle.main.bundleURL.appendingPathComponent("InstallerAssets", isDirectory: true))

    let root: URL

    var firmware: VMFirmware {
        VMFirmware(code: root.appendingPathComponent("edk2-aarch64-code.fd"),
                   variables: root.appendingPathComponent("edk2-arm-vars.fd"))
    }

    var drivers: URL { root.appendingPathComponent("NetKVM", isDirectory: true) }
}
