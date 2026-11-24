// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum WindowsISO {
    static func inspect(_ url: URL) throws -> [WindowsImage] {
        let size = try regularFileSize(url)
        guard size > 256 * 2048, size % 2048 == 0, let volume = udfread_init() else {
            throw VMError.message("Choose a complete Windows ARM64 ISO file.")
        }
        defer { udfread_close(volume) }
        let opened = url.withUnsafeFileSystemRepresentation { path in
            guard let path else { return false }
            return udfread_open(volume, path) == 0
        }
        guard opened, let root = udfread_opendir(volume, "/") else {
            throw VMError.message("The ISO does not contain a readable UDF filesystem.")
        }
        defer { udfread_closedir(root) }

        guard let efi = open(in: root, names: ["efi"], using: udfread_opendir_at) else {
            throw VMError.message("The ISO has no EFI boot directory.")
        }
        defer { udfread_closedir(efi) }
        guard let boot = open(in: efi, names: ["boot"], using: udfread_opendir_at) else {
            throw VMError.message("The ISO has no EFI boot directory.")
        }
        defer { udfread_closedir(boot) }
        guard let loader = open(in: boot, names: ["bootaa64.efi"], using: udfread_file_openat) else {
            throw VMError.message("The ISO has no ARM64 UEFI boot loader.")
        }
        defer { udfread_file_close(loader) }
        try requireARM64Loader(loader)

        guard let sources = open(in: root, names: ["sources"], using: udfread_opendir_at) else {
            throw VMError.message("The ISO has no Windows installation sources.")
        }
        defer { udfread_closedir(sources) }
        let bootImages = try readImages(in: sources, names: ["boot.wim"])
        try WindowsImageMetadata.requireBaseline(bootImages, media: "Windows PE")
        let installImages = try readImages(in: sources, names: ["install.wim", "install.esd"])
        try WindowsImageMetadata.requireBaseline(installImages, media: "Windows installation image")
        return installImages
    }

    private static func open(in dir: OpaquePointer, names: Set<String>,
                             using opener: (OpaquePointer?, UnsafePointer<CChar>?) -> OpaquePointer?) -> OpaquePointer? {
        udfread_rewinddir(dir)
        var entry = udfread_dirent()
        while udfread_readdir(dir, &entry) != nil {
            if let raw = entry.d_name, let name = String(validatingUTF8: raw), names.contains(name.lowercased()) {
                return opener(dir, raw)
            }
        }
        return nil
    }

    private static func readImages(in directory: OpaquePointer, names: Set<String>) throws -> [WindowsImage] {
        guard let file = open(in: directory, names: names, using: udfread_file_openat) else {
            throw VMError.message("The ISO is missing \(names.sorted().joined(separator: " or ")).")
        }
        defer { udfread_file_close(file) }
        let length = udfread_file_size(file)
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
        guard offset <= Int64.max, udfread_file_seek(file, Int64(offset), Int32(UDF_SEEK_SET)) == Int64(offset) else {
            throw VMError.message("The ISO contains an invalid file offset.")
        }
        var data = Data(count: count)
        let result = data.withUnsafeMutableBytes { bytes in
            udfread_file_read(file, bytes.baseAddress, bytes.count)
        }
        guard result == count else { throw VMError.message("A file inside the ISO is truncated.") }
        return data
    }
}
