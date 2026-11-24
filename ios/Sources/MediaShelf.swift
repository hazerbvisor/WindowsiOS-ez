// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Observation
import UIKit

@Observable @MainActor
final class MediaShelf {
    static let shared = MediaShelf()
    static let sessionID = (Bundle.main.bundleIdentifier ?? "io.github.hazerbvisor.windowsios-ez") + ".media"
    private static let workspace = URL.applicationSupportDirectory.appendingPathComponent("WindowsDownloads")
    private static let stateFile = workspace.appendingPathComponent("Download.plist")
    let repository = WindowsMediaRepository(root: URL.applicationSupportDirectory.appendingPathComponent("WindowsMedia"))

    private(set) var items: [WindowsMedia] = []
    private(set) var job: WindowsDownload?
    private(set) var recoveredISOs: [URL] = []
    var completedISO: URL? {
        guard let job, job.phase == .failed || job.phase == .paused else { return nil }
        let file = Self.temporaryISO(for: job.token, in: Self.workspace)
        return FileManager.default.fileExists(atPath: file.path) ? file : nil
    }
    private(set) var busy = false
    private(set) var importProgress: ImportProgress?
    private(set) var restoring = true
    private(set) var writtenBytes: Int64 = 0
    private(set) var expectedBytes: Int64 = 0
    private(set) var speed: Double = 0
    private(set) var verificationFraction: Double?
    var error: String?

    @ObservationIgnored private var activeTask: URLSessionDownloadTask?
    @ObservationIgnored private var backgroundCompletion: (() -> Void)?
    @ObservationIgnored private var lastSample: (date: Date, bytes: Int64)?
    @ObservationIgnored private lazy var session = makeSession()
    @ObservationIgnored private var loaded = false
    @ObservationIgnored private var validatingToken: UUID?

    private init() {}

    func open() {
        guard !loaded else { return }
        loaded = true
        removeAbandonedStaging(in: repository.root)
        do {
            try FileManager.default.createDirectory(at: Self.workspace, withIntermediateDirectories: true)
            var workspace = Self.workspace
            var attributes = URLResourceValues()
            attributes.isExcludedFromBackup = true
            try workspace.setResourceValues(attributes)
            try refresh()
            if FileManager.default.fileExists(atPath: Self.stateFile.path) {
                job = try PropertyListDecoder().decode(WindowsDownload.self, from: Data(contentsOf: Self.stateFile))
            }
        } catch {
            self.error = error.localizedDescription
        }
        refreshRecoveredISOs()
        session.getAllTasks { tasks in
            Task { @MainActor in self.restore(tasks) }
        }
    }

    func importISO(_ selection: ImportSelection, checksum: String?) {
        guard !busy else { return }
        busy = true
        error = nil
        let repository = repository
        let name = selection.url.deletingPathExtension().lastPathComponent
        Task {
            defer {
                busy = false
                importProgress = nil
            }
            do {
                let checksum = try WindowsDownload.normalizedChecksum(checksum)
                let copyProgress = progressReporter("Copying ISO")
                let hashProgress = progressReporter("Checking ISO SHA-256")
                _ = try await Task.detached(priority: .userInitiated) {
                    try VMFiles.withFileAccess(selection, kind: .iso) { source in
                        try repository.store(source: source, name: name, linking: false,
                                             checksum: checksum, copyProgress: copyProgress) { file in
                            try WindowsISO.inspect(file, expectedSHA256: checksum, progress: hashProgress)
                        }
                    }
                }.value
                try refresh()
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    private func progressReporter(_ label: String) -> @Sendable (UInt64, UInt64) -> Void {
        { bytes, total in
            Task { @MainActor in
                guard self.busy else { return }
                self.importProgress = ImportProgress(label: label, bytes: bytes, total: total)
            }
        }
    }

    func remove(_ item: WindowsMedia) {
        guard !busy else { return }
        busy = true
        let mediaRepository = repository
        let machines = VMFiles.repository
        Task {
            defer { busy = false }
            do {
                try await Task.detached(priority: .userInitiated) {
                    if FileManager.default.fileExists(atPath: machines.root.path),
                       try machines.list().contains(where: { $0.mediaID == item.id && $0.installation == .blankDisk }) {
                        throw VMError.message("This ISO is used by a virtual machine awaiting installation.")
                    }
                    try mediaRepository.remove(item)
                }.value
                try refresh()
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    func start(address: String, checksum: String?) throws {
        guard !restoring, job == nil else { throw VMError.message("Wait for recovery or cancel the current download first.") }
        guard let url = URL(string: address.trimmingCharacters(in: .whitespacesAndNewlines)) else {
            throw VMError.message("Enter a valid download link.")
        }
        let model = try WindowsDownload(url: url, checksum: checksum)
        let task = session.downloadTask(with: model.source)
        do {
            try begin(task, model: model)
        } catch {
            task.cancel()
            throw error
        }
    }

    func pause() {
        guard !restoring, var job, job.phase == .transferring, let activeTask else { return }
        job.pauseRequested()
        save(job)
        let token = job.token
        activeTask.cancel(byProducingResumeData: { data in
            Task { @MainActor in
                guard var pending = self.job, pending.token == token, pending.phase == .pausing else { return }
                pending.didPause(token: token, resumeData: data)
                self.activeTask = nil
                self.save(pending)
            }
        })
    }

    func revalidate(checksum: String) throws {
        guard !restoring, var job, let completedISO else { return }
        if let updated = try WindowsDownload.normalizedChecksum(checksum) {
            job.checksum = updated
        }
        save(job)
        acceptDownload(token: job.token, at: completedISO)
    }

    func validateRecovered(_ file: URL, checksum: String?) {
        guard !busy, recoveredISOs.contains(file) else { return }
        busy = true
        let store = repository
        Task {
            defer { busy = false }
            do {
                let checksum = try WindowsDownload.normalizedChecksum(checksum)
                _ = try await Task.detached(priority: .userInitiated) {
                    try store.store(source: file, name: "Windows 11 24H2 ARM64", linking: true,
                                    checksum: checksum) { file in
                        try WindowsISO.inspect(file, expectedSHA256: checksum)
                    }
                }.value
                try FileManager.default.removeItem(at: file)
                refreshRecoveredISOs()
                try refresh()
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    func discardRecovered(_ file: URL) {
        guard !busy, recoveredISOs.contains(file) else { return }
        do {
            try FileManager.default.removeItem(at: file)
            refreshRecoveredISOs()
        } catch {
            self.error = error.localizedDescription
        }
    }

    func resume() {
        guard !restoring, completedISO == nil, var job, job.phase == .paused || job.phase == .failed else { return }
        let resumeData = job.resumeData
        job.prepareRetry()
        let task: URLSessionDownloadTask
        if let resumeData {
            task = session.downloadTask(withResumeData: resumeData)
        } else {
            task = session.downloadTask(with: job.source)
        }
        job.resumeData = nil
        do {
            try begin(task, model: job)
        } catch {
            task.cancel()
            self.error = error.localizedDescription
        }
    }

    func cancel() {
        guard !restoring, let job, job.phase != .verifying else { return }
        activeTask?.cancel()
        activeTask = nil
        clear(job)
    }

    func reconnectBackgroundEvents(completion: @escaping () -> Void) {
        backgroundCompletion = completion
        open()
    }

    private func makeSession() -> URLSession {
        let configuration = URLSessionConfiguration.background(withIdentifier: Self.sessionID)
        configuration.sessionSendsLaunchEvents = true
        configuration.isDiscretionary = false
        return URLSession(configuration: configuration,
                          delegate: MediaTransferDelegate(workspace: Self.workspace, shelf: self),
                          delegateQueue: nil)
    }

    private func restore(_ tasks: [URLSessionTask]) {
        defer { restoring = false }
        for task in tasks where task.taskDescription != job?.token.uuidString { task.cancel() }
        guard var job, validatingToken == nil else { return }
        let received = Self.temporaryISO(for: job.token, in: Self.workspace)
        if FileManager.default.fileExists(atPath: received.path) {
            if job.phase != .failed { acceptDownload(token: job.token, at: received) }
            return
        }
        let running = tasks.first { $0.taskDescription == job.token.uuidString } as? URLSessionDownloadTask
        if job.phase == .pausing {
            running?.cancel()
            job.didPause(token: job.token, resumeData: job.resumeData)
            save(job)
        } else if job.phase == .transferring, let running {
            activeTask = running
        } else if job.phase == .transferring || job.phase == .verifying {
            job.phase = .failed
            job.failure = "The transfer was interrupted. Retry or supply a current link if it expired."
            save(job)
        } else {
            running?.cancel()
        }
    }

    private func begin(_ task: URLSessionDownloadTask, model: WindowsDownload) throws {
        task.taskDescription = model.token.uuidString
        try persist(model)
        job = model
        activeTask = task
        writtenBytes = 0
        expectedBytes = 0
        speed = 0
        lastSample = nil
        task.resume()
    }

    private func persist(_ model: WindowsDownload) throws {
        try PropertyListEncoder().encode(model).write(to: Self.stateFile, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
    }

    private func save(_ model: WindowsDownload) {
        job = model
        do {
            try persist(model)
        } catch {
            self.error = "Unable to save download recovery information. \(error.localizedDescription)"
        }
    }

    fileprivate func updateProgress(token: UUID, written: Int64, expected: Int64) {
        guard let job, job.token == token, job.phase == .transferring else { return }
        let now = Date()
        if let lastSample, now.timeIntervalSince(lastSample.date) < 0.25 { return }
        if let lastSample, written >= lastSample.bytes {
            speed = Double(written - lastSample.bytes) / now.timeIntervalSince(lastSample.date)
        }
        lastSample = (now, written)
        writtenBytes = written
        expectedBytes = expected
    }

    fileprivate func transferFailed(token: UUID, message: String, resumeData: Data?) {
        guard var job, job.token == token, job.phase == .transferring else { return }
        job.didFail(token: token, description: message, resumeData: resumeData)
        activeTask = nil
        save(job)
    }

    fileprivate func acceptDownload(token: UUID, at file: URL) {
        guard var job, job.token == token else {
            refreshRecoveredISOs()
            return
        }
        guard validatingToken != token else { return }
        validatingToken = token
        verificationFraction = nil
        // A completed background download always takes precedence over a racing pause callback.
        job.didStageFile(token: token)
        save(job)
        activeTask = nil
        let download = job
        let store = repository
        Task {
            defer {
                validatingToken = nil
                verificationFraction = nil
            }
            do {
                _ = try await Task.detached(priority: .userInitiated) {
                    try store.store(source: file, name: "Windows 11 24H2 ARM64", linking: true,
                                    id: download.id, checksum: download.checksum) { file in
                        try WindowsISO.inspect(file, expectedSHA256: download.checksum) { bytes, total in
                            Task { @MainActor in
                                guard self.validatingToken == token, total > 0 else { return }
                                self.verificationFraction = Double(bytes) / Double(total)
                            }
                        }
                    }
                }.value
            } catch {
                var failed = download
                failed.phase = .failed
                failed.failure = error.localizedDescription
                save(failed)
                return
            }
            do {
                try refresh()
                clear(download)
            } catch {
                var failed = download
                failed.phase = .failed
                failed.failure = error.localizedDescription
                save(failed)
            }
        }
    }

    private func clear(_ job: WindowsDownload) {
        do {
            if FileManager.default.fileExists(atPath: Self.stateFile.path) {
                try FileManager.default.removeItem(at: Self.stateFile)
            }
            self.job = nil
            let incoming = Self.temporaryISO(for: job.token, in: Self.workspace)
            if FileManager.default.fileExists(atPath: incoming.path) {
                try FileManager.default.removeItem(at: incoming)
            }
        } catch {
            self.error = error.localizedDescription
            if self.job != nil {
                var failed = job
                failed.phase = .failed
                failed.failure = "Could not clear the download state."
                self.job = failed
            }
        }
    }

    fileprivate func finishEvents() {
        backgroundCompletion?()
        backgroundCompletion = nil
    }

    private func refresh() throws {
        items = try repository.list()
    }

    nonisolated fileprivate static func temporaryISO(for token: UUID, in workspace: URL) -> URL {
        workspace.appendingPathComponent(token.uuidString).appendingPathExtension("iso")
    }

    private func refreshRecoveredISOs() {
        let files = (try? FileManager.default.contentsOfDirectory(at: Self.workspace, includingPropertiesForKeys: nil)) ?? []
        recoveredISOs = files.filter {
            $0.pathExtension == "iso" && $0.deletingPathExtension().lastPathComponent != job?.token.uuidString &&
                UUID(uuidString: $0.deletingPathExtension().lastPathComponent) != nil
        }.sorted { $0.lastPathComponent < $1.lastPathComponent }
    }
}

private final class MediaTransferDelegate: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    let workspace: URL
    weak var shelf: MediaShelf?
    private var checkedCapacity: UUID?

    init(workspace: URL, shelf: MediaShelf) {
        self.workspace = workspace
        self.shelf = shelf
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        let location = request.url
        let allowed = location?.scheme?.lowercased() == "https" && location?.user == nil && location?.password == nil
        completionHandler(allowed ? request : nil)
    }

    func urlSession(_ session: URLSession, downloadTask task: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard let token = task.taskDescription.flatMap(UUID.init(uuidString:)) else { return }
        if token != checkedCapacity, totalBytesExpectedToWrite > totalBytesWritten {
            checkedCapacity = token
            if let free = try? workspace.resourceValues(forKeys: [.volumeAvailableCapacityKey]).volumeAvailableCapacity,
               Int64(free) < totalBytesExpectedToWrite - totalBytesWritten {
                Task { @MainActor in
                    shelf?.transferFailed(token: token, message: "There is not enough free space for the rest of this ISO.", resumeData: nil)
                    task.cancel()
                }
                return
            }
        }
        Task { @MainActor in
            shelf?.updateProgress(token: token, written: totalBytesWritten, expected: totalBytesExpectedToWrite)
        }
    }

    func urlSession(_ session: URLSession, downloadTask task: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        guard let token = task.taskDescription.flatMap(UUID.init(uuidString:)) else { return }
        guard let response = task.response as? HTTPURLResponse, [200, 206].contains(response.statusCode),
              response.url?.scheme?.lowercased() == "https" else {
            let status = (task.response as? HTTPURLResponse)?.statusCode
            let message = status.map { "The server returned HTTP \($0). The download link may have expired." } ?? "The server did not return a valid download."
            Task { @MainActor in shelf?.transferFailed(token: token, message: message, resumeData: nil) }
            return
        }
        let file = MediaShelf.temporaryISO(for: token, in: workspace)
        do {
            try FileManager.default.moveItem(at: location, to: file)
            Task { @MainActor in shelf?.acceptDownload(token: token, at: file) }
        } catch {
            Task { @MainActor in
                shelf?.transferFailed(token: token, message: "Could not save the downloaded ISO. Check available storage.", resumeData: nil)
            }
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let failure = error as NSError?, let token = task.taskDescription.flatMap(UUID.init(uuidString:)) else { return }
        let data = failure.userInfo[NSURLSessionDownloadTaskResumeData] as? Data
        Task { @MainActor in
            shelf?.transferFailed(token: token, message: "The transfer stopped (network error \(failure.code)). Verify the connection and retry.",
                                  resumeData: data)
        }
    }

    func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
        Task { @MainActor in shelf?.finishEvents() }
    }
}

@MainActor
final class WindowsDownloadAppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, handleEventsForBackgroundURLSession identifier: String,
                     completionHandler: @escaping () -> Void) {
        guard identifier == MediaShelf.sessionID else {
            completionHandler()
            return
        }
        MediaShelf.shared.reconnectBackgroundEvents(completion: completionHandler)
    }
}
