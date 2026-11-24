// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

@main
enum VMFoundationTests {
    static func main() throws {
        try presetsAndInvalidInputs()
        try downloadRecovery()
        try imageMetadata()
        try preparedDiskValidation()
        try machinePreparationAndRecovery()
        try mediaPublication()
        try netKVMDriverValidation()
        try setupAnswer()
        try importsInboxAndPairing()
        print("9 VM foundation test groups passed")
    }

    static func presetsAndInvalidInputs() throws {
        let expected: [(HardwarePreset, Int, Int)] = [
            (.compatibility, 4608, 384), (.balanced, 6144, 512),
            (.performance, 8192, 1024), (.experimental, 10240, 1536),
        ]
        var hardware = VMHardware()
        for (preset, ram, jit) in expected {
            hardware.preset = preset
            try hardware.validate()
            precondition(hardware.ramMiB == ram && hardware.jitMiB == jit)
        }
        hardware.preset = .custom
        hardware.customRAMMiB = -1
        try rejects { try hardware.validate() }
        hardware.customRAMMiB = 4096
        hardware.customJITMiB = 0
        try rejects { try hardware.validate() }
        hardware.customJITMiB = 384
        hardware.vCPUs = 5
        try rejects { try hardware.validate() }
        hardware.vCPUs = 1
        try hardware.validate()
        try rejects {
            try VirtualMachine(name: "Windows", hardware: hardware, installation: .blankDisk, capacityGiB: Int.max)
                .validate()
        }
    }

    static func downloadRecovery() throws {
        let source = URL(string: "https://example.test/sample.iso")!
        var job = try WindowsDownload(url: source, checksum: String(repeating: "AB", count: 32))
        precondition(job.checksum == String(repeating: "ab", count: 32))
        try rejects { _ = try WindowsDownload(url: URL(string: "http://example.test/sample.iso")!, checksum: nil) }
        try rejects { _ = try WindowsDownload(url: source, checksum: "invalid") }
        let original = job.token
        job.pauseRequested()
        job.didPause(token: original, resumeData: Data([1, 2, 3]))
        let encoded = try PropertyListEncoder().encode(job)
        let restored = try PropertyListDecoder().decode(WindowsDownload.self, from: encoded)
        precondition(restored == job && job.phase == .paused)
        job.prepareRetry()
        precondition(job.token != original && job.phase == .transferring)
        job.didFail(token: original, description: "stale callback", resumeData: nil)
        precondition(job.phase == .transferring)
        job.didFail(token: job.token, description: "interrupted", resumeData: Data([4]))
        precondition(job.phase == .failed && job.resumeData == Data([4]))
        job.prepareRetry()
        job.pauseRequested()
        job.didStageFile(token: job.token)
        job.didPause(token: job.token, resumeData: Data([5]))
        precondition(job.phase == .verifying && job.resumeData == nil)
    }

    static func imageMetadata() throws {
        let good = syntheticWIM(architecture: 12, build: 26100)
        let images = try parse(good)
        try WindowsImageMetadata.requireBaseline(images, media: "Fixture")
        precondition(images == [WindowsImage(name: "Windows 11 Pro", index: 1, architecture: 12, build: 26100)])
        for (arch, build) in [(9, 26100), (12, 28000)] {
            let other = try parse(syntheticWIM(architecture: arch, build: build))
            try rejects { try WindowsImageMetadata.requireBaseline(other, media: "Fixture") }
        }
        try rejects { _ = try parse(good.dropLast()) }
        var oversized = good
        oversized[79] = 0x04
        try rejects { _ = try parse(oversized) }
    }

    static func preparedDiskValidation() throws {
        try withTemporaryDirectory { root in
            let url = root.appendingPathComponent("prepared.img")
            var table = Data(count: 512)
            table.replaceSubrange(0..<16, with: [0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                                  0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b])
            writeLE(2048, into: &table, at: 32, bytes: 8)
            writeLE(4095, into: &table, at: 40, bytes: 8)
            var header = Data(count: 512)
            header.replaceSubrange(0..<8, with: "EFI PART".utf8)
            for (offset, value, bytes) in [(12, 92, 4), (24, 1, 8), (32, 131071, 8),
                                           (40, 2048, 8), (48, 131000, 8), (72, 2, 8),
                                           (80, 4, 4), (84, 128, 4), (88, Int(crc32(table)), 4)] {
                writeLE(value, into: &header, at: offset, bytes: bytes)
            }
            writeLE(Int(crc32(Data(header.prefix(92)))), into: &header, at: 16, bytes: 4)
            try (Data(count: 512) + header + table).write(to: url)
            let file = try FileHandle(forUpdating: url)
            defer { try? file.close() }
            try file.truncate(atOffset: 64 << 20)
            try PreparedDisk.validate(url)
            try file.seek(toOffset: 1024)
            try file.write(contentsOf: Data([0]))
            try rejects { try PreparedDisk.validate(url) }
        }
    }

    static func machinePreparationAndRecovery() throws {
        try withTemporaryDirectory { root in
            let repo = VMRepository(root: root.appendingPathComponent("Machines"))
            let store = InstallationDraftStore(root: root.appendingPathComponent("Install"))
            let assets = InstallerAssets(root: root)
            let code = assets.firmware.code
            let variables = assets.firmware.variables
            try makeFirmware(isVariables: false, at: code)
            try makeFirmware(isVariables: true, at: variables)
            try rejects { try VMRepository.validateFirmware(code, isVariables: true) }
            try rejects { try VMRepository.validateFirmware(variables, isVariables: false) }
            try store.importFirmware(code, destination: store.code)
            try store.importFirmware(variables, destination: store.variables)

            var draft = InstallationDraft()
            draft.stage = .preparing
            draft.mediaID = UUID()
            try store.save(draft)
            let restored = try store.load()
            precondition(restored == draft)
            let firmware = try store.firmware(for: restored, assets: assets)
            try FileManager.default.setAttributes([.posixPermissions: 0o444], ofItemAtPath: variables.path)
            let machine = try store.prepare(restored, repository: repo, firmware: firmware)
            let disk = repo.directory(for: machine).appendingPathComponent(VMRepository.rawDisk)
            let diskBytes = try regularFileSize(disk)
            precondition(diskBytes == 64 << 30)
            let retried = try store.prepare(restored, repository: repo, firmware: firmware)
            precondition(retried == machine)
            var second = restored
            second.machineID = UUID()
            let secondMachine = try store.prepare(second, repository: repo, firmware: firmware)
            try Data([1]).write(to: repo.directory(for: secondMachine).appendingPathComponent(VMRepository.nvram))
            let firstVariables = repo.directory(for: machine).appendingPathComponent(VMRepository.nvram)
            let unchanged = try Data(contentsOf: firstVariables)
            let permissions = try FileManager.default.attributesOfItem(atPath: firstVariables.path)[.posixPermissions] as? NSNumber
            precondition(permissions?.intValue == 0o600)
            let template = try Data(contentsOf: variables)
            precondition(unchanged == template && template.count > 1)
            try repo.remove(secondMachine)

            var rename = machine
            rename.name = "Windows Work"
            try repo.update(rename)
            let listed = try repo.list()
            precondition(listed == [rename])
            let duplicate = try repo.duplicate(rename) { _, destination in
                let file = destination.appendingPathComponent(VMRepository.rawDisk)
                try Data([0]).write(to: file)
            }
            precondition(duplicate.id != rename.id && duplicate.hardware == rename.hardware)
            let copyVars = repo.directory(for: duplicate).appendingPathComponent(VMRepository.nvram)
            try Data([1]).write(to: copyVars)
            let originalVars = repo.directory(for: rename).appendingPathComponent(VMRepository.nvram)
            let originalSize = try regularFileSize(originalVars)
            precondition(originalSize > 1)
            try repo.remove(duplicate)
            try rejects {
                _ = try repo.importPrepared(from: repo.directory(for: rename)) { _, _ in
                    throw VMError.message("disk copy failed")
                }
            }
            let afterError = try repo.list()
            precondition(afterError == [rename])
        }
    }

    static func mediaPublication() throws {
        try withTemporaryDirectory { root in
            let media = WindowsMediaRepository(root: root.appendingPathComponent("Media"))
            let source = root.appendingPathComponent("incoming.iso")
            try Data([1, 2, 3]).write(to: source)
            let id = UUID()
            try rejects {
                _ = try media.store(source: source, name: "Invalid", linking: true, id: id, checksum: nil) { _ in
                    throw VMError.message("not an ISO")
                }
            }
            let invalid = try media.list()
            precondition(invalid.isEmpty && FileManager.default.fileExists(atPath: source.path))
            let first = try media.store(source: source, name: "24H2", linking: true, id: id, checksum: nil) { _ in
                WindowsISOInspection(images: [WindowsImage(name: "Pro", index: 1, architecture: 12, build: 26100)],
                                     computedSHA256: String(repeating: "a", count: 64),
                                     filesystem: "UDF", diagnostics: "Selected UDF.")
            }
            let retry = try media.store(source: source, name: "24H2", linking: true, id: id, checksum: nil) { _ in
                preconditionFailure("recovering an already published image should reuse it")
            }
            precondition(first == retry)
            precondition(first.sha256 == nil && first.savedChecksum == String(repeating: "a", count: 64))
            precondition(first.filesystem == "UDF")
            try media.remove(first)
            let removed = try media.list()
            precondition(removed.isEmpty && FileManager.default.fileExists(atPath: source.path))
        }
    }

    static func netKVMDriverValidation() throws {
        try withTemporaryDirectory { root in
            let folder = root.appendingPathComponent("User driver")
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)
            try Data("CatalogFile=netkvm.cat\n[NetKVM.NTarm64]".utf8)
                .write(to: folder.appendingPathComponent("NetKVM.INF"))
            try Data([0x30, 0]).write(to: folder.appendingPathComponent("netkvm.cat"))
            var binary = Data(count: 70)
            binary[0] = 0x4d
            binary[1] = 0x5a
            binary[60] = 64
            binary.replaceSubrange(64..<70, with: [0x50, 0x45, 0, 0, 0x64, 0x86])
            let sys = folder.appendingPathComponent("netkvm.sys")
            try binary.write(to: sys)
            let destination = root.appendingPathComponent("Selected")
            try rejects { try NetKVMDrivers.stage(from: folder, to: destination) }
            precondition(!FileManager.default.fileExists(atPath: destination.path))
            binary[69] = 0xaa
            try binary.write(to: sys)
            try NetKVMDrivers.stage(from: folder, to: destination)
            let staged = try Data(contentsOf: destination.appendingPathComponent("netkvm.sys"))
            precondition(staged == binary)
        }
    }

    static func importsInboxAndPairing() throws {
        try withTemporaryDirectory { root in
            let imports = root.appendingPathComponent("Imports")
            let empty = try ImportInbox.scan(imports)
            precondition(empty.isEmpty)
            let iso = imports.appendingPathComponent("Windows.ISO")
            try Data([7]).write(to: iso)
            let variables = imports.appendingPathComponent("vars.fd")
            try makeFirmware(isVariables: true, at: variables)
            let pairing = imports.appendingPathComponent("local.mobiledevicepairing")
            let record: [String: Any] = ["public_key": Data(count: 32), "private_key": Data(count: 32),
                                         "identifier": "test-device"]
            try PropertyListSerialization.data(fromPropertyList: record, format: .binary, options: 0).write(to: pairing)
            try PairingFile.validate(pairing)
            try rejects { try PairingFile.validate(iso) }
            try FileManager.default.createSymbolicLink(at: imports.appendingPathComponent("linked.iso"), withDestinationURL: iso)
            let entries = try ImportInbox.scan(imports)
            precondition(entries.count == 3)
            precondition(Set(entries.map(\.kind)) == [.iso, .firmware, .pairing])
            let copy = root.appendingPathComponent("Managed.iso")
            try ImportCopy.file(iso, to: copy)
            try rejects { try ImportCopy.file(iso, to: copy) }
            try Data([1, 2]).write(to: copy)
            let original = try Data(contentsOf: iso)
            precondition(original == Data([7]))
        }
        if let folder = ProcessInfo.processInfo.environment["WINDOWSIOS_INSTALLER_ASSETS"] {
            let assets = InstallerAssets(root: URL(fileURLWithPath: folder))
            try assets.firmware.validate()
            try NetKVMDrivers.validate(from: assets.drivers)
        }
    }

    static func setupAnswer() throws {
        try rejects { _ = try WindowsUnattend.make(imageIndex: 0) }
        let bytes = try WindowsUnattend.make(imageIndex: 6)
        let xml = String(decoding: bytes, as: UTF8.self)
        precondition(xml.contains("<Key>/IMAGE/INDEX</Key><Value>6</Value>"))
        precondition(xml.contains("<WillWipeDisk>false</WillWipeDisk>"))
        precondition(!xml.contains("UserAccounts") && !xml.contains("AcceptEula") && !xml.contains("Bypass"))
    }

    static func makeFirmware(isVariables: Bool, at url: URL) throws {
        let offset = isVariables ? 0 : 4096
        var bytes = Data(count: offset + 64)
        if isVariables {
            bytes.replaceSubrange(16..<32, with: [0x8d, 0x2b, 0xf1, 0xff, 0x96, 0x76, 0x8b, 0x4c,
                                                0xa9, 0x85, 0x27, 0x47, 0x07, 0x5b, 0x4f, 0x50])
        }
        bytes[offset + 32] = 64
        bytes.replaceSubrange((offset + 40)..<(offset + 44), with: Data("_FVH".utf8))
        try bytes.write(to: url)
    }

    static func syntheticWIM(architecture: Int, build: Int) -> Data {
        let contents = "<WIM><IMAGE INDEX=\"1\"><NAME>Windows 11 Pro</NAME><WINDOWS><ARCH>\(architecture)</ARCH><VERSION><BUILD>\(build)</BUILD></VERSION></WINDOWS></IMAGE></WIM>"
        let xml = Data([0xff, 0xfe]) + contents.data(using: .utf16LittleEndian)!
        var wim = Data(count: 208)
        wim.replaceSubrange(0..<8, with: [77, 83, 87, 73, 77, 0, 0, 0])
        for (offset, value, bytes) in [(8, 208, 4), (40, 1, 2), (42, 1, 2), (44, 1, 4),
                                       (72, xml.count, 7), (80, 208, 8), (88, xml.count, 8)] {
            writeLE(value, into: &wim, at: offset, bytes: bytes)
        }
        return wim + xml
    }

    static func writeLE(_ value: Int, into data: inout Data, at offset: Int, bytes: Int) {
        for shift in 0..<bytes { data[offset + shift] = UInt8(truncatingIfNeeded: value >> (shift * 8)) }
    }

    static func crc32(_ data: Data) -> UInt32 {
        ~data.reduce(UInt32.max) { sum, byte in
            (0..<8).reduce(sum ^ UInt32(byte)) { value, _ in
                value & 1 == 0 ? value >> 1 : (value >> 1) ^ 0xedb88320
            }
        }
    }

    static func parse(_ wim: Data) throws -> [WindowsImage] {
        try WindowsImageMetadata.read(length: UInt64(wim.count)) { offset, length in
            let start = Int(offset)
            guard start <= wim.count, length <= wim.count - start else { throw VMError.message("truncated") }
            return wim.subdata(in: start..<(start + length))
        }
    }

    static func rejects(_ body: () throws -> Void) throws {
        do { try body() }
        catch { return }
        preconditionFailure("expected operation to throw")
    }

    static func withTemporaryDirectory(_ body: (URL) throws -> Void) throws {
        let url = URL.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: url) }
        try body(url)
    }
}
