// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum NetKVMDrivers {
    /// Copy a user-supplied ARM64 package. Windows Setup verifies the catalog and its trust chain.
    static func stage(from source: URL, to destination: URL) throws {
        let fm = FileManager.default
        let required = Set(["netkvm.inf", "netkvm.cat", "netkvm.sys"])
        let allowed = required.union(["netkvmco.dll"])
        var selected: [String: URL] = [:]
        for item in try fm.contentsOfDirectory(at: source, includingPropertiesForKeys: nil) {
            let name = item.lastPathComponent.lowercased()
            if allowed.contains(name) {
                guard selected[name] == nil else { throw VMError.message("The driver folder contains duplicate names.") }
                _ = try regularFileSize(item)
                selected[name] = item
            }
        }
        guard required.isSubset(of: selected.keys), let inf = selected["netkvm.inf"],
              let catalog = selected["netkvm.cat"], let driver = selected["netkvm.sys"] else {
            throw VMError.message("Choose a NetKVM folder containing netkvm.inf, netkvm.cat, and netkvm.sys.")
        }
        guard try regularFileSize(inf) <= 1 << 20 else {
            throw VMError.message("The NetKVM INF is too large.")
        }
        let contents = try Data(contentsOf: inf)
        let text = String(data: contents, encoding: .utf8) ?? String(data: contents, encoding: .utf16)
        guard let text, text.localizedCaseInsensitiveContains("CatalogFile"),
              text.localizedCaseInsensitiveContains("netkvm.cat"),
              text.localizedCaseInsensitiveContains("NTarm64") else {
            throw VMError.message("The NetKVM INF does not declare an ARM64 package and catalog.")
        }
        let cat = try FileHandle(forReadingFrom: catalog)
        defer { try? cat.close() }
        guard try cat.read(upToCount: 1)?.first == 0x30 else {
            throw VMError.message("The NetKVM catalog does not have the expected format.")
        }
        try requireARM64PE(driver)
        if let coInstaller = selected["netkvmco.dll"] { try requireARM64PE(coInstaller) }

        try fm.createDirectory(at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
        let staging = destination.deletingLastPathComponent().appendingPathComponent(".staging-\(UUID().uuidString)")
        try fm.createDirectory(at: staging, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: staging) }
        for (name, url) in selected {
            try fm.copyItem(at: url, to: staging.appendingPathComponent(name))
        }
        try fm.moveItem(at: staging, to: destination)
    }

    private static func requireARM64PE(_ url: URL) throws {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        guard let header = try file.read(upToCount: 64), header.count == 64,
              header.prefix(2) == Data([0x4d, 0x5a]) else {
            throw VMError.message("\(url.lastPathComponent) is not a Windows PE binary.")
        }
        let offset = header.littleEndian(at: 60, bytes: 4)
        guard offset >= 64 else { throw VMError.message("The NetKVM PE header is invalid.") }
        try file.seek(toOffset: offset)
        guard try file.read(upToCount: 6) == Data([0x50, 0x45, 0, 0, 0x64, 0xaa]) else {
            throw VMError.message("\(url.lastPathComponent) does not target ARM64.")
        }
    }
}
