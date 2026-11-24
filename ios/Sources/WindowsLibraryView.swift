// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers
import UIKit

struct WindowsLibraryView: View {
    let vm: VMViewModel

    var body: some View {
        TabView {
            MachineLibraryView(vm: vm)
                .tabItem { Label("Virtual Machines", systemImage: "desktopcomputer") }
            NavigationStack {
                MediaLibraryView()
            }
            .tabItem { Label("Downloads", systemImage: "arrow.down.circle") }
            NavigationStack {
                JITSettingsView(vm: vm)
            }
            .tabItem { Label("Settings", systemImage: "gearshape") }
            NavigationStack {
                DiagnosticsView(vm: vm)
            }
            .tabItem { Label("Diagnostics", systemImage: "waveform.path.ecg") }
        }
    }
}

private struct MachineLibraryView: View {
    @ObservedObject var vm: VMViewModel
    @State private var machines: [VirtualMachine] = []
    @State private var activeMachine: VirtualMachine?
    @State private var editor: VirtualMachine?
    @State private var deleteCandidate: VirtualMachine?
    @State private var export: VMExport?
    @State private var showSession = false
    @State private var showAssistant = false
    @State private var showImporter = false
    @State private var busy = false
    @State private var hasLegacy = false
    @State private var error: String?
    @Environment(\.scenePhase) private var scenePhase

    private let repo = VMFiles.repository

    var body: some View {
        NavigationStack {
            List {
                if sessionBusy {
                    Button { showSession = true } label: {
                        Label(activeMachine?.name ?? "Windows 11", systemImage: "play.rectangle")
                    }
                    .accessibilityHint("Return to the active virtual machine")
                }
                if !sessionBusy, let activeMachine, !vm.needsRelaunchToStart {
                    Section {
                        Text(vm.vmStatusText).foregroundStyle(.secondary)
                    } header: {
                        Text(activeMachine.name)
                    }
                }
                if vm.needsRelaunchToStart {
                    Section {
                        Label("Reopen the app to start a new session.", systemImage: "arrow.clockwise")
                    }
                }
                if busy {
                    ProgressView("Working with machine files…")
                }
                ForEach(machines) { machine in
                    machineRow(machine)
                }
                if hasLegacy {
                    Section {
                        Button { start(nil) } label: {
                            Label("Windows 11", systemImage: "desktopcomputer")
                        }
                        .disabled(sessionBusy || busy || vm.needsRelaunchToStart)
                    } header: {
                        Text("Existing installation")
                    } footer: {
                        Text("Uses your prepared disk and firmware in Files.")
                    }
                }
            }
            .overlay {
                if machines.isEmpty && !hasLegacy && !busy && !sessionBusy {
                    ContentUnavailableView {
                        Label("No virtual machines", systemImage: "desktopcomputer")
                    } description: {
                        Text("Prepare a new virtual disk or import a folder with a prepared Windows machine.")
                    } actions: {
                        Button("Create Virtual Machine") { showAssistant = true }
                            .buttonStyle(.borderedProminent)
                        Button("Import Folder") { showImporter = true }
                    }
                }
            }
            .navigationTitle("Virtual Machines")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Menu {
                        Button("Create Virtual Machine", systemImage: "plus") { showAssistant = true }
                        Button("Import Folder", systemImage: "folder") { showImporter = true }
                    } label: {
                        Label("Add Virtual Machine", systemImage: "plus")
                    }
                    .disabled(busy || sessionBusy)
                }
            }
            .refreshable { refresh() }
            .onAppear { refresh() }
            .onChange(of: scenePhase) { _, newPhase in
                if newPhase == .active { refresh() }
            }
            .fileImporter(isPresented: $showImporter, allowedContentTypes: [.folder]) { result in
                switch result {
                case .success(let url):
                    doFileWork {
                        try VMFiles.withFileAccess(url) { url in
                            _ = try repo.importPrepared(from: url, copyDisk: VMFiles.copyDisk)
                        }
                    }
                case .failure(let failure):
                    error = failure.localizedDescription
                }
            }
            .sheet(item: $editor) { machine in
                MachineEditor(machine: machine, directory: repo.directory(for: machine)) { edited in
                    try repo.update(edited)
                    refresh()
                }
            }
            .sheet(isPresented: $showAssistant, onDismiss: refresh) {
                InstallationAssistant(vm: vm)
            }
            .sheet(item: $export, onDismiss: cleanExports) { item in
                FolderExporter(url: item.folder)
            }
            .fullScreenCover(isPresented: $showSession) {
                SessionView(vm: vm)
                    .preferredColorScheme(.dark)
                    .overlay(alignment: .topLeading) {
                        Button {
                            showSession = false
                        } label: {
                            Label("Library", systemImage: "chevron.backward")
                                .padding(6)
                        }
                        .buttonStyle(.borderedProminent)
                        .padding()
                    }
            }
            .confirmationDialog("Delete virtual machine?", isPresented: deletionPresented, presenting: deleteCandidate) { machine in
                Button("Delete \(machine.name)", role: .destructive) {
                    deleteCandidate = nil
                    guard !sessionBusy, !busy else { return }
                    doFileWork { try repo.remove(machine) }
                }
            } message: { machine in
                Text("This permanently deletes the managed disk and firmware state for \(machine.name).")
            }
            .alert("Virtual Machine Files", isPresented: errorPresented) {
                Button("OK", role: .cancel) { error = nil }
            } message: {
                Text(error ?? "")
            }
        }
    }

    private var sessionBusy: Bool { vm.isVMRunning || vm.isUnpacking }

    private var deletionPresented: Binding<Bool> {
        Binding(get: { deleteCandidate != nil }, set: { if !$0 { deleteCandidate = nil } })
    }

    private var errorPresented: Binding<Bool> {
        Binding(get: { error != nil }, set: { if !$0 { error = nil } })
    }

    private func machineRow(_ machine: VirtualMachine) -> some View {
        HStack {
            Label {
                VStack(alignment: .leading, spacing: 4) {
                    Text(machine.name).font(.headline)
                    Text(machine.installation == .blankDisk ? "Awaiting Windows installation" : "Prepared Windows disk")
                        .font(.subheadline).foregroundStyle(.secondary)
                }
            } icon: {
                Image(systemName: "desktopcomputer").foregroundStyle(.tint)
            }
            Spacer()
            Menu {
                Button("Start", systemImage: "play") { start(machine) }
                    .disabled(machine.installation != .preparedDisk || sessionBusy || vm.needsRelaunchToStart)
                if activeMachine?.id == machine.id && vm.isVMRunning {
                    Button("Send Power Button", systemImage: "power") { WinIOSBridge.shared.pressPowerButton() }
                }
                Button("Edit Configuration", systemImage: "slider.horizontal.3") { editor = machine }
                    .disabled(sessionBusy)
                Button("Duplicate", systemImage: "plus.square.on.square") {
                    doFileWork { _ = try repo.duplicate(machine, copyDisk: VMFiles.copyDisk) }
                }
                .disabled(sessionBusy)
                Button("Export to Files", systemImage: "square.and.arrow.up") { exportMachine(machine) }
                    .disabled(sessionBusy)
                Button("Delete", systemImage: "trash", role: .destructive) { deleteCandidate = machine }
                    .disabled(sessionBusy)
            } label: {
                Label("Actions for \(machine.name)", systemImage: "ellipsis.circle")
                    .labelStyle(.iconOnly)
                    .font(.title3)
            }
            .disabled(busy)
        }
        .padding(.vertical, 5)
    }

    private func start(_ machine: VirtualMachine?) {
        guard !sessionBusy, !busy, !vm.needsRelaunchToStart else { return }
        activeMachine = machine
        vm.open(directory: machine.map { repo.directory(for: $0) }, hardware: machine?.hardware ?? VMHardware())
        showSession = true
    }

    private func refresh() {
        hasLegacy = WinIOSBridge.shared.hasProvisionedMachine()
        do {
            try FileManager.default.createDirectory(at: repo.root, withIntermediateDirectories: true)
            machines = try repo.list()
        } catch {
            self.error = error.localizedDescription
        }
    }

    private func doFileWork(_ action: @escaping @Sendable () throws -> Void) {
        guard !busy, !sessionBusy else { return }
        busy = true
        Task {
            defer { busy = false }
            do {
                try await Task.detached(priority: .userInitiated, operation: action).value
                refresh()
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    private static var exportDirectory: URL { URL.temporaryDirectory.appendingPathComponent("VMExport") }

    private func exportMachine(_ machine: VirtualMachine) {
        guard !busy, !sessionBusy else { return }
        busy = true
        let workspace = Self.exportDirectory
        let repository = repo
        Task {
            defer { busy = false }
            do {
                let folder = try await Task.detached(priority: .userInitiated) {
                    let fileManager = FileManager.default
                    try? fileManager.removeItem(at: workspace)
                    try fileManager.createDirectory(at: workspace, withIntermediateDirectories: true)
                    return try repository.export(machine, to: workspace, copyDisk: VMFiles.exportDisk)
                }.value
                export = VMExport(folder: folder)
            } catch {
                cleanExports()
                self.error = error.localizedDescription
            }
        }
    }

    private func cleanExports() {
        try? FileManager.default.removeItem(at: Self.exportDirectory)
    }
}

private struct VMExport: Identifiable {
    let folder: URL
    var id: URL { folder }
}

private struct FolderExporter: UIViewControllerRepresentable {
    let url: URL

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        UIDocumentPickerViewController(forExporting: [url], asCopy: true)
    }

    func updateUIViewController(_ controller: UIDocumentPickerViewController, context: Context) {}
}

private struct MachineEditor: View {
    @Environment(\.dismiss) private var dismiss
    @State private var draft: VirtualMachine
    @State private var error: String?
    @State private var diskSize: URLResourceValues?
    let directory: URL
    let save: (VirtualMachine) throws -> Void

    init(machine: VirtualMachine, directory: URL, save: @escaping (VirtualMachine) throws -> Void) {
        _draft = State(initialValue: machine)
        self.directory = directory
        self.save = save
    }

    var body: some View {
        NavigationStack {
            Form {
                TextField("Machine name", text: $draft.name)
                HardwareFields(hardware: $draft.hardware)
                Section("Storage") {
                    if let size = diskSize?.fileSize {
                        LabeledContent("Virtual capacity", value: ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .binary))
                    }
                    if let allocated = diskSize?.fileAllocatedSize {
                        LabeledContent("Allocated on device", value: ByteCountFormatter.string(fromByteCount: Int64(allocated), countStyle: .file))
                    }
                }
                if let error { Text(error).foregroundStyle(.red) }
            }
            .navigationTitle("Edit Virtual Machine")
            .task {
                let rawDisk = directory.appendingPathComponent(VMRepository.rawDisk)
                diskSize = try? rawDisk.resourceValues(forKeys: [.fileSizeKey, .fileAllocatedSizeKey])
            }
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        do {
                            try save(draft)
                            dismiss()
                        } catch {
                            self.error = error.localizedDescription
                        }
                    }
                }
            }
        }
    }
}

struct HardwareFields: View {
    @Binding var hardware: VMHardware
    private let maximumJITMiB = WinIOSBridge.shared.maximumJITMiB

    var body: some View {
        Section {
            Picker("Memory preset", selection: $hardware.preset) {
                ForEach(HardwarePreset.allCases) { preset in
                    Text(preset.title).tag(preset)
                }
            }
            if hardware.preset == .custom {
                TextField("Guest RAM (MiB)", value: $hardware.customRAMMiB, format: .number)
                    .keyboardType(.numberPad)
                TextField("JIT cache (MiB)", value: $hardware.customJITMiB, format: .number)
                    .keyboardType(.numberPad)
            } else if let preset = hardware.preset.settings {
                LabeledContent("Guest RAM", value: "\(preset.ramMiB) MiB")
                LabeledContent("JIT cache", value: "\(preset.jitMiB) MiB")
            }
            Stepper("Virtual CPUs · \(hardware.vCPUs)", value: $hardware.vCPUs, in: 1...4)
        } header: {
            Text("Hardware")
        } footer: {
            if hardware.jitMiB > Int(maximumJITMiB) {
                Text("This preset is saved for testing. The current engine refuses to start with a JIT cache above \(maximumJITMiB) MiB.")
            } else if hardware.preset == .automatic {
                Text("Automatic uses the original device-based memory allocation.")
            }
        }
    }
}
