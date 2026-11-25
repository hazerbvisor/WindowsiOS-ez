// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct WindowsDownload: Codable, Equatable {
    enum Phase: String, Codable {
        case transferring, pausing, paused, verifying, failed
    }

    let id: UUID
    let source: URL
    var checksum: String?
    var token: UUID
    var phase: Phase
    var resumeData: Data?
    var failure: String?

    init(url: URL, checksum: String?) throws {
        guard url.scheme?.lowercased() == "https", url.host?.isEmpty == false,
              url.user == nil, url.password == nil else {
            throw VMError.message("Enter a direct HTTPS link to Windows 11 24H2 ARM64 media.")
        }
        id = UUID()
        source = url
        self.checksum = try Self.normalizedChecksum(checksum)
        token = UUID()
        phase = .transferring
    }

    static func normalizedChecksum(_ checksum: String?) throws -> String? {
        guard let checksum = checksum?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased(),
              !checksum.isEmpty else { return nil }
        guard checksum.utf8.count == 64,
              checksum.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else {
            throw VMError.message("Enter a SHA-256 checksum as 64 hexadecimal characters.")
        }
        return checksum
    }

    mutating func pauseRequested() {
        if phase == .transferring { phase = .pausing }
    }

    mutating func didPause(token: UUID, resumeData: Data?) {
        guard self.token == token, phase == .pausing else { return }
        phase = .paused
        self.resumeData = resumeData
    }

    mutating func didFail(token: UUID, description: String, resumeData: Data?) {
        guard self.token == token, phase == .transferring else { return }
        phase = .failed
        failure = description
        self.resumeData = resumeData
    }

    mutating func prepareRetry() {
        guard phase == .failed || phase == .paused else { return }
        token = UUID()
        phase = .transferring
        failure = nil
    }

    mutating func didStageFile(token: UUID) {
        guard self.token == token else { return }
        phase = .verifying
        resumeData = nil
        failure = nil
    }
}
