// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

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
    @State private var showFirmwarePicker = false
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
                    Label(draft.stage.title, systemImage: "desktopcomputer")
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
                    if draft.stage != .welcome && draft.stage.rawValue <= InstallationDraft.Stage.review.rawValue {
                        Button("Back", systemImage: "chevron.backward") {
                            advance(to: InstallationDraft.Stage(rawValue: draft.stage.rawValue - 1))
                        }
                    }
                }
            }
            .task { load() }
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
            .fileImporter(isPresented: $showFirmwarePicker, allowedContentTypes: fileSelection == .drivers ? [.folder] : [.data]) { result in
                switch result {
                case .success(let url):
                    if fileSelection == .drivers { importDrivers(url) } else { copyFirmware(url) }
                case .failure(let error): self.error = error.localizedDescription
                }
            }
            .interactiveDismissDisabled(saving)
        }
    }

    @ViewBuilder private var stageContents: some View {
        switch draft.stage {
        case .welcome:
            Text("Prepare a virtual disk and writable firmware for Windows 11 24H2 ARM64.")
            Text("You can also import a folder containing an existing Windows installation from the library.")
                .foregroundStyle(.secondary)
        case .device:
            Section("Host") {
                LabeledContent("Physical memory", value: "\(ProcessInfo.processInfo.physicalMemory >> 20) MiB")
                LabeledContent("Available process memory", value: "\(vm.availableMemoryMB) MiB")
                #if targetEnvironment(simulator)
                Text("Windows sessions require a physical ARM64 iPhone or iPad.")
                #else
                Text("Guest boot requires executable JIT and memory available to the signed app.")
                #endif
            }
        case .jit:
            JITStatusSection(vm: vm)
            Text("You can prepare files while JIT is unavailable. Launching a guest requires the executable self-test.")
                .foregroundStyle(.secondary)
        case .media:
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
        case .hardware:
            TextField("Name", text: $draft.name)
            HardwareFields(hardware: $draft.hardware)
        case .storage:
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
        case .review:
            Section("Configuration") {
                LabeledContent("Name", value: draft.name)
                LabeledContent("Virtual capacity", value: "\(draft.capacityGiB) GiB")
                LabeledContent("Memory preset", value: draft.hardware.preset.title)
                if let media = shelf.items.first(where: { $0.id == draft.mediaID }) {
                    LabeledContent("Windows ISO", value: media.name)
                }
            }
            Section("EDK2 firmware") {
                Button(hasCode ? "Replace code.fd" : "Choose code.fd", systemImage: hasCode ? "checkmark.circle" : "doc") {
                    choose(.code)
                }
                Button(hasVars ? "Replace vars.fd" : "Choose vars.fd", systemImage: hasVars ? "checkmark.circle" : "doc") {
                    choose(.variables)
                }
                Text("Choose compatible ARM64 ArmVirtQemu firmware and its matching variables template. Each VM gets its own writable copy.")
                    .foregroundStyle(.secondary)
            }
            Section("Network driver") {
                Button(draft.driverID == nil ? "Choose ARM64 NetKVM Folder" : "Replace NetKVM Folder", systemImage: "folder") {
                    choose(.drivers)
                }
                if draft.driverID != nil {
                    Button("Clear Driver Selection", role: .destructive) { clearDriverSelection() }
                }
                Text("Windows Setup checks the optional driver package’s signature. Choose the Windows 11 ARM64 folder from a trusted virtio-win release.")
                    .foregroundStyle(.secondary)
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
                ProgressView("Preparing firmware and storage…")
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
        }
    }

    private var selectedMedia: WindowsMedia? { shelf.items.first { $0.id == draft.mediaID } }
    private var hasSelectedMedia: Bool { selectedMedia != nil }

    private var continueLabel: String {
        switch draft.stage {
        case .review: "Prepare"
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

    private func advance() {
        switch draft.stage {
        case .review, .preparing:
            prepare()
        case .prepared:
            do {
                try store.save(InstallationDraft())
                dismiss()
            } catch {
                self.error = error.localizedDescription
            }
        default:
            advance(to: InstallationDraft.Stage(rawValue: draft.stage.rawValue + 1))
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
        showFirmwarePicker = true
    }

    private func updateFirmwareState() {
        hasCode = FileManager.default.fileExists(atPath: store.code.path)
        hasVars = FileManager.default.fileExists(atPath: store.variables.path)
    }

    private func copyFirmware(_ url: URL) {
        let store = store
        let destination = fileSelection == .code ? store.code : store.variables
        saving = true
        error = nil
        Task {
            defer { saving = false }
            do {
                try await Task.detached(priority: .userInitiated) {
                    try VMFiles.withFileAccess(url) { url in
                        try store.importFirmware(url, destination: destination)
                    }
                }.value
                updateFirmwareState()
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    private func importDrivers(_ url: URL) {
        let id = UUID()
        let store = store
        let destination = store.driversURL(for: id)
        let previous = draft.driverID
        saving = true
        error = nil
        Task {
            defer { saving = false }
            do {
                try await Task.detached(priority: .userInitiated) {
                    try VMFiles.withFileAccess(url) { folder in
                        try NetKVMDrivers.stage(from: folder, to: destination)
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
        guard let media = draft.mediaID, hasSelectedMedia, hasCode, hasVars else {
            error = "Select a verified Windows ISO and both firmware files before preparing this machine."
            if draft.stage == .preparing { draft.stage = .review }
            return
        }
        let store = store
        let repository = repository
        let iso = shelf.repository.isoURL(for: media)
        let stageDisk = store.installerURL(for: draft, media: media)
        let drivers = draft.driverID.map { store.driversURL(for: $0) }
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
                        try InstallerPreparation.build(iso: iso, output: stageDisk, drivers: drivers, edition: input.editionIndex) { status in
                            Task { @MainActor in progress.wrappedValue = status }
                        }
                        try Task.checkCancellation()
                        return try store.prepare(input, repository: repository, installer: stageDisk)
                    }
                    preparationTask = work
                    defer { preparationTask = nil }
                    _ = try await work.value
                    var complete = input
                    complete.stage = .prepared
                    try store.save(complete)
                    draft = complete
                    store.cleanupFirmwareCopies()
                    if let drivers { try? FileManager.default.removeItem(at: drivers) }
                    try? FileManager.default.removeItem(at: stageDisk)
                } catch {
                    draft.stage = .review
                    draft.lastError = error.localizedDescription
                }
            }
        } catch {
            self.error = error.localizedDescription
        }
    }
}
