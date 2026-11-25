// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

struct WindowsLibraryView: View {
    let vm: VMViewModel

    enum Tab { case machines, install, settings }
    @State private var tab = Tab.machines
    @State private var beginInstall = false
    @AppStorage("appearance") private var appearance = WindowsAppearance.system

    var body: some View {
        TabView(selection: $tab) {
            MachineLibraryView(vm: vm, beginInstall: $beginInstall)
                .tabItem { Label("Machines", systemImage: "desktopcomputer") }
                .tag(Tab.machines)
            NavigationStack {
                ScrollView {
                    VStack(alignment: .leading, spacing: 16) {
                        VStack(alignment: .leading, spacing: 14) {
                            WindowsMark()
                            Text("Windows 11").font(.title2.bold())
                            Text("24H2 · ARM64").foregroundStyle(.secondary)
                            Button {
                                tab = .machines
                                beginInstall = true
                            } label: { Label("Create Virtual Machine", systemImage: "plus") }
                            .buttonStyle(.borderedProminent)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .windowsCard()
                        NavigationLink { MediaLibraryView() } label: {
                            Label("Downloads and ISO Imports", systemImage: "arrow.down.circle")
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .windowsCard()
                        }
                    }
                    .padding(16)
                    .windowsColumn()
                }
                .background(Color(uiColor: .systemGroupedBackground))
                .navigationTitle("Install")
            }
            .tabItem { Label("Install", systemImage: "arrow.down.circle") }
            .tag(Tab.install)
            NavigationStack { AppSettingsView(vm: vm) }
                .tabItem { Label("Settings", systemImage: "gearshape") }
                .tag(Tab.settings)
        }
        .tint(Color(red: 0.19, green: 0.40, blue: 0.82))
        .preferredColorScheme(appearance.scheme)
        .task { ImportInboxModel.shared.refresh() }
    }
}

private struct MachineLibraryView: View {
    @ObservedObject var vm: VMViewModel
    @Binding var beginInstall: Bool
    @AppStorage("session.defaultScaling") private var defaultScaling = GuestDisplayGeometry.Scaling.fit
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
            machinePresentations
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
        .modifier(MachineConfirmations(deleteCandidate: $deleteCandidate, installedCandidate: $installedCandidate,
                                       remove: { machine in
                                           guard !sessionBusy, !busy else { return }
                                           editor = nil
                                           doFileWork { try repo.remove(machine) }
                                       }, adopt: adoptInstalledDisk))
    }

    private var featured: VirtualMachine? {
        activeMachine.flatMap { active in machines.first { $0.id == active.id } }
            ?? machines.max { ($0.lastOpened ?? .distantPast) < ($1.lastOpened ?? .distantPast) }
    }

    private var machineList: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                if busy {
                    if let importProgress { ImportProgressView(progress: importProgress) }
                    else { ProgressView("Working with machine files…") }
                }
                if vm.needsRelaunchToStart {
                    Label("Reopen the app to start another session.", systemImage: "arrow.clockwise")
                        .font(.callout).foregroundStyle(.secondary)
                }
                if sessionBusy && activeMachine == nil {
                    Button("Resume Windows", systemImage: "play.rectangle") { showSession = true }
                }
                if let featured {
                    MachineHero(machine: featured, directory: repo.directory(for: featured), running: isActive(featured),
                                jitReady: vm.isJITLive, canStart: canStart(featured),
                                open: { editor = featured }, launch: { launch(featured) })
                    let others = machines.filter { $0.id != featured.id }
                    if !others.isEmpty {
                        Text("Other Machines").font(.headline)
                        VStack(spacing: 12) {
                            ForEach(others) { machine in
                                Button { editor = machine } label: {
                                    HStack {
                                        Image(systemName: "desktopcomputer").foregroundStyle(.tint)
                                        VStack(alignment: .leading) {
                                            Text(machine.name).font(.headline)
                                            Text(machine.installation == .preparedDisk ? "Windows 11" : "Ready to install")
                                                .font(.subheadline).foregroundStyle(.secondary)
                                        }
                                        Spacer()
                                        Image(systemName: "chevron.right").foregroundStyle(.tertiary)
                                    }
                                }
                                .buttonStyle(.plain)
                            }
                        }
                        .windowsCard()
                    }
                } else if !busy && !hasLegacy && !sessionBusy {
                    ContentUnavailableView {
                        Label("No virtual machines", systemImage: "desktopcomputer")
                    } description: {
                        Text("Install Windows or import an existing virtual machine.")
                    } actions: {
                        Button("Install Windows") { showAssistant = true }
                            .buttonStyle(.borderedProminent)
                    }
                    .windowsCard()
                }
                ImportInboxSection(kinds: [.machine, .disk], disabled: busy || sessionBusy) { entry in
                    importMachine(kind: entry.kind, selection: ImportSelection(entry.url))
                }
                if hasLegacy {
                    Button("Start Existing Files Installation", systemImage: "desktopcomputer") { start(nil) }
                        .disabled(sessionBusy || busy || vm.needsRelaunchToStart)
                        .windowsCard()
                }
            }
            .padding(16)
            .windowsColumn()
        }
        .background(Color(uiColor: .systemGroupedBackground))
        .navigationTitle("Machines")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Menu {
                    Button("Install Windows", systemImage: "plus") { showAssistant = true }
                    Button("Import Virtual Machine", systemImage: "folder") { pickerRequest = DocumentPickerRequest(kind: .machine) }
                    Button("Import Virtual Disk", systemImage: "externaldrive") { pickerRequest = DocumentPickerRequest(kind: .disk) }
                } label: {
                    Label("Add Virtual Machine", systemImage: "plus")
                }
                .disabled(busy || sessionBusy)
            }
        }
        .onChange(of: beginInstall) { _, request in
            if request {
                beginInstall = false
                if !busy && !sessionBusy { showAssistant = true }
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
            .navigationDestination(isPresented: Binding(get: { editor != nil }, set: { if !$0 { editor = nil } })) {
                if let machine = editor {
                    MachineEditor(machine: machine, directory: repo.directory(for: machine), editable: !sessionBusy && !busy,
                                  save: { edited in
                                      try repo.update(edited)
                                      editor = edited
                                      refresh()
                                  }) {
                        machineActions(machine)
                    }
                    .id(machine.id)
                }
            }
            .sheet(isPresented: $showAssistant, onDismiss: closeAssistant) {
                InstallationAssistant(vm: vm) { setupAfterAssistant = $0 }
            }
            .fullScreenCover(isPresented: $showSession, onDismiss: refresh) { machineSession }
    }

    private var machineSession: some View {
        SessionView(vm: vm) {
            vm.saveDesktopPreview()
            if var machine = activeMachine, vm.fbWidth > 0, vm.fbHeight > 0 {
                machine.lastGuestWidth = vm.fbWidth
                machine.lastGuestHeight = vm.fbHeight
                try? repo.update(machine)
                activeMachine = machine
            }
            showSession = false
        }
        .preferredColorScheme(.dark)
    }

    private var sessionBusy: Bool { vm.isVMRunning || vm.isUnpacking }

    private var errorPresented: Binding<Bool> {
        Binding(get: { error != nil }, set: { if !$0 { error = nil } })
    }

    private func isActive(_ machine: VirtualMachine) -> Bool {
        sessionBusy && activeMachine?.id == machine.id
    }

    private func canStart(_ machine: VirtualMachine) -> Bool {
        isActive(machine) || (!sessionBusy && !busy && !vm.needsRelaunchToStart &&
            (machine.installation == .preparedDisk || FileManager.default.fileExists(atPath: installer(for: machine).path)))
    }

    private func installer(for machine: VirtualMachine) -> URL {
        repo.directory(for: machine).appendingPathComponent(VMRepository.installerDisk)
    }

    private func launch(_ machine: VirtualMachine) {
        if isActive(machine) { showSession = true }
        else { start(machine, installer: machine.installation == .blankDisk ? installer(for: machine) : nil) }
    }

    @ViewBuilder private func machineActions(_ machine: VirtualMachine) -> some View {
        let installerURL = installer(for: machine)
        let hasInstaller = FileManager.default.fileExists(atPath: installerURL.path)
        Section("Session") {
            Button(isActive(machine) ? "Resume" : "Start", systemImage: "play") { launch(machine) }
                .disabled(!canStart(machine))
            if machine.installation == .blankDisk {
                Button("Use Installed Windows Disk", systemImage: "checkmark.circle") { installedCandidate = machine }
                    .disabled(sessionBusy)
            }
        }
        Section("Manage") {
            if machine.installation == .blankDisk, machine.installationMethod != .direct, let mediaID = machine.mediaID {
                Button(hasInstaller ? "Update Graphical Setup Disk" : "Build Graphical Setup Disk",
                       systemImage: "opticaldisc") { buildInstaller(for: mediaID, output: installerURL) }
                    .disabled(sessionBusy || busy)
            }
            if FileManager.default.fileExists(atPath: repo.directory(for: machine)
                .appendingPathComponent(VMRepository.setupLogs).path) {
                Button("Export Installation Logs", systemImage: "square.and.arrow.up") { exportSetupLogs(machine) }
                    .disabled(sessionBusy || busy)
            }
            Button("Duplicate", systemImage: "plus.square.on.square") {
                doFileWork { _ = try repo.duplicate(machine, copyDisk: VMFiles.copyDisk) }
            }
            .disabled(sessionBusy || busy)
            Button(shouldKeepBackupLocal(machine) ? "Save Local VM Backup" : "Export Virtual Machine",
                   systemImage: "square.and.arrow.up") { exportMachine(machine) }
                .disabled(sessionBusy || busy)
            Button("Delete Virtual Machine", systemImage: "trash", role: .destructive) { deleteCandidate = machine }
                .disabled(sessionBusy || busy)
        }
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
        if var opened = machine {
            opened.lastOpened = Date()
            do { try repo.update(opened) } catch { self.error = error.localizedDescription }
            activeMachine = opened
        }
        vm.open(directory: machine.map { repo.directory(for: $0) }, hardware: machine?.hardware ?? VMHardware(),
                installer: installer, useTPM: machine?.usesTPM == true, method: machine?.installationMethod,
                scaling: machine?.displayScaling.flatMap(GuestDisplayGeometry.Scaling.init(rawValue:)) ?? defaultScaling,
                experimentalGraphics: machine?.installation == .preparedDisk && machine?.experimentalGraphics == true,
                guestResolution: machine?.guestResolution ?? .compatibility)
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

private struct MachineConfirmations: ViewModifier {
    @Binding var deleteCandidate: VirtualMachine?
    @Binding var installedCandidate: VirtualMachine?
    let remove: (VirtualMachine) -> Void
    let adopt: (VirtualMachine, Bool) -> Void

    func body(content: Content) -> some View {
        content
            .confirmationDialog("Delete virtual machine?", isPresented: presentation($deleteCandidate), presenting: deleteCandidate) { machine in
                Button("Delete \(machine.name)", role: .destructive) { remove(machine); deleteCandidate = nil }
            } message: { machine in
                Text("This permanently deletes the managed disk and firmware state for \(machine.name).")
            }
            .confirmationDialog("Use the installed Windows disk?", isPresented: presentation($installedCandidate),
                                presenting: installedCandidate) { machine in
                Button("Use Installed Disk and Remove Setup Disk") { adopt(machine, true); installedCandidate = nil }
                Button("Use Installed Disk and Keep Setup Disk") { adopt(machine, false); installedCandidate = nil }
            } message: { _ in
                Text("Continue after installation finishes and the guest shuts down. The app checks the primary disk for ARM64 Windows boot files.")
            }
    }

    private func presentation(_ candidate: Binding<VirtualMachine?>) -> Binding<Bool> {
        Binding(get: { candidate.wrappedValue != nil }, set: { if !$0 { candidate.wrappedValue = nil } })
    }
}

private struct MachineHero: View {
    let machine: VirtualMachine
    let directory: URL
    let running: Bool
    let jitReady: Bool
    let canStart: Bool
    let open: () -> Void
    let launch: () -> Void
    @State private var disk: MachineDiskUsage?
    @State private var screenshot: UIImage?

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Button(action: open) {
                VStack(alignment: .leading, spacing: 14) {
                    preview
                    HStack(spacing: 12) {
                        WindowsMark()
                        VStack(alignment: .leading, spacing: 3) {
                            Text(machine.name).font(.title2.bold())
                            Text(machine.windowsEdition ?? (machine.mediaID != nil ? "Windows 11 24H2 · ARM64" : "Windows"))
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        Image(systemName: "chevron.right").foregroundStyle(.tertiary)
                    }
                }
            }
            .buttonStyle(.plain)
            HStack {
                Label(running ? "Active" : "Stopped", systemImage: running ? "play.circle" : "stop.circle")
                Spacer()
                Label(jitReady ? "JIT Ready" : "JIT Required", systemImage: jitReady ? "bolt.fill" : "bolt.slash")
            }
            .font(.subheadline)
            .foregroundStyle(.secondary)
            Text("\(machine.hardware.preset.title) · \(machine.hardware.vCPUs) vCPU · \(machine.hardware.jitMiB) MiB JIT")
                .font(.subheadline).foregroundStyle(.secondary)
            Button(running ? "Resume" : machine.installation == .blankDisk ? "Start Installation" : "Start",
                   systemImage: "play.fill", action: launch)
                .buttonStyle(.borderedProminent)
                .disabled(!canStart)
            Divider()
            HStack(alignment: .top) {
                LabeledContent("Disk", value: disk.map { MachineDiskUsage.text($0.allocated) } ?? "—")
                Spacer(minLength: 16)
                if let date = machine.lastOpened {
                    Text(date, style: .relative).foregroundStyle(.secondary)
                        .accessibilityLabel("Last opened \(date.formatted())")
                }
            }
            .font(.footnote)
        }
        .windowsCard()
        .task(id: machine) {
            disk = MachineDiskUsage.read(in: directory)
            screenshot = UIImage(contentsOfFile: directory.appendingPathComponent(VMRepository.preview).path)
        }
    }

    private var preview: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 14).fill(Color(uiColor: .tertiarySystemGroupedBackground))
            if let screenshot {
                Image(uiImage: screenshot).resizable().scaledToFit()
                    .accessibilityLabel("Last captured Windows desktop")
            } else {
                Image(systemName: "desktopcomputer").font(.system(size: 42, weight: .light))
                    .foregroundStyle(.secondary).accessibilityHidden(true)
            }
        }
        .aspectRatio(16.0 / 9.0, contentMode: .fit)
        .clipShape(.rect(cornerRadius: 14))
    }
}

private struct MachineEditor<Actions: View>: View {
    @State private var draft: VirtualMachine
    @State private var error: String?
    @State private var disk: MachineDiskUsage?
    let directory: URL
    let editable: Bool
    let save: (VirtualMachine) throws -> Void
    let actions: () -> Actions

    init(machine: VirtualMachine, directory: URL, editable: Bool, save: @escaping (VirtualMachine) throws -> Void,
         @ViewBuilder actions: @escaping () -> Actions) {
        _draft = State(initialValue: machine)
        self.directory = directory
        self.editable = editable
        self.save = save
        self.actions = actions
    }

    var body: some View {
        Form {
            Section("Identity") {
                TextField("Machine name", text: $draft.name).disabled(!editable)
                LabeledContent("Edition", value: draft.windowsEdition ?? "Unavailable")
                LabeledContent("Version", value: draft.mediaID == nil ? "Unavailable" : "24H2 · ARM64")
                LabeledContent("Status", value: draft.installation == .preparedDisk ? "Installed" : "Awaiting installation")
            }
            HardwareFields(hardware: $draft.hardware).disabled(!editable)
            Section {
                Picker("Host scaling", selection: Binding(get: {
                    draft.displayScaling.flatMap(GuestDisplayGeometry.Scaling.init(rawValue:)) ?? .fit
                }, set: { draft.displayScaling = $0.rawValue })) {
                    ForEach(GuestDisplayGeometry.Scaling.allCases, id: \.self) {
                        Text($0 == .pixelPerfect ? "1:1" : $0.title).tag($0)
                    }
                }
                .pickerStyle(.segmented)
                .disabled(!editable)
                if draft.experimentalGraphics == true && draft.installation == .preparedDisk {
                    Picker("Guest resolution", selection: Binding(get: {
                        draft.guestResolution ?? .compatibility
                    }, set: { draft.guestResolution = $0 })) {
                        ForEach(GuestResolution.allCases) { mode in
                            Text(mode.title).tag(mode)
                        }
                    }.disabled(!editable)
                    Text("Advertises the selected mode to the Windows VirtIO display driver on next launch. Choose that resolution in Windows Display settings if it does not switch automatically.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                if let width = draft.lastGuestWidth, let height = draft.lastGuestHeight {
                    LabeledContent("Last guest resolution", value: "\(width) × \(height)")
                }
            } header: {
                Text("Screen")
            } footer: {
                Text("To change the guest resolution, open Session Details while Windows is running. The resolutions you can choose depend on the Windows display adapter. Host scaling applies when you open the session.")
            }
            Section {
                Toggle("VirtIO-GPU (experimental)", isOn: Binding(get: { draft.experimentalGraphics == true },
                    set: { draft.experimentalGraphics = $0 }))
                    .disabled(!editable || draft.installation != .preparedDisk)
                if draft.installation == .preparedDisk && draft.experimentalGraphics == true {
                    NavigationLink("Display driver installation") { MetalGPUDriverView() }
                }
            } header: { Text("Graphics") } footer: {
                Text("RAMFB is the default. Ordinary builds offer display-only. The experimental MetalGPU SMP IPA accepts 1–8 vCPUs. Back up the installed VM; multi-vCPU GPU operation is unverified. Save before starting Windows. Shut down and relaunch the app with this off to recover. Setup always uses RAMFB.")
            }
            Section("Storage") {
                if let disk {
                    LabeledContent("Allocated on device", value: MachineDiskUsage.text(disk.allocated))
                    LabeledContent("Virtual capacity", value: MachineDiskUsage.text(disk.logical))
                    if disk.logical > 0 {
                        ProgressView(value: min(1, Double(disk.allocated) / Double(disk.logical))) {
                            Text("Physical allocation")
                        }
                    }
                } else {
                    Text("Disk usage is unavailable.").foregroundStyle(.secondary)
                }
            }
            actions()
            if let error { Text(error).foregroundStyle(.red) }
        }
        .windowsColumn()
        .background(Color(uiColor: .systemGroupedBackground))
        .navigationTitle(draft.name)
        .task { disk = MachineDiskUsage.read(in: directory) }
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") {
                    do { try save(draft); error = nil }
                    catch { self.error = error.localizedDescription }
                }
                .disabled(!editable)
            }
        }
    }
}

struct HardwareFields: View {
    @Binding var hardware: VMHardware
    @State private var customJIT = false
    private let maximumJITMiB = WinIOSBridge.shared.maximumJITMiB
    private let jitSizes = [384, 512, 768, 1024, 1536]

    var body: some View {
        Section {
            Picker("Preset", selection: $hardware.preset) {
                ForEach(HardwarePreset.allCases) { Text($0.title).tag($0) }
            }
            .onChange(of: hardware.preset) { _, preset in
                if preset == .experimental {
                    hardware.vCPUs = 8
                    hardware.jitOverrideMiB = nil
                    customJIT = false
                }
            }
            if hardware.preset == .experimental {
                LabeledContent("MetalGPU resource limit", value: "1,024 MiB")
                LabeledContent("Configured budget (GPU on)", value: "\(hardware.ramMiB + hardware.jitMiB + 1024) MiB")
                Text("Experimental budget excludes additional emulator and renderer overhead. GPU resources are allocated on demand, not reserved VRAM.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            if hardware.preset == .custom {
                Stepper("Guest RAM · \(hardware.ramMiB) MiB", onIncrement: { adjustRAM(by: 256) },
                        onDecrement: { adjustRAM(by: -256) })
                TextField("Guest RAM (MiB)", value: $hardware.customRAMMiB, format: .number).keyboardType(.numberPad)
            } else {
                LabeledContent("Guest RAM", value: hardware.ramMiB == 0 ? "Automatic" : "\(hardware.ramMiB) MiB")
            }
        } header: {
            Text("Memory")
        } footer: {
            if hardware.ramMiB > 6144 {
                Text("High guest RAM can cause iPadOS memory pressure or end the app session.")
            }
        }
        Section {
            Stepper("Virtual CPUs · \(hardware.vCPUs)", value: $hardware.vCPUs, in: VMHardware.vCPURange)
        } header: {
            Text("Processors")
        } footer: {
            if hardware.vCPUs > 4 {
                Text("5–8 virtual CPUs are experimental on iPadOS. More virtual CPUs can increase memory use and may reduce performance.")
            }
        }
        Section {
            Menu {
                Button("Use Preset") { hardware.jitOverrideMiB = nil; customJIT = hardware.preset == .custom }
                ForEach(jitSizes.filter { $0 <= Int(maximumJITMiB) }, id: \.self) { value in
                    Button("\(value) MiB") { hardware.jitOverrideMiB = value; customJIT = false }
                }
                Button("Custom") { hardware.jitOverrideMiB = hardware.jitMiB; customJIT = true }
            } label: { LabeledContent("Requested cache", value: "\(hardware.jitMiB) MiB") }
            if customJIT || (hardware.jitOverrideMiB == nil && hardware.preset == .custom)
                || hardware.jitOverrideMiB.map({ !jitSizes.contains($0) }) == true {
                TextField("JIT cache (MiB)", value: Binding(get: { hardware.jitMiB }, set: { hardware.jitOverrideMiB = $0 }),
                          format: .number).keyboardType(.numberPad)
            }
        } header: {
            Text("JIT Cache")
        } footer: {
            if hardware.jitMiB > Int(maximumJITMiB) {
                Text("This build supports requests up to \(maximumJITMiB) MiB.")
            } else {
                Text("The executable cache is allocated once per app launch. Reopen the app after changing this setting.")
            }
        }
    }

    private func adjustRAM(by delta: Int) {
        let (updated, overflow) = hardware.customRAMMiB.addingReportingOverflow(delta)
        if !overflow { hardware.customRAMMiB = max(4096, updated) }
    }
}
