// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct WindowsISOInspection: Sendable {
    let images: [WindowsImage]
    let computedSHA256: String
    let filesystem: String
    let diagnostics: String
}

struct WindowsMedia: Codable, Equatable, Identifiable {
    let id: UUID
    let name: String
    let bytes: UInt64
    let images: [WindowsImage]
    let sha256: String?
    let computedSHA256: String?
    let filesystem: String?
    let diagnostics: String?

    var savedChecksum: String? { sha256 ?? computedSHA256 }
}

struct WindowsMediaRepository: Sendable {
    let root: URL

    func isoURL(for id: UUID) -> URL {
        root.appendingPathComponent(id.uuidString).appendingPathComponent("Windows.iso")
    }

    func list() throws -> [WindowsMedia] {
        let fm = FileManager.default
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        return try fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])
            .compactMap { directory in
                guard let id = UUID(uuidString: directory.lastPathComponent) else { return nil }
                let metadata = directory.appendingPathComponent("Media.plist")
                guard try regularFileSize(metadata) <= 64 << 10 else {
                    throw VMError.message("The media metadata is too large.")
                }
                let item = try PropertyListDecoder().decode(WindowsMedia.self, from: Data(contentsOf: metadata))
                guard item.id == id, try regularFileSize(directory.appendingPathComponent("Windows.iso")) == item.bytes else {
                    throw VMError.message("A stored Windows ISO is missing or incomplete.")
                }
                return item
            }.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    func remove(_ item: WindowsMedia) throws {
        guard try list().contains(where: { $0.id == item.id }) else {
            throw VMError.message("The stored ISO could not be found.")
        }
        try FileManager.default.removeItem(at: root.appendingPathComponent(item.id.uuidString))
    }

    func store(source: URL, name: String, linking: Bool, id: UUID = UUID(),
               checksum: String?, copyProgress: (@Sendable (UInt64, UInt64) -> Void)? = nil,
               inspect: (URL) throws -> WindowsISOInspection) throws -> WindowsMedia {
        let fm = FileManager.default
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        #if os(iOS)
        var mediaRoot = root
        var attributes = URLResourceValues()
        attributes.isExcludedFromBackup = true
        try mediaRoot.setResourceValues(attributes)
        #endif
        let target = root.appendingPathComponent(id.uuidString)
        if linking, fm.fileExists(atPath: target.path), let existing = try list().first(where: { $0.id == id }) {
            return existing
        }
        let bytes = try regularFileSize(source)
        if !linking {
            let free = try root.resourceValues(forKeys: [.volumeAvailableCapacityKey]).volumeAvailableCapacity
            if let free, free >= 0, UInt64(free) < bytes {
                throw VMError.message("Free enough storage for a \(ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)) copy of this ISO.")
            }
        }
        let stage = root.appendingPathComponent(".staging-\(UUID().uuidString)")
        try fm.createDirectory(at: stage, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: stage) }
        let iso = stage.appendingPathComponent("Windows.iso")
        if linking {
            try fm.linkItem(at: source, to: iso)
        } else {
            try ImportCopy.file(source, to: iso, progress: copyProgress)
        }
        let inspection = try inspect(iso)
        let item = WindowsMedia(id: id, name: name, bytes: bytes, images: inspection.images,
                                sha256: checksum, computedSHA256: inspection.computedSHA256,
                                filesystem: inspection.filesystem, diagnostics: inspection.diagnostics)
        try PropertyListEncoder().encode(item).write(to: stage.appendingPathComponent("Media.plist"), options: .atomic)
        try fm.moveItem(at: stage, to: target)
        return item
    }
}
