// SPDX-License-Identifier: GPL-2.0-or-later
import Darwin
import Foundation

enum VMFiles {
    static let repository = VMRepository(root: URL.applicationSupportDirectory.appendingPathComponent("VirtualMachines"))

    static func copyDisk(from source: URL, to target: URL) throws {
        let raw = source.appendingPathComponent(VMRepository.rawDisk)
        let packed = source.appendingPathComponent(VMRepository.packedDisk)
        let output = target.appendingPathComponent(VMRepository.rawDisk)
        let result: Int32
        if FileManager.default.fileExists(atPath: raw.path) {
            try validateRawSize(raw)
            result = withPaths(raw, output) { input, destination in
                if clonefile(input, destination, 0) == 0 { return 0 }
                return hv_sparse_copy_raw(input, destination)
            }
        } else {
            _ = try regularFileSize(packed)
            result = withPaths(packed, output) { input, destination in
                hv_sparse_unpack(input, destination, VMRepository.minimumDiskBytes, nil, nil)
            }
        }
        guard result == 0 else {
            throw VMError.message("Could not copy or unpack the virtual disk. Check the image and available storage.")
        }
        try validateRawSize(output)
    }

    static func exportDisk(from source: URL, to target: URL) throws {
        let raw = source.appendingPathComponent(VMRepository.rawDisk)
        let output = target.appendingPathComponent(VMRepository.packedDisk)
        try validateRawSize(raw)
        guard withPaths(raw, output, hv_sparse_pack) == 0 else {
            throw VMError.message("Could not pack the disk. Check available space and try again.")
        }
        let valid = output.withUnsafeFileSystemRepresentation { path in
            guard let path else { return false }
            return hv_sparse_validate(path, VMRepository.minimumDiskBytes) == 0
        }
        guard valid else { throw VMError.message("The exported sparse disk did not pass validation.") }
    }

    static func withFileAccess<T>(_ url: URL, perform: (URL) throws -> T) throws -> T {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        var coordinationError: NSError?
        var result: Result<T, Error>?
        NSFileCoordinator().coordinate(readingItemAt: url, options: .withoutChanges, error: &coordinationError) { source in
            result = Result { try perform(source) }
        }
        if let coordinationError { throw coordinationError }
        guard let result else { throw VMError.message("Could not access the selected file or folder.") }
        return try result.get()
    }

    private static func withPaths(_ input: URL, _ output: URL,
                                  _ action: (UnsafePointer<CChar>, UnsafePointer<CChar>) -> Int32) -> Int32 {
        input.withUnsafeFileSystemRepresentation { input in
            output.withUnsafeFileSystemRepresentation { output in
                guard let input, let output else { return -1 }
                return action(input, output)
            }
        }
    }

    private static func validateRawSize(_ raw: URL) throws {
        let size = try regularFileSize(raw)
        guard size >= VMRepository.minimumDiskBytes, size % 512 == 0 else {
            throw VMError.message("The raw disk must be at least 64 MiB with a size aligned to 512-byte sectors.")
        }
    }
}
