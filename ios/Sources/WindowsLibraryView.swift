// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
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
        .task { ImportInboxModel.shared.refresh() }
    }
}

private struct MachineLibraryView: View {
    @ObservedObject var vm: VMViewModel
    @State private var machines: [VirtualMachine] = []
    @State private var activeMachine: VirtualMachine?
    @State private var editor: VirtualMachine?
    @State private var deleteCandidate: VirtualMachine?
    @State private var installedCandidate: VirtualMachine?
    @State private var showSession = false
    @State private var showAssistant = false
    @State private var setupAfterAssistant: VirtualMachine?
    @State private var pickerRequest: DocumentPickerRequest?
    @State private var importProgress: ImportProgress?
    @State private var busy = false
    @State private var hasLegacy = false
    @State private var error: String?
    @State private var localBackup: String?
    @Environment(\.scenePhase) private var scenePhase

    private let repo = VMFiles.repository

    var body: some View {
        NavigationStack {
            machineConfirmations
                .alert("Local Backup Saved", isPresented: Binding(
                    get: { localBackup != nil }, set: { if !$0 { localBackup = nil } }
                )) {
                    Button("OK", role: .cancel) { localBackup = nil }
                } message: {
                    Text(localBackup ?? "")
                }
                .alert("Virtual Machine Files", isPresented: errorPresented) {
                    Button("OK", role: .cancel) { error = nil }
                } message: {
                    Text(error ?? "")
                }
        }
    }

    private var machineList: some View {
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
                if let importProgress { ImportProgressView(progress: importProgress) }
                else { ProgressView("Working with machine files…") }
            }
            if machines.isEmpty && !hasLegacy && !busy && !sessionBusy {
                ContentUnavailableView {
                    Label("No virtual machines", systemImage: "desktopcomputer")
                } description: {
                    Text("Prepare a new virtual disk or import a folder with a prepared Windows machine.")
                } actions: {
                    Button("Create Virtual Machine") { showAssistant = true }
                        .buttonStyle(.borderedProminent)
                }
            }
            ForEach(machines) { machine in
                machineRow(machine)
            }
            ImportInboxSection(kinds: [.machine, .disk], disabled: busy || sessionBusy) { entry in
                importMachine(kind: entry.kind, selection: ImportSelection(entry.url))
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
        .navigationTitle("Virtual Machines")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Menu {
                    Button("Create Virtual Machine", systemImage: "plus") { showAssistant = true }
                    Button("Import Folder", systemImage: "folder") { pickerRequest = DocumentPickerRequest(kind: .machine) }
                    Button("Import Disk Image", systemImage: "externaldrive") { pickerRequest = DocumentPickerRequest(kind: .disk) }
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
    }

    private var machinePresentations: some View {
        machineList
            .documentPicker($pickerRequest, exported: { _ in cleanExports() }, selected: importMachine)
            .sheet(item: $editor) { machine in
                MachineEditor(machine: machine, directory: repo.directory(for: machine)) { edited in
                    try repo.update(edited)
                    refresh()
                }
            }
            .sheet(isPresented: $showAssistant, onDismiss: closeAssistant) {
                InstallationAssistant(vm: vm) { setupAfterAssistant = $0 }
            }
            .fullScreenCover(isPresented: $showSession) { machineSession }
    }

    private var machineConfirmations: some View {
        machinePresentations
            .confirmationDialog("Delete virtual machine?", isPresented: deletionPresented, presenting: deleteCandidate) { machine in
                Button("Delete \(machine.name)", role: .destructive) {
                    deleteCandidate = nil
                    guard !sessionBusy, !busy else { return }
                    doFileWork { try repo.remove(machine) }
                }
            } message: { machine in
                Text("This permanently deletes the managed disk and firmware state for \(machine.name).")
            }
            .confirmationDialog("Use the installed Windows disk?", isPresented: installedPresented, presenting: installedCandidate) { machine in
                Button("Use Installed Disk and Remove Setup Disk") {
                    installedCandidate = nil
                    adoptInstalledDisk(machine, removeInstaller: true)
                }
                Button("Use Installed Disk and Keep Setup Disk") {
                    installedCandidate = nil
                    adoptInstalledDisk(machine, removeInstaller: false)
                }
            } message: { _ in
                Text("Continue after Windows Setup has finished and the guest has shut down. The primary disk is checked for ARM64 Windows boot files before switching.")
            }
    }

    private var machineSession: some View {
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

    private var sessionBusy: Bool { vm.isVMRunning || vm.isUnpacking }

    private var deletionPresented: Binding<Bool> {
        Binding(get: { deleteCandidate != nil }, set: { if !$0 { deleteCandidate = nil } })
    }

    private var installedPresented: Binding<Bool> {
        Binding(get: { installedCandidate != nil }, set: { if !$0 { installedCandidate = nil } })
    }

    private var errorPresented: Binding<Bool> {
        Binding(get: { error != nil }, set: { if !$0 { error = nil } })
    }

    private func machineRow(_ machine: VirtualMachine) -> some View {
        let installer = repo.directory(for: machine).appendingPathComponent(VMRepository.installerDisk)
        let hasInstaller = FileManager.default.fileExists(atPath: installer.path)
        return HStack {
            Label {
                VStack(alignment: .leading, spacing: 4) {
                    Text(machine.name).font(.headline)
                    Text(machine.installation == .preparedDisk ? "Prepared Windows disk"
                         : hasInstaller ? "Windows Setup disk ready" : "Awaiting Windows installation")
                        .font(.subheadline).foregroundStyle(.secondary)
                }
            } icon: {
                Image(systemName: "desktopcomputer").foregroundStyle(.tint)
            }
            Spacer()
            Menu {
                if machine.installation == .preparedDisk {
                    Button("Start", systemImage: "play") { start(machine) }
                        .disabled(sessionBusy || vm.needsRelaunchToStart)
                } else {
                    if hasInstaller {
                        Button("Start Windows Setup", systemImage: "play") { start(machine, installer: installer) }
                            .disabled(sessionBusy || vm.needsRelaunchToStart)
                    }
                    if let mediaID = machine.mediaID {
                        Button(hasInstaller ? "Update Windows Setup Disk" : "Build Windows Setup Disk", systemImage: "opticaldisc") {
                            buildInstaller(for: mediaID, output: installer)
                        }
                        .disabled(sessionBusy)
                    }
                    Button("Use Installed Windows Disk", systemImage: "checkmark.circle") { installedCandidate = machine }
                        .disabled(sessionBusy)
                }
                if FileManager.default.fileExists(atPath: repo.directory(for: machine)
                    .appendingPathComponent(VMRepository.setupLogs).path) {
                    Button("Export Windows Setup Logs", systemImage: "square.and.arrow.up") { exportSetupLogs(machine) }
                        .disabled(sessionBusy)
                }
                if activeMachine?.id == machine.id && vm.isVMRunning {
                    Button("Send Power Button", systemImage: "power") { WinIOSBridge.shared.pressPowerButton() }
                }
                Button("Edit Configuration", systemImage: "slider.horizontal.3") { editor = machine }
                    .disabled(sessionBusy)
                Button("Duplicate", systemImage: "plus.square.on.square") {
                    doFileWork { _ = try repo.duplicate(machine, copyDisk: VMFiles.copyDisk) }
                }
                .disabled(sessionBusy)
                Button(shouldKeepBackupLocal(machine) ? "Save Local VM Backup" : "Export to Files",
                       systemImage: "square.and.arrow.up") { exportMachine(machine) }
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

    private func closeAssistant() {
        refresh()
        if let machine = setupAfterAssistant {
            setupAfterAssistant = nil
            let installer = repo.directory(for: machine).appendingPathComponent(VMRepository.installerDisk)
            start(machine, installer: installer)
        }
    }

    private func start(_ machine: VirtualMachine?, installer: URL? = nil) {
        guard !sessionBusy, !busy, !vm.needsRelaunchToStart else { return }
        activeMachine = machine
        vm.open(directory: machine.map { repo.directory(for: $0) }, hardware: machine?.hardware ?? VMHardware(),
                installer: installer, useTPM: machine?.usesTPM == true)
        showSession = true
    }

    private func buildInstaller(for mediaID: UUID, output: URL) {
        let shelf = MediaShelf.shared
        let iso = shelf.repository.isoURL(for: mediaID)
        let checksum = shelf.items.first { $0.id == mediaID }?.savedChecksum
        doFileWork {
            let drivers = InstallerAssets.bundled.drivers
            try NetKVMDrivers.validate(from: drivers)
            let manager = FileManager.default
            let workspace = output.deletingLastPathComponent()
                .appendingPathComponent(".setup-update-\(UUID().uuidString)", isDirectory: true)
            try manager.createDirectory(at: workspace, withIntermediateDirectories: false)
            defer { try? manager.removeItem(at: workspace) }
            let updated = workspace.appendingPathComponent(output.lastPathComponent)
            try InstallerPreparation.build(iso: iso, output: updated, expectedSHA256: checksum, drivers: drivers) { _ in }
            if manager.fileExists(atPath: output.path) {
                let backup = output.deletingLastPathComponent()
                    .appendingPathComponent("Setup Backup-\(UUID().uuidString).img")
                try manager.linkItem(at: output, to: backup)
                _ = try manager.replaceItemAt(output, withItemAt: updated)
            } else {
                try manager.moveItem(at: updated, to: output)
            }
        }
    }

    private func adoptInstalledDisk(_ machine: VirtualMachine, removeInstaller: Bool) {
        let repository = repo
        let folder = repository.directory(for: machine)
        doFileWork {
            let disk = folder.appendingPathComponent(VMRepository.rawDisk)
            try InstallerPreparation.verifyWindowsBootFiles(disk)
            var installed = machine
            installed.installation = .preparedDisk
            let state = folder.appendingPathComponent(VMRepository.tpmState).appendingPathComponent("permall")
            if installed.usesTPM == true || FileManager.default.fileExists(atPath: state.path) {
                _ = try regularFileSize(state)
                installed.usesTPM = true
            }
            try repository.update(installed)
            if removeInstaller {
                let installer = folder.appendingPathComponent(VMRepository.installerDisk)
                if FileManager.default.fileExists(atPath: installer.path) {
                    try FileManager.default.removeItem(at: installer)
                }
            }
        }
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

    private func importMachine(kind: DocumentImportKind, selection: ImportSelection) {
        let repository = repo
        let report: @Sendable (UInt64, UInt64) -> Void = { bytes, total in
            Task { @MainActor in
                if busy { importProgress = ImportProgress(label: "Copying disk image", bytes: bytes, total: total) }
            }
        }
        doFileWork {
            try VMFiles.withFileAccess(selection, kind: kind) { url in
                if kind == .disk {
                    _ = try repository.importDisk(from: url, firmware: InstallerAssets.bundled.firmware) { input, output in
                        try VMFiles.importDisk(from: input, to: output, progress: report)
                    }
                } else {
                    _ = try repository.importPrepared(from: url) { input, output in
                        try VMFiles.copyMachineDisk(from: input, to: output, progress: report)
                    }
                }
            }
        }
    }

    private func doFileWork(_ action: @escaping @Sendable () throws -> Void) {
        guard !busy, !sessionBusy else { return }
        busy = true
        Task {
            defer { busy = false; importProgress = nil }
            do {
                try await Task.detached(priority: .userInitiated, operation: action).value
                refresh()
            } catch {
                refresh()
                self.error = error.localizedDescription
            }
        }
    }

    private static var exportDirectory: URL { URL.temporaryDirectory.appendingPathComponent("VMExport") }

    private func shouldKeepBackupLocal(_ machine: VirtualMachine) -> Bool {
        machine.usesTPM == true || FileManager.default.fileExists(
            atPath: repo.directory(for: machine).appendingPathComponent(VMRepository.tpmState).path
        )
    }

    private func exportMachine(_ machine: VirtualMachine) {
        guard !busy, !sessionBusy else { return }
        busy = true
        let local = shouldKeepBackupLocal(machine)
        let backupID = UUID().uuidString
        let workspace = local
            ? URL.documentsDirectory.appendingPathComponent("Local Backups").appendingPathComponent(backupID)
            : Self.exportDirectory
        let repository = repo
        Task {
            defer { busy = false }
            do {
                let folder = try await Task.detached(priority: .userInitiated) {
                    let fileManager = FileManager.default
                    if !local { try? fileManager.removeItem(at: workspace) }
                    try fileManager.createDirectory(at: workspace, withIntermediateDirectories: true)
                    if local {
                        var localFolder = workspace
                        var values = URLResourceValues()
                        values.isExcludedFromBackup = true
                        try localFolder.setResourceValues(values)
                    }
                    return try repository.export(machine, to: workspace, copyDisk: VMFiles.exportDisk)
                }.value
                if local {
                    localBackup = "Find the VM in Files under On My iPhone or On My iPad → WindowsiOS-ez → Local Backups → \(backupID). Its TPM state is part of the local backup."
                } else {
                    pickerRequest = DocumentPickerRequest(exporting: folder)
                }
            } catch {
                if local { try? FileManager.default.removeItem(at: workspace) } else { cleanExports() }
                self.error = error.localizedDescription
            }
        }
    }

    private func exportSetupLogs(_ machine: VirtualMachine) {
        guard !busy, !sessionBusy else { return }
        busy = true
        let image = repo.directory(for: machine).appendingPathComponent(VMRepository.setupLogs)
        let folder = Self.exportDirectory.appendingPathComponent("Windows Setup Logs", isDirectory: true)
        Task {
            defer { busy = false }
            do {
                try await Task.detached(priority: .userInitiated) {
                    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                    let files = image.withUnsafeFileSystemRepresentation { input in
                        folder.withUnsafeFileSystemRepresentation { output in
                            winios_export_setup_logs(input, output)
                        }
                    }
                    guard files > 0 else {
                        throw VMError.message(files == 0 ? "Windows Setup has not saved any logs on the diagnostics disk yet."
                            : "The Windows Setup diagnostics disk could not be read.")
                    }
                }.value
                pickerRequest = DocumentPickerRequest(exporting: folder)
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
                Text("This build can request at most \(maximumJITMiB) MiB.")
            } else if hardware.jitMiB > (HardwarePreset.compatibility.settings?.jitMiB ?? 384) {
                Text("Larger caches are experimental. Start this VM before enabling JIT so the app can request its selected mapping.")
            } else if hardware.preset == .automatic {
                Text("Automatic uses the original device-based memory allocation.")
            }
        }
    }
}
