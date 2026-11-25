// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import SwiftUI

struct MachineDiskUsage: Sendable {
    let logical: Int64
    let allocated: Int64

    static func read(in directory: URL) -> Self? {
        let url = directory.appendingPathComponent(VMRepository.rawDisk)
        guard let values = try? url.resourceValues(forKeys: [.fileSizeKey, .fileAllocatedSizeKey, .isRegularFileKey]),
              values.isRegularFile == true, let logical = values.fileSize, let allocated = values.fileAllocatedSize else { return nil }
        return Self(logical: Int64(logical), allocated: Int64(allocated))
    }

    static func text(_ bytes: Int64) -> String { ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file) }
}

struct ApplicationStorageView: View {
    private enum Category: String, CaseIterable, Sendable {
        case disks = "Virtual disks"
        case media = "Installation media"
        case installers = "Installer disks"
        case firmware = "Firmware"
        case temporary = "Temporary files"
        case backups = "Backups"
        case diagnostics = "Diagnostics"
        case other = "Other files"
    }

    @State private var allocated: [Category: Int64]?
    var body: some View {
        Form {
            Section {
                if let allocated {
                    ForEach(Category.allCases, id: \.self) { category in
                        LabeledContent(category.rawValue, value: MachineDiskUsage.text(allocated[category, default: 0]))
                    }
                } else { ProgressView("Reading storage allocation…") }
            } footer: {
                Text("These are allocated file blocks. APFS clones can share blocks, so removing a file may free less space.")
            }
            NavigationLink("Manage Installation Media") { MediaLibraryView() }
        }
        .navigationTitle("Storage")
        .task { await refresh() }
        .refreshable { await refresh() }
    }

    private func refresh() async {
        allocated = await Task.detached(priority: .utility) { Self.readAllocation() }.value
    }

    private nonisolated static func readAllocation() -> [Category: Int64] {
        let keys: Set<URLResourceKey> = [.isRegularFileKey, .isSymbolicLinkKey, .fileAllocatedSizeKey]
        let roots = [URL.applicationSupportDirectory, URL.documentsDirectory, URL.temporaryDirectory]
        var totals: [Category: Int64] = [:]
        for root in roots {
            guard let files = FileManager.default.enumerator(at: root, includingPropertiesForKeys: Array(keys)) else { continue }
            for case let file as URL in files {
                guard let values = try? file.resourceValues(forKeys: keys), values.isRegularFile == true,
                      values.isSymbolicLink != true, let size = values.fileAllocatedSize else { continue }
                let name = file.lastPathComponent.lowercased()
                let category: Category
                if file.pathComponents.contains(where: { $0.lowercased().contains("backup") }) { category = .backups }
                else if root == URL.temporaryDirectory || file.pathComponents.contains(where: { $0.hasPrefix(".staging-") }) { category = .temporary }
                else if name == VMRepository.rawDisk || name == VMRepository.packedDisk { category = .disks }
                else if name.hasSuffix(".iso") { category = .media }
                else if name.hasPrefix("installer") && name.hasSuffix(".img") { category = .installers }
                else if name.hasSuffix(".fd") { category = .firmware }
                else if name.hasSuffix(".log") || name.hasSuffix(".txt") || name == VMRepository.setupLogs.lowercased() { category = .diagnostics }
                else { category = .other }
                totals[category, default: 0] += Int64(size)
            }
        }
        return totals
    }
}
