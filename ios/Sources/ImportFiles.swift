// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum DocumentImportKind: String, Sendable {
    case iso, firmware, machine, disk, pairing, drivers
}

struct ImportEntry: Identifiable, Sendable {
    let url: URL
    let kind: DocumentImportKind
    var id: URL { url }
}

enum ImportInbox {
    static func scan(_ directory: URL) throws -> [ImportEntry] {
        let fm = FileManager.default
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let keys: Set<URLResourceKey> = [.isDirectoryKey, .isRegularFileKey, .isSymbolicLinkKey]
        let files = try fm.contentsOfDirectory(at: directory, includingPropertiesForKeys: Array(keys),
                                               options: [.skipsHiddenFiles])
        return files.compactMap { url in
            guard let values = try? url.resourceValues(forKeys: keys), values.isSymbolicLink != true else { return nil }
            let kind: DocumentImportKind?
            if values.isDirectory == true {
                let names = Set(((try? fm.contentsOfDirectory(atPath: url.path)) ?? []).map { $0.lowercased() })
                if names.isSuperset(of: ["netkvm.inf", "netkvm.cat", "netkvm.sys"]) {
                    kind = .drivers
                } else if names.contains(VMRepository.rawDisk) || names.contains(VMRepository.packedDisk) {
                    kind = .machine
                } else {
                    kind = nil
                }
            } else if values.isRegularFile == true {
                kind = switch url.pathExtension.lowercased() {
                case "iso": .iso
                case "fd": .firmware
                case "img", "raw", "hvsparse": .disk
                case "mobiledevicepairing", "pairing", "plist": .pairing
                default: nil
                }
            } else {
                kind = nil
            }
            return kind.map { ImportEntry(url: url, kind: $0) }
        }.sorted { $0.url.lastPathComponent.localizedStandardCompare($1.url.lastPathComponent) == .orderedAscending }
    }
}

struct ImportProgress: Sendable {
    let label: String
    let bytes: UInt64
    let total: UInt64
}

enum ImportCopy {
    static func file(_ source: URL, to destination: URL,
                     progress: (@Sendable (UInt64, UInt64) -> Void)? = nil) throws {
        let total = try regularFileSize(source)
        let input = try FileHandle(forReadingFrom: source)
        defer { try? input.close() }
        let fm = FileManager.default
        guard !fm.fileExists(atPath: destination.path),
              fm.createFile(atPath: destination.path, contents: nil) else {
            throw VMError.message("Could not create a new private copy of the import.")
        }
        var complete = false
        defer { if !complete { try? fm.removeItem(at: destination) } }
        let output = try FileHandle(forWritingTo: destination)
        defer { try? output.close() }
        var copied: UInt64 = 0
        var lastUpdate = Date.distantPast
        progress?(0, total)
        while let data = try input.read(upToCount: 1 << 20), !data.isEmpty {
            try Task.checkCancellation()
            guard UInt64(data.count) <= total - copied else {
                throw VMError.message("The source grew while it was being copied. Try again after it finishes saving.")
            }
            try output.write(contentsOf: data)
            copied += UInt64(data.count)
            let now = Date()
            if now.timeIntervalSince(lastUpdate) >= 0.2 {
                lastUpdate = now
                progress?(copied, total)
            }
        }
        guard copied == total else {
            throw VMError.message("The source was truncated while it was being copied.")
        }
        try output.synchronize()
        try output.close()
        complete = true
        progress?(copied, total)
    }
}
