// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

struct MediaLibraryView: View {
    @State private var shelf = MediaShelf.shared
    @State private var showImport = false
    @State private var showDownloadForm = false
    @State private var checksum = ""
    @State private var removeCandidate: WindowsMedia?

    var body: some View {
        List {
            Section {
                Button("Import Windows ISO", systemImage: "square.and.arrow.down") { showImport = true }
                    .disabled(shelf.busy)
                Button("Download from a Link", systemImage: "link") { showDownloadForm = true }
                    .disabled(shelf.job != nil || shelf.restoring)
                Link(destination: URL(string: "https://www.microsoft.com/software-download/windows11arm64")!) {
                    Label("Microsoft ARM64 download page", systemImage: "arrow.up.right.square")
                }
                TextField("Optional SHA-256 checksum", text: $checksum)
                    .autocorrectionDisabled().textInputAutocapitalization(.never)
            } header: {
                Text("Windows 11 24H2 ARM64")
            } footer: {
                Text("Choose build 26100. Microsoft’s current public download may be a newer version. If so, use a 24H2 ISO you already obtained.")
            }
            if let job = shelf.job {
                Section("Download") {
                    DownloadStatusView(shelf: shelf, job: job)
                }
            }
            if shelf.busy {
                ProgressView("Importing and inspecting ISO…")
            }
            Section("Verified media") {
                if shelf.items.isEmpty {
                    Text("No installation media").foregroundStyle(.secondary)
                }
                ForEach(shelf.items) { item in
                    VStack(alignment: .leading, spacing: 4) {
                        Text(item.name).font(.headline)
                        Text("ARM64 · Build 26100 · \(formattedBytes(Int64(item.bytes)))")
                            .foregroundStyle(.secondary).font(.subheadline)
                        if item.sha256 != nil {
                            Label("SHA-256 matched", systemImage: "checkmark.shield")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    .swipeActions(allowsFullSwipe: false) {
                        Button("Remove", role: .destructive) { removeCandidate = item }
                            .disabled(shelf.busy)
                    }
                    .contextMenu {
                        Button("Remove Stored ISO", role: .destructive) { removeCandidate = item }
                            .disabled(shelf.busy)
                    }
                }
            }
        }
        .navigationTitle("Downloads")
        .confirmationDialog("Remove the stored ISO?", isPresented: Binding(
            get: { removeCandidate != nil }, set: { if !$0 { removeCandidate = nil } }
        ), presenting: removeCandidate) { item in
            Button("Remove", role: .destructive) {
                removeCandidate = nil
                shelf.remove(item)
            }
        } message: { item in
            Text("This removes the app’s stored copy of \(item.name).")
        }
        .task { shelf.open() }
        .fileImporter(isPresented: $showImport, allowedContentTypes: [UTType(filenameExtension: "iso") ?? .data]) { result in
            switch result {
            case .success(let url): shelf.importISO(url, checksum: checksum)
            case .failure(let error): shelf.error = error.localizedDescription
            }
        }
        .sheet(isPresented: $showDownloadForm) {
            DownloadForm(initialChecksum: checksum) { address, checksum in
                try shelf.start(address: address, checksum: checksum)
            }
        }
        .alert("Windows Installation Media", isPresented: Binding(
            get: { shelf.error != nil }, set: { if !$0 { shelf.error = nil } }
        )) {
            Button("OK", role: .cancel) { shelf.error = nil }
        } message: {
            Text(shelf.error ?? "")
        }
    }
}

private struct DownloadStatusView: View {
    let shelf: MediaShelf
    let job: WindowsDownload

    var body: some View {
        switch job.phase {
        case .transferring:
            if shelf.expectedBytes > 0 {
                ProgressView(value: min(1, Double(shelf.writtenBytes) / Double(shelf.expectedBytes)))
                LabeledContent("Remaining", value: formattedBytes(max(0, shelf.expectedBytes - shelf.writtenBytes)))
            } else {
                ProgressView("Downloading Windows ISO…")
                LabeledContent("Downloaded", value: formattedBytes(shelf.writtenBytes))
            }
            if shelf.speed > 0 { LabeledContent("Speed", value: "\(formattedBytes(Int64(shelf.speed)))/s") }
            Button("Pause", systemImage: "pause") { shelf.pause() }
                .disabled(shelf.restoring)
        case .pausing:
            ProgressView("Pausing download…")
        case .paused, .failed:
            if let failure = job.failure {
                Text(failure).foregroundStyle(.secondary)
            } else {
                Text("Paused")
            }
            Button(job.resumeData == nil ? "Restart Download" : "Resume Download", systemImage: "play") { shelf.resume() }
                .disabled(shelf.restoring)
        case .verifying:
            ProgressView("Checking ARM64 media and build number…")
        }
        if job.phase != .verifying {
            Button("Cancel Download", role: .destructive) { shelf.cancel() }
                .disabled(shelf.restoring)
        }
    }
}

private struct DownloadForm: View {
    @Environment(\.dismiss) private var dismiss
    @State private var address = ""
    @State private var checksum: String
    @State private var error: String?
    let onStart: (String, String?) throws -> Void

    init(initialChecksum: String, onStart: @escaping (String, String?) throws -> Void) {
        _checksum = State(initialValue: initialChecksum)
        self.onStart = onStart
    }

    var body: some View {
        NavigationStack {
            Form {
                TextField("Direct HTTPS ISO link", text: $address)
                    .keyboardType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                TextField("Optional SHA-256 checksum", text: $checksum)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                if let error { Text(error).foregroundStyle(.red) }
            }
            .navigationTitle("Download Windows ISO")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Download") {
                        do {
                            try onStart(address, checksum)
                            dismiss()
                        } catch {
                            self.error = error.localizedDescription
                        }
                    }
                    .disabled(address.isEmpty)
                }
            }
        }
    }
}

private func formattedBytes(_ bytes: Int64) -> String {
    ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
}

struct JITSettingsView: View {
    @ObservedObject var vm: VMViewModel

    var body: some View {
        Form {
            JITStatusSection(vm: vm)
            Section("Setup") {
                Text("Sign with a profile that allows debugging and enable Developer Mode where required. Complete any pairing in your authorized debugger, open it from here, then return to WindowsiOS-ez.")
                Text("On a paired Mac, the existing LLDB launcher can attach using the installed bundle identifier.")
                Toggle("Open guest sign-in links in Safari", isOn: Binding(
                    get: { vm.useSafariForOAuth },
                    set: { if $0 != vm.useSafariForOAuth { vm.toggleOAuthBrowserMode() } }
                ))
            }
        }
        .navigationTitle("Settings")
    }
}

struct JITStatusSection: View {
    @ObservedObject var vm: VMViewModel

    var body: some View {
        Section("JIT") {
            Label(vm.isJITLive ? "Executable self-test passed" : "JIT not ready",
                  systemImage: vm.isJITLive ? "checkmark.circle.fill" : "exclamationmark.circle")
                .foregroundStyle(vm.isJITLive ? Color.green : Color.orange)
            Text(vm.jitStatusText)
                .font(.subheadline)
                .foregroundStyle(.secondary)
            if !vm.isJITLive {
                Button("Open StikDebug", systemImage: "bolt") { vm.requestStikDebugJIT() }
                Button("Check JIT Readiness", systemImage: "arrow.clockwise") { vm.onAppBecameActive() }
            }
        }
    }
}

struct DiagnosticsView: View {
    @ObservedObject var vm: VMViewModel

    private let snapshot = URL.documentsDirectory.appendingPathComponent("boot_status.txt")

    var body: some View {
        List {
            Section("Device") {
                LabeledContent("iOS / iPadOS", value: UIDevice.current.systemVersion)
                LabeledContent("Physical memory", value: "\(ProcessInfo.processInfo.physicalMemory >> 20) MiB")
                LabeledContent("Estimated process headroom", value: "\(vm.availableMemoryMB) MiB")
            }
            Section("Engine") {
                Text(vm.vmStatusText)
                Text(vm.jitStatusText)
                if FileManager.default.fileExists(atPath: snapshot.path) {
                    ShareLink(item: snapshot) { Label("Export Diagnostics", systemImage: "square.and.arrow.up") }
                }
            }
        }
        .navigationTitle("Diagnostics")
        .task { WinIOSBridge.shared.captureDiagnostics() }
        .refreshable { WinIOSBridge.shared.captureDiagnostics() }
    }
}
