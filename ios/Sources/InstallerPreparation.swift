// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct InstallerProgress: Sendable {
    enum Stage: String, Sendable {
        case checking = "Checking installation media SHA-256"
        case winPE = "Preparing Windows PE"
        case installer = "Writing the installer disk"
    }

    let stage: Stage
    let copied: UInt64
    let total: UInt64
}

enum InstallerPreparation {
    static func verifyWindowsBootFiles(_ disk: URL) throws {
        let efi = try PreparedDisk.efiPartition(in: disk)
        let present = disk.withUnsafeFileSystemRepresentation { path in
            winios_has_windows_boot_files(path, efi.start, efi.sectors) == 1
        }
        guard present else {
            throw VMError.message("The primary disk has no ARM64 Windows Boot Manager and BCD. Review the installation logs.")
        }
    }

    static func makeDirectTarget(_ disk: URL, capacityGiB: Int, guid: UUID) throws {
        let result = disk.withUnsafeFileSystemRepresentation { path in
            guid.uuidString.withCString { winios_create_direct_target(path, UInt64(capacityGiB) << 30, $0) }
        }
        guard result == 0 else { throw VMError.message("Could not create the new sparse GPT target for DISM.") }
    }

    static func build(iso: URL, output: URL, expectedSHA256: String?, drivers: URL? = nil,
                      edition: Int? = nil, directTarget: UUID? = nil,
                      progress: @escaping @Sendable (InstallerProgress) -> Void) throws {
        try Task.checkCancellation()
        let manager = FileManager.default
        if manager.fileExists(atPath: output.path) {
            guard try regularFileSize(output) >= 256 << 20 else {
                throw VMError.message("The existing installer image is incomplete.")
            }
            return
        }

        progress(InstallerProgress(stage: .checking, copied: 0, total: 0))
        let inspection = try WindowsISO.inspect(iso, expectedSHA256: expectedSHA256) { bytes, total in
            progress(InstallerProgress(stage: .checking, copied: bytes, total: total))
        }
        if let edition, !inspection.images.contains(where: { $0.index == edition }) {
            throw VMError.message("The selected Windows edition is missing from this ISO.")
        }
        let directImageIndex: Int32?
        if directTarget != nil {
            let image = try WindowsImageMetadata.directImage(in: inspection.images, requested: edition)
            guard let index = Int32(exactly: image.index) else { throw VMError.message("The DISM image index is invalid.") }
            directImageIndex = index
        } else {
            directImageIndex = nil
        }
        let parent = output.deletingLastPathComponent()
        try manager.createDirectory(at: parent, withIntermediateDirectories: true)
        let workspace = parent.appendingPathComponent(".staging-\(UUID().uuidString)")
        try manager.createDirectory(at: workspace, withIntermediateDirectories: false)
        defer { try? manager.removeItem(at: workspace) }
        let wim = workspace.appendingPathComponent("boot.wim")
        let stagedDisk = workspace.appendingPathComponent(VMRepository.installerDisk)
        let answer = workspace.appendingPathComponent("Unattend.xml")
        if directTarget == nil, let edition { try WindowsUnattend.make(imageIndex: edition).write(to: answer, options: .atomic) }

        let sink = InstallerProgressSink(progress: progress)
        let opaque = Unmanaged.passUnretained(sink).toOpaque()
        var error = [CChar](repeating: 0, count: 512)
        sink.setStage(.winPE)
        let winPEResult = iso.withUnsafeFileSystemRepresentation { input in
            workspace.withUnsafeFileSystemRepresentation { directory in
                if let guid = directTarget?.uuidString, let directImageIndex {
                    return guid.withCString {
                        winios_prepare_direct_winpe_wim(input, directory, $0, directImageIndex,
                                                        installerProgress, opaque, &error, error.count)
                    }
                }
                return winios_prepare_winpe_wim(input, directory, installerProgress, opaque, &error, error.count)
            }
        }
        guard winPEResult == 0 else {
            throw VMError.message("Could not prepare Windows PE. \(installerMessage(error))")
        }

        sink.setStage(.installer)
        let diskResult = iso.withUnsafeFileSystemRepresentation { input in
            stagedDisk.withUnsafeFileSystemRepresentation { destination in
                wim.withUnsafeFileSystemRepresentation { prepared in
                    let driverPath = drivers?.path
                    if let guid = directTarget?.uuidString {
                        return guid.withCString {
                            winios_build_direct_installer_disk(input, destination, prepared, driverPath, $0,
                                                              installerProgress, opaque, &error, error.count)
                        }
                    }
                    let answerPath = edition == nil ? nil : answer.path
                    return winios_build_installer_disk(input, destination, prepared, driverPath, answerPath,
                                                        installerProgress, opaque, &error, error.count)
                }
            }
        }
        withExtendedLifetime(sink) {}
        guard diskResult == 0 else {
            throw VMError.message("Could not write the installer disk. \(installerMessage(error))")
        }
        try Task.checkCancellation()
        try manager.linkItem(at: stagedDisk, to: output)
    }

    private static func installerMessage(_ buffer: [CChar]) -> String {
        String(decoding: buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }
}

private final class InstallerProgressSink {
    private let progress: @Sendable (InstallerProgress) -> Void
    private var stage: InstallerProgress.Stage = .checking
    private var lastUpdate = Date.distantPast

    init(progress: @escaping @Sendable (InstallerProgress) -> Void) {
        self.progress = progress
    }

    func setStage(_ stage: InstallerProgress.Stage) {
        self.stage = stage
        lastUpdate = .distantPast
        progress(InstallerProgress(stage: stage, copied: 0, total: 0))
    }

    func update(copied: UInt64, total: UInt64) {
        let now = Date()
        guard copied == total || now.timeIntervalSince(lastUpdate) >= 0.25 else { return }
        lastUpdate = now
        progress(InstallerProgress(stage: stage, copied: copied, total: total))
    }
}

private func installerProgress(_ copied: UInt64, _ total: UInt64, _ context: UnsafeMutableRawPointer?) -> Int32 {
    guard let context else { return 0 }
    if Task.isCancelled { return 1 }
    Unmanaged<InstallerProgressSink>.fromOpaque(context).takeUnretainedValue().update(copied: copied, total: total)
    return 0
}
