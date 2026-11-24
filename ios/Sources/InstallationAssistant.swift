// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

struct InstallationAssistant: View {
    enum FirmwareSelection { case code, variables }

    let vm: VMViewModel
    @Environment(\.dismiss) private var dismiss
    @State private var shelf = MediaShelf.shared
    @State private var draft = InstallationDraft()
    @State private var loaded = false
    @State private var saving = false
    @State private var showMedia = false
    @State private var showFirmwarePicker = false
    @State private var firmwareSelection: FirmwareSelection = .code
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
            .fileImporter(isPresented: $showFirmwarePicker, allowedContentTypes: [.data]) { result in
                switch result {
                case .success(let url): copyFirmware(url)
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
        case .preparing:
            if saving {
                ProgressView("Creating the sparse disk and copying firmware…")
            } else {
                Text("Preparation was interrupted. Retry to finish publishing the virtual machine.")
            }
        case .prepared:
            Label("Sparse disk and firmware saved", systemImage: "checkmark.circle")
            Text("This machine is awaiting Windows installation. On-device installer-disk creation and WinPE provisioning are still required before it can boot.")
                .foregroundStyle(.secondary)
        }
    }

    private var hasSelectedMedia: Bool { shelf.items.contains { $0.id == draft.mediaID } }

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
            if draft.stage == .prepared { store.cleanupFirmwareCopies() }
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

    private func choose(_ selection: FirmwareSelection) {
        firmwareSelection = selection
        showFirmwarePicker = true
    }

    private func updateFirmwareState() {
        hasCode = FileManager.default.fileExists(atPath: store.code.path)
        hasVars = FileManager.default.fileExists(atPath: store.variables.path)
    }

    private func copyFirmware(_ url: URL) {
        let store = store
        let destination = firmwareSelection == .code ? store.code : store.variables
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

    private func prepare() {
        guard !shelf.busy else {
            error = "Wait for the current media operation to finish."
            return
        }
        guard hasSelectedMedia, hasCode, hasVars else {
            error = "Select a verified Windows ISO and both firmware files before preparing this machine."
            if draft.stage == .preparing { draft.stage = .review }
            return
        }
        let store = store
        let repository = repository
        do {
            try draft.validateForPreparation()
            var preparing = draft
            preparing.stage = .preparing
            preparing.lastError = nil
            try store.save(preparing)
            draft = preparing
            saving = true
            error = nil
            Task {
                defer { saving = false }
                do {
                    let input = preparing
                    _ = try await Task.detached(priority: .userInitiated) { try store.prepare(input, repository: repository) }.value
                    var complete = input
                    complete.stage = .prepared
                    try store.save(complete)
                    draft = complete
                    store.cleanupFirmwareCopies()
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
