// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct InstallationDraft: Codable, Equatable {
    enum Stage: Int, CaseIterable, Codable {
        case welcome, device, jit, media, hardware, storage, review, preparing, prepared

        var title: String {
            switch self {
            case .welcome: "Welcome"
            case .device: "Device compatibility"
            case .jit: "JIT setup"
            case .media: "Windows installation media"
            case .hardware: "VM hardware"
            case .storage: "Virtual storage"
            case .review: "Review"
            case .preparing: "Disk and firmware preparation"
            case .prepared: "Preparation complete"
            }
        }
    }

    var stage: Stage = .welcome
    var machineID = UUID()
    var name = "Windows 11"
    var hardware = VMHardware()
    var capacityGiB = 64
    var mediaID: UUID?
    var lastError: String?

    func validateForPreparation() throws {
        guard mediaID != nil else { throw VMError.message("Select verified Windows 11 24H2 ARM64 installation media.") }
        try VirtualMachine(id: machineID, name: name, hardware: hardware, installation: .blankDisk,
                           capacityGiB: capacityGiB, mediaID: mediaID).validate()
    }
}

struct InstallationDraftStore: Sendable {
    let root: URL

    private var stateFile: URL { root.appendingPathComponent("Draft.plist") }

    var code: URL { root.appendingPathComponent(VMRepository.firmware) }
    var variables: URL { root.appendingPathComponent(VMRepository.nvram) }

    func load() throws -> InstallationDraft {
        guard FileManager.default.fileExists(atPath: stateFile.path) else { return InstallationDraft() }
        return try PropertyListDecoder().decode(InstallationDraft.self, from: Data(contentsOf: stateFile))
    }

    func save(_ draft: InstallationDraft) throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        try PropertyListEncoder().encode(draft).write(to: stateFile, options: .atomic)
    }

    func importFirmware(_ source: URL, destination: URL) throws {
        try VMRepository.validateFirmware(source, isVariables: destination.lastPathComponent == VMRepository.nvram)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        // The EDK2 banks have already been limited to 64 MiB by validateFirmware.
        try Data(contentsOf: source).write(to: destination, options: .atomic)
    }

    func prepare(_ draft: InstallationDraft, repository: VMRepository) throws -> VirtualMachine {
        try draft.validateForPreparation()
        return try repository.createBlank(id: draft.machineID, name: draft.name, hardware: draft.hardware,
                                          capacityGiB: draft.capacityGiB, mediaID: draft.mediaID,
                                          code: code, variables: variables)
    }

    func cleanupFirmwareCopies() {
        try? FileManager.default.removeItem(at: code)
        try? FileManager.default.removeItem(at: variables)
    }
}
