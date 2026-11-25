// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum WindowsISO {
    static func inspect(_ url: URL, expectedSHA256: String? = nil,
                        progress: (@Sendable (UInt64, UInt64) -> Void)? = nil) throws -> WindowsISOInspection {
        let checksum = try WindowsDownload.normalizedChecksum(expectedSHA256)
        let hex = try hash(url, progress: progress)
        if let checksum, checksum != hex {
            throw VMError.message("SHA-256 mismatch. Expected \(checksum). Read \(hex). Filesystem inspection was skipped.")
        }

        var diagnostic = [CChar](repeating: 0, count: 512)
        let image = url.withUnsafeFileSystemRepresentation { path in
            disc_open_windows(path, &diagnostic, diagnostic.count)
        }
        let detection = decodeCString(diagnostic)
        let integrity = checksum == nil ? "Computed SHA-256" : "Verified SHA-256"
        guard let image else {
            throw VMError.message("Could not read Windows setup media. \(detection) \(integrity): \(hex).")
        }
        defer { disc_close(image) }

        do {
            guard let loader = disc_open_path(image, "efi/boot/bootaa64.efi") else {
                throw VMError.message("The ISO has no readable ARM64 UEFI boot loader.")
            }
            defer { disc_file_close(loader) }
            try requireARM64Loader(loader)
            let bootImages = try readImages(image, paths: ["sources/boot.wim"])
            try WindowsImageMetadata.requireBaseline(bootImages, media: "Windows PE")
            let installImages = try readImages(image, paths: ["sources/install.wim", "sources/install.esd"])
            try WindowsImageMetadata.requireBaseline(installImages, media: "Windows installation image")
            return WindowsISOInspection(images: installImages, computedSHA256: hex,
                                        filesystem: String(cString: disc_filesystem(image)), diagnostics: detection)
        } catch {
            throw VMError.message("\(error.localizedDescription) \(detection) \(integrity): \(hex).")
        }
    }

    private static func hash(_ url: URL, progress: (@Sendable (UInt64, UInt64) -> Void)?) throws -> String {
        let sink = ISOHashProgress(progress: progress)
        let context = Unmanaged.passUnretained(sink).toOpaque()
        var hex = [CChar](repeating: 0, count: 65)
        let result = url.withUnsafeFileSystemRepresentation { path in
            media_sha256(path, &hex, isoHashProgress, context)
        }
        withExtendedLifetime(sink) {}
        try Task.checkCancellation()
        guard result == 0 else {
            throw VMError.message("Could not compute SHA-256 for the complete ISO. Check that the file is readable.")
        }
        return decodeCString(hex)
    }

    private static func decodeCString(_ bytes: [CChar]) -> String {
        String(decoding: bytes.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }

    private static func readImages(_ image: OpaquePointer, paths: [String]) throws -> [WindowsImage] {
        var opened: OpaquePointer?
        for path in paths {
            if let file = disc_open_path(image, path) {
                opened = file
                break
            }
        }
        guard let file = opened else {
            throw VMError.message("The ISO is missing \(paths.joined(separator: " or ")).")
        }
        defer { disc_file_close(file) }
        let length = disc_file_size(file)
        guard length >= 96 else { throw VMError.message("A Windows image inside the ISO is empty or truncated.") }
        return try WindowsImageMetadata.read(length: UInt64(length)) { offset, count in
            try read(file, offset: offset, count: count)
        }
    }

    private static func requireARM64Loader(_ file: OpaquePointer) throws {
        let header = try read(file, offset: 0, count: 64)
        guard header[0] == 0x4d, header[1] == 0x5a else {
            throw VMError.message("The ARM64 UEFI boot loader is not a valid PE executable.")
        }
        let offset = header.littleEndian(at: 60, bytes: 4)
        guard offset >= 64, try read(file, offset: offset, count: 6) == Data([0x50, 0x45, 0, 0, 0x64, 0xaa]) else {
            throw VMError.message("The UEFI boot loader does not target ARM64.")
        }
    }

    private static func read(_ file: OpaquePointer, offset: UInt64, count: Int) throws -> Data {
        var data = Data(count: count)
        let result = data.withUnsafeMutableBytes { bytes in
            disc_file_pread(file, bytes.baseAddress, bytes.count, offset)
        }
        guard result == count else { throw VMError.message("A file inside the ISO is truncated or the filesystem could not read it.") }
        return data
    }
}

private final class ISOHashProgress {
    let progress: (@Sendable (UInt64, UInt64) -> Void)?
    var lastUpdate = Date.distantPast

    init(progress: (@Sendable (UInt64, UInt64) -> Void)?) { self.progress = progress }
}

private let isoHashProgress: @convention(c) (UInt64, UInt64, UnsafeMutableRawPointer?) -> Int32 = { bytes, total, context in
    if Task.isCancelled { return 1 }
    if let context {
        let sink = Unmanaged<ISOHashProgress>.fromOpaque(context).takeUnretainedValue()
        if let progress = sink.progress {
            let date = Date()
            if bytes == total || date.timeIntervalSince(sink.lastUpdate) >= 0.2 {
                sink.lastUpdate = date
                progress(bytes, total)
            }
        }
    }
    return 0
}
