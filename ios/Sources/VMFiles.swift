// SPDX-License-Identifier: GPL-2.0-or-later
import Darwin
import Foundation

enum VMFiles {
    static let repository = VMRepository(root: URL.applicationSupportDirectory.appendingPathComponent("VirtualMachines"))

    static func copyDisk(from source: URL, to target: URL) throws {
        try copyMachineDisk(from: source, to: target, progress: nil)
    }

    static func copyMachineDisk(from source: URL, to target: URL,
                                progress: (@Sendable (UInt64, UInt64) -> Void)?) throws {
        let raw = source.appendingPathComponent(VMRepository.rawDisk)
        let input = FileManager.default.fileExists(atPath: raw.path) ? raw : source.appendingPathComponent(VMRepository.packedDisk)
        try importDisk(from: input, to: target.appendingPathComponent(VMRepository.rawDisk), progress: progress)
    }

    static func importDisk(from source: URL, to destination: URL,
                           progress: (@Sendable (UInt64, UInt64) -> Void)?) throws {
        let extensionName = source.pathExtension.lowercased()
        guard ["img", "raw", "hvsparse"].contains(extensionName) else {
            throw VMError.message("Choose a raw IMG or RAW disk, or a WindowsiOS-ez HVSPARSE archive.")
        }
        let packed = extensionName == "hvsparse"
        let total = try regularFileSize(source)
        if !packed { try validateRawSize(source) }
        let sink = DiskCopyProgress(progress)
        let context = Unmanaged.passUnretained(sink).toOpaque()
        let result = withPaths(source, destination) { input, output in
            if packed { return hv_sparse_unpack(input, output, VMRepository.minimumDiskBytes, diskCopyProgress, context) }
            if clonefile(input, output, 0) == 0 { return 0 }
            return hv_sparse_copy_raw_progress(input, output, diskCopyProgress, context)
        }
        withExtendedLifetime(sink) {}
        guard result == 0 else {
            throw VMError.message("Could not copy or unpack the virtual disk. Check the image and available storage.")
        }
        try validateRawSize(destination)
        let logicalBytes = packed ? try regularFileSize(destination) : total
        progress?(logicalBytes, logicalBytes)
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

    static func withFileAccess<T>(_ selection: ImportSelection, kind: DocumentImportKind,
                                  perform: (URL) throws -> T) throws -> T {
        let label = kind.rawValue
        Task { @MainActor in ImportDiagnostics.shared.record("\(label) coordinating provider read") }
        var coordinationError: NSError?
        var result: Result<T, Error>?
        defer { withExtendedLifetime(selection) {} }
        NSFileCoordinator().coordinate(readingItemAt: selection.url, options: .withoutChanges,
                                       error: &coordinationError) { source in
            Task { @MainActor in ImportDiagnostics.shared.record("\(label) coordinated read started") }
            result = Result { try perform(source) }
        }
        do {
            if let coordinationError { throw coordinationError }
            guard let result else { throw VMError.message("Could not access the selected file or folder.") }
            let value = try result.get()
            Task { @MainActor in ImportDiagnostics.shared.record("\(label) coordinated read completed") }
            return value
        } catch {
            let code = (error as NSError).code
            Task { @MainActor in ImportDiagnostics.shared.record("\(label) coordinated read failed (code \(code))") }
            throw error
        }
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


private final class DiskCopyProgress {
    let report: (@Sendable (UInt64, UInt64) -> Void)?
    var lastUpdate = Date.distantPast

    init(_ report: (@Sendable (UInt64, UInt64) -> Void)?) { self.report = report }
}

private let diskCopyProgress: @convention(c) (UInt64, UInt64, UnsafeMutableRawPointer?) -> Void = { done, total, context in
    guard let context else { return }
    let sink = Unmanaged<DiskCopyProgress>.fromOpaque(context).takeUnretainedValue()
    let now = Date()
    guard done == total || now.timeIntervalSince(sink.lastUpdate) >= 0.2 else { return }
    sink.lastUpdate = now
    sink.report?(done, total)
}
