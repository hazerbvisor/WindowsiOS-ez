// SPDX-License-Identifier: GPL-2.0-or-later
import Darwin
import Observation
import SwiftUI

@Observable @MainActor
final class ImportInboxModel {
    static let shared = ImportInboxModel()
    static let directory = URL.documentsDirectory.appendingPathComponent("Imports", isDirectory: true)
    private(set) var entries: [ImportEntry] = []
    private(set) var failure: String?
    @ObservationIgnored private var watcher: (any DispatchSourceFileSystemObject)?
    @ObservationIgnored private var scan: Task<Void, Never>?

    func refresh() {
        scan?.cancel()
        let directory = Self.directory
        scan = Task {
            do {
                let entries = try await Task.detached(priority: .utility) { try ImportInbox.scan(directory) }.value
                try Task.checkCancellation()
                self.entries = entries
                self.failure = nil
                watch(directory)
            } catch is CancellationError {
            } catch {
                if !Task.isCancelled { self.failure = error.localizedDescription }
            }
        }
    }

    private func watch(_ directory: URL) {
        guard watcher == nil else { return }
        let fd = open(directory.path, O_EVTONLY)
        guard fd >= 0 else { return }
        let source = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd,
                                                               eventMask: [.write, .delete, .rename], queue: .main)
        source.setEventHandler { [weak self] in
            Task { @MainActor in
                guard let self, let watcher = self.watcher else { return }
                if !watcher.data.intersection([.delete, .rename]).isEmpty {
                    watcher.cancel()
                    self.watcher = nil
                }
                self.refresh()
            }
        }
        source.setCancelHandler { close(fd) }
        watcher = source
        source.resume()
    }
}

struct ImportInboxSection: View {
    let kinds: Set<DocumentImportKind>
    let disabled: Bool
    let select: (ImportEntry) -> Void
    private let inbox = ImportInboxModel.shared
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        Section {
            ForEach(inbox.entries.filter { kinds.contains($0.kind) }) { entry in
                Button(entry.url.lastPathComponent, systemImage: "doc") { select(entry) }
                    .disabled(disabled)
            }
            Button("Refresh Imports", systemImage: "arrow.clockwise") { inbox.refresh() }
            if let failure = inbox.failure { Text(failure).foregroundStyle(.secondary) }
        } header: {
            Text("Imports")
        } footer: {
            Text("In Files, place items in On My iPad or On My iPhone → WindowsiOS-ez → Imports.")
        }
        .task { inbox.refresh() }
        .onChange(of: scenePhase) { _, phase in if phase == .active { inbox.refresh() } }
    }
}

struct ImportProgressView: View {
    let progress: ImportProgress?

    var body: some View {
        if let progress, progress.total > 0 {
            ProgressView(progress.label, value: Double(progress.bytes), total: Double(progress.total))
            LabeledContent("Processed", value: ByteCountFormatter.string(fromByteCount: Int64(progress.bytes), countStyle: .file))
        } else {
            ProgressView(progress?.label ?? "Opening file…")
        }
    }
}
