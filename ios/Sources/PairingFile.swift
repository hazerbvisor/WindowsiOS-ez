// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum PairingFile {
    static func validate(_ source: URL) throws {
        let size = try regularFileSize(source)
        guard size > 0, size <= 1 << 20 else { throw invalid }
        let data = try Data(contentsOf: source)
        guard let values = (try? PropertyListSerialization.propertyList(from: data, options: [], format: nil))
                as? [String: Any] else { throw invalid }

        let remote = (values["public_key"] as? Data)?.count == 32 &&
            (values["private_key"] as? Data)?.count == 32 &&
            (values["identifier"] as? String)?.isEmpty == false
        let usb = (values["HostID"] as? String)?.isEmpty == false &&
            (values["SystemBUID"] as? String)?.isEmpty == false &&
            (values["HostCertificate"] as? Data)?.isEmpty == false &&
            (values["HostPrivateKey"] as? Data)?.isEmpty == false
        guard remote || usb else { throw invalid }
    }

    private static var invalid: VMError {
        .message("Choose a valid remote or USB pairing property list from your authorized debugger setup.")
    }
}
