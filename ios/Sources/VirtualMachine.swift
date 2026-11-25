// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum HardwarePreset: String, CaseIterable, Codable, Identifiable {
    case automatic, compatibility, balanced, performance, experimental, custom

    var id: Self { self }
    var title: String { rawValue.capitalized }

    var settings: (ramMiB: Int, jitMiB: Int)? {
        switch self {
        case .automatic, .custom: nil
        case .compatibility: (4608, 384)
        case .balanced: (6144, 512)
        case .performance: (8192, 1024)
        case .experimental: (10240, 1536)
        }
    }
}

struct VMHardware: Codable, Equatable {
    static let vCPURange = 1...8
    var preset: HardwarePreset = .automatic
    var customRAMMiB = 4608
    var customJITMiB = 384
    var jitOverrideMiB: Int?
    var vCPUs = 4

    var ramMiB: Int { preset.settings?.ramMiB ?? (preset == .custom ? customRAMMiB : 0) }
    var jitMiB: Int { jitOverrideMiB ?? preset.settings?.jitMiB ?? (preset == .custom ? customJITMiB : 384) }

    func validate() throws {
        guard Self.vCPURange.contains(vCPUs) else {
            throw VMError.message("Choose \(Self.vCPURange.lowerBound)–\(Self.vCPURange.upperBound) virtual CPUs.")
        }
        guard (preset != .custom || ramMiB >= 4096), jitMiB > 0 else {
            throw VMError.message("Windows 11 needs at least 4096 MiB of RAM and a nonzero JIT cache.")
        }
    }
}

enum WindowsInstallationMethod: String, Codable, CaseIterable {
    case direct, graphical

    var title: String {
        switch self {
        case .direct: "DISM provisioning"
        case .graphical: "Graphical Setup (Experimental)"
        }
    }
}

struct VirtualMachine: Codable, Identifiable, Equatable {
    enum Installation: String, Codable {
        case preparedDisk, blankDisk
    }

    let id: UUID
    var name: String
    var hardware: VMHardware
    var installation: Installation
    var usesTPM: Bool?
    var installationMethod: WindowsInstallationMethod?
    var lastOpened: Date?
    var displayScaling: String?
    // Optional for backwards-compatible manifests; imported and new VMs default to RAMFB.
    var experimentalGraphics: Bool?
    var windowsEdition: String?
    var lastGuestWidth: UInt32?
    var lastGuestHeight: UInt32?
    let capacityGiB: Int?
    let mediaID: UUID?

    init(id: UUID = UUID(), name: String, hardware: VMHardware = VMHardware(),
         installation: Installation, capacityGiB: Int? = nil, mediaID: UUID? = nil, usesTPM: Bool? = nil,
         installationMethod: WindowsInstallationMethod? = nil,
         displayScaling: String? = nil, windowsEdition: String? = nil) {
        self.id = id
        self.name = name
        self.hardware = hardware
        self.installation = installation
        self.usesTPM = usesTPM
        self.installationMethod = installationMethod
        self.displayScaling = displayScaling
        self.windowsEdition = windowsEdition
        self.capacityGiB = capacityGiB
        self.mediaID = mediaID
    }

    func validate() throws {
        guard !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, name.count <= 100 else {
            throw VMError.message("Enter a name of 1 to 100 characters.")
        }
        try hardware.validate()
        if installation == .blankDisk {
            guard let capacityGiB, capacityGiB >= 64, capacityGiB <= Int64.max >> 30 else {
                throw VMError.message("Enter a virtual disk capacity of at least 64 GiB that fits in a signed 64-bit file size.")
            }
        }
    }
}

enum VMError: LocalizedError {
    case message(String)

    var errorDescription: String? {
        switch self {
        case .message(let text): text
        }
    }
}

/// Each machine is published only after its files and manifest have been staged.
struct VMRepository: Sendable {
    private static let manifest = "Machine.plist"
    static let rawDisk = "windows11.img"
    static let packedDisk = "windows11.hvsparse"
    static let firmware = "code.fd"
    static let nvram = "vars.fd"
    static let preview = "DesktopPreview.png"
    static let setupLogs = "SetupLogs.img"
    static let installerDisk = "installer.img"
    static let tpmState = "TPM"
    static let minimumDiskBytes: UInt64 = 64 << 20
    private static let flashBankBytes: UInt64 = 64 << 20

    let root: URL

    func directory(for machine: VirtualMachine) -> URL {
        root.appendingPathComponent(machine.id.uuidString, isDirectory: true)
    }

    func list() throws -> [VirtualMachine] {
        let children = try FileManager.default.contentsOfDirectory(
            at: root, includingPropertiesForKeys: [.isDirectoryKey, .isSymbolicLinkKey]
        )
        return try children.compactMap { folder in
            guard let id = UUID(uuidString: folder.lastPathComponent) else { return nil }
            let values = try folder.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            guard values.isDirectory == true, values.isSymbolicLink != true else { return nil }
            let machine = try loadManifest(in: folder)
            guard machine.id == id else { throw VMError.message("Machine identifier does not match its folder.") }
            return machine
        }.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    func update(_ machine: VirtualMachine) throws {
        try machine.validate()
        let folder = directory(for: machine)
        _ = try loadManifest(in: folder)
        try writeManifest(machine, in: folder)
    }

    func remove(_ machine: VirtualMachine) throws {
        let folder = directory(for: machine)
        guard try loadManifest(in: folder).id == machine.id else {
            throw VMError.message("Machine identifier does not match its folder.")
        }
        try FileManager.default.removeItem(at: folder)
    }

    func createBlank(id: UUID, name: String, hardware: VMHardware, capacityGiB: Int, mediaID: UUID?,
                     code: URL, variables: URL, installer: URL? = nil,
                     installationMethod: WindowsInstallationMethod? = nil,
                     prepareTarget: ((URL) throws -> Void)? = nil, windowsEdition: String? = nil,
                     displayScaling: String? = nil) throws -> VirtualMachine {
        if installationMethod == .direct, installer == nil || prepareTarget == nil {
            throw VMError.message("DISM provisioning requires both the installer and a new GPT target.")
        }
        let machine = VirtualMachine(id: id, name: name, hardware: hardware, installation: .blankDisk,
                                     capacityGiB: capacityGiB, mediaID: mediaID, usesTPM: true,
                                     installationMethod: installationMethod, displayScaling: displayScaling, windowsEdition: windowsEdition)
        if FileManager.default.fileExists(atPath: directory(for: machine).path) {
            let existing = try loadManifest(in: directory(for: machine))
            var updated = existing
            updated.usesTPM = true
            guard updated == machine else {
                throw VMError.message("A different virtual machine already uses this identifier.")
            }
            return existing
        }
        try publish(machine) { stage in
            try copyFirmware(code: code, variables: variables, to: stage)
            if let installer {
                _ = try regularFileSize(installer)
                try FileManager.default.linkItem(at: installer, to: stage.appendingPathComponent(Self.installerDisk))
            }
            let disk = stage.appendingPathComponent(Self.rawDisk)
            if let prepareTarget {
                try prepareTarget(disk)
                return
            }
            guard FileManager.default.createFile(atPath: disk.path, contents: nil) else {
                throw VMError.message("Unable to create the virtual disk.")
            }
            let handle = try FileHandle(forWritingTo: disk)
            defer { try? handle.close() }
            try handle.truncate(atOffset: UInt64(capacityGiB) << 30)
            try handle.synchronize()
        }
        return machine
    }

    func importPrepared(from source: URL, copyDisk: (URL, URL) throws -> Void) throws -> VirtualMachine {
        let manifest = source.appendingPathComponent(Self.manifest)
        let original = FileManager.default.fileExists(atPath: manifest.path) ? try loadManifest(in: source) : nil
        let machine = VirtualMachine(name: original?.name ?? source.lastPathComponent,
                                     hardware: original?.hardware ?? VMHardware(),
                                     installation: original?.installation ?? .preparedDisk,
                                     capacityGiB: original?.capacityGiB, usesTPM: original?.usesTPM,
                                     installationMethod: original?.installationMethod, displayScaling: original?.displayScaling,
                                     windowsEdition: original?.windowsEdition)
        try publish(machine) { stage in
            try copyMachineFiles(from: source, to: stage, copyDisk: copyDisk)
            if machine.installation == .preparedDisk {
                try PreparedDisk.validate(stage.appendingPathComponent(Self.rawDisk))
                if machine.usesTPM == true {
                    _ = try regularFileSize(stage.appendingPathComponent(Self.tpmState).appendingPathComponent("permall"))
                }
            }
        }
        return machine
    }

    func importDisk(from source: URL, firmware: VMFirmware,
                    copy: (URL, URL) throws -> Void) throws -> VirtualMachine {
        let name = String(source.deletingPathExtension().lastPathComponent.prefix(96))
        let machine = VirtualMachine(name: name, installation: .preparedDisk)
        try publish(machine) { stage in
            try copyFirmware(code: firmware.code, variables: firmware.variables, to: stage)
            let disk = stage.appendingPathComponent(Self.rawDisk)
            try copy(source, disk)
            try PreparedDisk.validate(disk)
        }
        return machine
    }

    func duplicate(_ original: VirtualMachine, copyDisk: (URL, URL) throws -> Void) throws -> VirtualMachine {
        let machine = VirtualMachine(name: String(original.name.prefix(95)) + " Copy",
                                     hardware: original.hardware, installation: original.installation,
                                     capacityGiB: original.capacityGiB, mediaID: original.mediaID, usesTPM: original.usesTPM,
                                     installationMethod: original.installationMethod, displayScaling: original.displayScaling,
                                     windowsEdition: original.windowsEdition)
        try publish(machine) { stage in
            try copyMachineFiles(from: directory(for: original), to: stage, copyDisk: copyDisk)
        }
        return machine
    }

    func export(_ machine: VirtualMachine, to workspace: URL, copyDisk: (URL, URL) throws -> Void) throws -> URL {
        let folder = workspace.appendingPathComponent("Windows VM \(machine.id.uuidString.prefix(8))", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)
        do {
            try copyMachineFiles(from: directory(for: machine), to: folder, copyDisk: copyDisk)
            try writeManifest(machine, in: folder)
            return folder
        } catch {
            try? FileManager.default.removeItem(at: folder)
            throw error
        }
    }

    private func publish(_ machine: VirtualMachine, populate: (URL) throws -> Void) throws {
        try machine.validate()
        let manager = FileManager.default
        try manager.createDirectory(at: root, withIntermediateDirectories: true)
        let stage = root.appendingPathComponent(".staging-\(UUID().uuidString)", isDirectory: true)
        try manager.createDirectory(at: stage, withIntermediateDirectories: false)
        defer { try? manager.removeItem(at: stage) }
        try populate(stage)
        try writeManifest(machine, in: stage)
        try manager.moveItem(at: stage, to: directory(for: machine))
    }

    private func loadManifest(in folder: URL) throws -> VirtualMachine {
        let manifest = folder.appendingPathComponent(Self.manifest)
        guard try regularFileSize(manifest) <= 64 << 10 else {
            throw VMError.message("The machine manifest is too large.")
        }
        let machine = try PropertyListDecoder().decode(VirtualMachine.self, from: Data(contentsOf: manifest))
        try machine.validate()
        return machine
    }

    private func writeManifest(_ machine: VirtualMachine, in folder: URL) throws {
        try PropertyListEncoder().encode(machine)
            .write(to: folder.appendingPathComponent(Self.manifest), options: .atomic)
    }

    private func copyMachineFiles(from source: URL, to target: URL, copyDisk: (URL, URL) throws -> Void) throws {
        try copyFirmware(code: source.appendingPathComponent(Self.firmware),
                         variables: source.appendingPathComponent(Self.nvram), to: target)
        try copyDisk(source, target)
        let state = source.appendingPathComponent(Self.tpmState)
        if FileManager.default.fileExists(atPath: state.path) {
            let properties = try state.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            guard properties.isDirectory == true, properties.isSymbolicLink != true else {
                throw VMError.message("The TPM state must be a real directory.")
            }
            let folder = target.appendingPathComponent(Self.tpmState)
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false,
                                                    attributes: [.posixPermissions: 0o700])
            #if os(iOS)
            var privateFolder = folder
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try privateFolder.setResourceValues(values)
            #endif
            for name in ["permall", "savestate", "volatilestate"] {
                let input = state.appendingPathComponent(name)
                guard FileManager.default.fileExists(atPath: input.path) else { continue }
                let bytes = try regularFileSize(input)
                guard bytes > 0 && bytes <= 4 << 20 else {
                    throw VMError.message("A TPM state file has an invalid size.")
                }
                try FileManager.default.copyItem(at: input, to: folder.appendingPathComponent(name))
            }
        }
    }

    static func validateFirmware(_ source: URL, isVariables: Bool) throws {
        let size = try regularFileSize(source)
        guard size >= 56, size <= flashBankBytes else {
            throw VMError.message("The selected file must fit in a 64 MiB firmware bank.")
        }
        let handle = try FileHandle(forReadingFrom: source)
        defer { try? handle.close() }
        // ArmVirtQemu commonly places an ARM trampoline in the first 4 KiB of code flash.
        let candidates: [UInt64] = isVariables ? [0] : [0, 4096]
        let variableGUID = Data([0x8d, 0x2b, 0xf1, 0xff, 0x96, 0x76, 0x8b, 0x4c,
                                 0xa9, 0x85, 0x27, 0x47, 0x07, 0x5b, 0x4f, 0x50])
        for offset in candidates where size >= offset + 56 {
            try handle.seek(toOffset: offset)
            guard let header = try handle.read(upToCount: 56), header.count == 56,
                  header[40..<44] == Data("_FVH".utf8),
                  (header[16..<32] == variableGUID) == isVariables else { continue }
            let length = header.littleEndian(at: 32, bytes: 8)
            if length >= 56 && length <= size - offset { return }
        }
        throw VMError.message("Choose the matching ArmVirtQemu \(isVariables ? "UEFI variable store" : "UEFI code image").")
    }

    private func copyFirmware(code: URL, variables: URL, to folder: URL) throws {
        for (source, name) in [(code, Self.firmware), (variables, Self.nvram)] {
            try Self.validateFirmware(source, isVariables: name == Self.nvram)
            let destination = folder.appendingPathComponent(name)
            try FileManager.default.copyItem(at: source, to: destination)
            if name == Self.nvram {
                try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: destination.path)
            }
        }
    }
}

func regularFileSize(_ file: URL) throws -> UInt64 {
    let attributes = try FileManager.default.attributesOfItem(atPath: file.path)
    guard attributes[.type] as? FileAttributeType == .typeRegular,
          let size = attributes[.size] as? NSNumber else {
        throw VMError.message("\(file.lastPathComponent) must be a regular file.")
    }
    return size.uint64Value
}

/// Called once when opening the app's private storage, before any file operation starts.
func removeAbandonedStaging(in root: URL) {
    let manager = FileManager.default
    guard let entries = try? manager.contentsOfDirectory(at: root, includingPropertiesForKeys: [.isDirectoryKey]) else {
        return
    }
    let prefix = ".staging-"
    for entry in entries {
        let name = entry.lastPathComponent
        guard name.hasPrefix(prefix), UUID(uuidString: String(name.dropFirst(prefix.count))) != nil,
              (try? entry.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true else { continue }
        try? manager.removeItem(at: entry)
    }
}

extension Data {
    func littleEndian(at offset: Int, bytes: Int) -> UInt64 {
        (0..<bytes).reduce(0) { value, index in
            value | (UInt64(self[startIndex + offset + index]) << (index * 8))
        }
    }
}
