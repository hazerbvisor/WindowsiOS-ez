// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct InstallationAssistant: View {
    enum InstallationFile { case code, variables, drivers }

    let vm: VMViewModel
    let onLaunch: (VirtualMachine) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var shelf = MediaShelf.shared
    @State private var draft = InstallationDraft()
    @State private var loaded = false
    @State private var saving = false
    @State private var installerProgress: InstallerProgress?
    @State private var preparationTask: Task<VirtualMachine, Error>?
    @State private var showMedia = false
    @State private var pickerRequest: DocumentPickerRequest?
    @State private var importProgress: ImportProgress?
    @State private var fileSelection: InstallationFile = .code
    @State private var hasCode = false
    @State private var hasVars = false
    @State private var freeDiskBytes: Int?
    @State private var error: String?

    private let store = InstallationDraftStore(root: URL.applicationSupportDirectory.appendingPathComponent("Installation"))
    private let repository = VMFiles.repository

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Label(stageTitle, systemImage: "desktopcomputer")
                        .font(.headline)
                }
                stageContents
                if let message = error ?? draft.lastError {
                    Section {
                        Text(message).foregroundStyle(.red)
                        ShareLink(item: "WindowsiOS-ez installation preparation\nStage: \(draft.stage.title)\nError: \(message)") {
                            Label("Share Failure Details", systemImage: "square.and.arrow.up")
                        }
                    }
                }
            }
            .navigationTitle("Create Virtual Machine")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                        .disabled(saving)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(continueLabel, action: advance)
                        .disabled(!loaded || saving || (draft.stage == .media && !hasSelectedMedia))
                }
                ToolbarItem(placement: .bottomBar) {
                    if draft.stage == .hardware || (draft.stage == .preparing && !saving) {
                        Button("Back", systemImage: "chevron.backward") {
                            advance(to: draft.stage == .preparing ? .hardware : .media)
                        }
                    }
                }
            }
            .task { load() }
            .onChange(of: shelf.items) { _, _ in selectOnlyMedia() }
            .onChange(of: draft.mediaID) { _, _ in
                if let index = draft.editionIndex, let media = selectedMedia,
                   !media.images.contains(where: { $0.index == index }) {
                    draft.editionIndex = nil
                }
            }
            .onChange(of: draft) { _, newDraft in
                guard loaded else { return }
                do { try store.save(newDraft) }
                catch { self.error = error.localizedDescription }
            }
            .sheet(isPresented: $showMedia) {
                NavigationStack {
                    MediaLibraryView()
                        .toolbar { ToolbarItem(placement: .cancellationAction) {
                            Button("Done") { showMedia = false }
                        } }
                }
            }
            .documentPicker($pickerRequest) { kind, selection in
                if kind == .drivers { importDrivers(selection) }
                else { copyFirmware(selection, file: fileSelection) }
            }
            .interactiveDismissDisabled(saving)
        }
    }

    @ViewBuilder private var stageContents: some View {
        switch draft.stage {
        case .welcome, .device, .jit, .media:
            Section("Media") {
                Picker("Windows ISO", selection: $draft.mediaID) {
                    Text("Choose an ISO").tag(UUID?.none)
                    ForEach(shelf.items) { item in
                        Text(item.name).tag(Optional(item.id))
                    }
                }
                if let media = selectedMedia {
                    Picker("Windows edition", selection: $draft.editionIndex) {
                        Text("Choose in Windows Setup").tag(Int?.none)
                        ForEach(media.images, id: \.index) { image in
                            Text(image.name).tag(Optional(image.index))
                        }
                    }
                    Text("Selecting an edition lets Setup partition a verified blank VM disk. Windows still asks you to accept its terms and complete account setup.")
                        .foregroundStyle(.secondary)
                }
                Button("Open Downloads", systemImage: "arrow.down.circle") { showMedia = true }
            }
        case .hardware, .storage, .review:
            TextField("Name", text: $draft.name)
            HardwareFields(hardware: $draft.hardware)
            Section {
                ForEach([64, 128, 256], id: \.self) { size in
                    Button {
                        draft.capacityGiB = size
                    } label: {
                        HStack {
                            Text("\(size) GiB")
                            Spacer()
                            if draft.capacityGiB == size { Image(systemName: "checkmark") }
                        }
                    }
                }
                TextField("Custom capacity (GiB)", value: $draft.capacityGiB, format: .number)
                    .keyboardType(.numberPad)
                if let freeDiskBytes {
                    LabeledContent("Available storage", value: ByteCountFormatter.string(fromByteCount: Int64(freeDiskBytes), countStyle: .file))
                }
            } header: {
                Text("Sparse disk capacity")
            } footer: {
                Text("64 GiB is the recommended logical capacity. The disk grows as Windows writes to it. Budget at least 32 GiB of free space for Windows and staging, plus the ISO if it is not already stored. Actual use depends on the edition and updates.")
            }
            Section {
                NavigationLink("Advanced Settings") { advancedSettings }
            }
        case .preparing:
            if saving, let progress = installerProgress {
                if progress.total > 0 {
                    ProgressView(progress.stage.rawValue, value: Double(progress.copied), total: Double(progress.total))
                } else {
                    ProgressView(progress.stage.rawValue)
                }
                Button("Cancel Preparation", role: .cancel) { preparationTask?.cancel() }
            } else if saving {
                ProgressView("Preparing installation…")
            } else {
                Text("Preparation was interrupted. Retry to finish publishing the virtual machine.")
            }
        case .prepared:
            Label("Virtual machine saved", systemImage: "checkmark.circle")
            Text("Windows Setup checks the guest hardware and handles the standard Windows installation.")
                .foregroundStyle(.secondary)
            Button("Launch Windows Setup", systemImage: "play") {
                do {
                    guard let machine = try repository.list().first(where: { $0.id == draft.machineID }),
                          FileManager.default.fileExists(atPath: repository.directory(for: machine)
                            .appendingPathComponent(VMRepository.installerDisk).path) else {
                        throw VMError.message("Open this machine in the library to build its Setup disk.")
                    }
                    try store.save(InstallationDraft())
                    onLaunch(machine)
                    dismiss()
                } catch {
                    self.error = error.localizedDescription
                }
            }
            .disabled(vm.needsRelaunchToStart)
            JITStatusSection(vm: vm)
        }
    }

    private var advancedSettings: some View {
        Form {
            Section("EDK2 firmware") {
                Toggle("Use custom firmware", isOn: Binding(
                    get: { draft.useCustomFirmware == true },
                    set: { draft.useCustomFirmware = $0 }
                ))
                .disabled(saving)
                if draft.useCustomFirmware == true {
                    Button(hasCode ? "Replace code.fd" : "Choose code.fd", systemImage: "doc") { choose(.code) }
                        .disabled(saving)
                    Button(hasVars ? "Replace vars.fd" : "Choose vars.fd", systemImage: "doc") { choose(.variables) }
                        .disabled(saving)
                }
                Text("The default uses bundled QEMU ArmVirt firmware. Each VM receives a separate writable variable store.")
                    .foregroundStyle(.secondary)
            }
            if draft.useCustomFirmware == true {
                ImportInboxSection(kinds: [.firmware], disabled: saving) { copyFirmware(ImportSelection($0.url), file: nil) }
            }
            Section("Network driver") {
                Button(draft.driverID == nil ? "Choose ARM64 NetKVM Folder" : "Replace NetKVM Folder", systemImage: "folder") {
                    choose(.drivers)
                }
                .disabled(saving)
                if draft.driverID != nil {
                    Button("Use Bundled NetKVM") { clearDriverSelection() }
                        .disabled(saving)
                }
                Text("Windows Setup checks the driver catalog and signature. The default uses the bundled Windows 11 ARM64 package.")
                    .foregroundStyle(.secondary)
            }
            ImportInboxSection(kinds: [.drivers], disabled: saving) { importDrivers(ImportSelection($0.url)) }
            if saving { ImportProgressView(progress: importProgress) }
            if let error { Text(error).foregroundStyle(.red) }
        }
        .navigationTitle("Advanced Settings")
    }

    private var stageTitle: String {
        switch draft.stage {
        case .welcome, .device, .jit, .media: "Windows installation media"
        case .hardware, .storage, .review: "Hardware and storage"
        default: draft.stage.title
        }
    }

    private var selectedMedia: WindowsMedia? { shelf.items.first { $0.id == draft.mediaID } }
    private var hasSelectedMedia: Bool { selectedMedia != nil }

    private var continueLabel: String {
        switch draft.stage {
        case .hardware, .storage, .review: "Install"
        case .preparing: "Retry"
        case .prepared: "Done"
        default: "Continue"
        }
    }

    private func load() {
        shelf.open()
        guard !loaded else { return }
        do {
            draft = try store.load()
            switch draft.stage {
            case .welcome, .device, .jit: draft.stage = .media
            case .storage, .review: draft.stage = .hardware
            default: break
            }
            selectOnlyMedia()
            removeAbandonedStaging(in: store.root)
            if draft.stage == .prepared {
                store.cleanupFirmwareCopies()
                if let driver = draft.driverID { try? FileManager.default.removeItem(at: store.driversURL(for: driver)) }
                if let media = draft.mediaID { try? FileManager.default.removeItem(at: store.installerURL(for: draft, media: media)) }
            }
            loaded = true
            updateFirmwareState()
            freeDiskBytes = try URL.applicationSupportDirectory.resourceValues(forKeys: [.volumeAvailableCapacityKey]).volumeAvailableCapacity
        } catch {
            self.error = error.localizedDescription
        }
    }

    private func selectOnlyMedia() {
        if draft.mediaID == nil, shelf.items.count == 1 { draft.mediaID = shelf.items.first?.id }
    }

    private func advance() {
        switch draft.stage {
        case .hardware, .storage, .review, .preparing:
            prepare()
        case .prepared:
            do {
                try store.save(InstallationDraft())
                dismiss()
            } catch {
                self.error = error.localizedDescription
            }
        default:
            advance(to: .hardware)
        }
    }

    private func advance(to stage: InstallationDraft.Stage?) {
        guard let stage else { return }
        error = nil
        draft.lastError = nil
        draft.stage = stage
    }

    private func choose(_ selection: InstallationFile) {
        fileSelection = selection
        pickerRequest = DocumentPickerRequest(kind: selection == .drivers ? .drivers : .firmware)
    }

    private func updateFirmwareState() {
        hasCode = FileManager.default.fileExists(atPath: store.code.path)
        hasVars = FileManager.default.fileExists(atPath: store.variables.path)
    }

    private func copyFirmware(_ selection: ImportSelection, file: InstallationFile?) {
        let store = store
        saving = true
        error = nil
        Task {
            defer { saving = false; importProgress = nil }
            do {
                let report = progressReporter("Copying firmware")
                try await Task.detached(priority: .userInitiated) {
                    try VMFiles.withFileAccess(selection, kind: .firmware) { url in
                        let isVariables: Bool
                        if let file {
                            isVariables = file == .variables
                        } else {
                            isVariables = (try? VMRepository.validateFirmware(url, isVariables: true)) != nil
                        }
                        try store.importFirmware(url, destination: isVariables ? store.variables : store.code, progress: report)
                    }
                }.value
                updateFirmwareState()
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    private func importDrivers(_ selection: ImportSelection) {
        let id = UUID()
        let store = store
        let destination = store.driversURL(for: id)
        let previous = draft.driverID
        saving = true
        error = nil
        Task {
            defer { saving = false; importProgress = nil }
            do {
                let report = progressReporter("Copying driver package")
                try await Task.detached(priority: .userInitiated) {
                    try VMFiles.withFileAccess(selection, kind: .drivers) { folder in
                        try NetKVMDrivers.stage(from: folder, to: destination, progress: report)
                    }
                }.value
                var updated = draft
                updated.driverID = id
                try store.save(updated)
                draft = updated
                if let previous { try? FileManager.default.removeItem(at: store.driversURL(for: previous)) }
            } catch {
                try? FileManager.default.removeItem(at: destination)
                self.error = error.localizedDescription
            }
        }
    }

    private func progressReporter(_ label: String) -> @Sendable (UInt64, UInt64) -> Void {
        { bytes, total in
            Task { @MainActor in
                if saving { importProgress = ImportProgress(label: label, bytes: bytes, total: total) }
            }
        }
    }

    private func clearDriverSelection() {
        guard let previous = draft.driverID else { return }
        do {
            var updated = draft
            updated.driverID = nil
            try store.save(updated)
            draft = updated
            try? FileManager.default.removeItem(at: store.driversURL(for: previous))
        } catch {
            self.error = error.localizedDescription
        }
    }

    private func prepare() {
        guard !shelf.busy else {
            error = "Wait for the current media operation to finish."
            return
        }
        guard let media = draft.mediaID, let source = selectedMedia else {
            error = "Select a verified Windows 11 24H2 ARM64 ISO."
            if draft.stage == .preparing { draft.stage = .media }
            return
        }
        let store = store
        let repository = repository
        let iso = shelf.repository.isoURL(for: media)
        let stageDisk = store.installerURL(for: draft, media: media)
        let customDrivers = draft.driverID.map { store.driversURL(for: $0) }
        do {
            try draft.validateForPreparation()
            var preparing = draft
            preparing.stage = .preparing
            preparing.lastError = nil
            try store.save(preparing)
            draft = preparing
            saving = true
            installerProgress = nil
            error = nil
            Task {
                defer { saving = false; installerProgress = nil }
                do {
                    let input = preparing
                    let progress = $installerProgress
                    let work = Task.detached(priority: .userInitiated) {
                        let assets = InstallerAssets.bundled
                        let firmware = try store.firmware(for: input, assets: assets)
                        let drivers = try store.driverDirectory(for: input, assets: assets)
                        try InstallerPreparation.build(iso: iso, output: stageDisk, expectedSHA256: source.savedChecksum,
                                                       drivers: drivers, edition: input.editionIndex) { status in
                            Task { @MainActor in progress.wrappedValue = status }
                        }
                        try Task.checkCancellation()
                        return try store.prepare(input, repository: repository, firmware: firmware, installer: stageDisk)
                    }
                    preparationTask = work
                    defer { preparationTask = nil }
                    _ = try await work.value
                    var complete = input
                    complete.stage = .prepared
                    try store.save(complete)
                    draft = complete
                    store.cleanupFirmwareCopies()
                    if let customDrivers { try? FileManager.default.removeItem(at: customDrivers) }
                    try? FileManager.default.removeItem(at: stageDisk)
                } catch {
                    draft.stage = .hardware
                    draft.lastError = error.localizedDescription
                }
            }
        } catch {
            self.error = error.localizedDescription
        }
    }
}
