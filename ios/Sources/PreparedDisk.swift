// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum PreparedDisk {
    private static let sector: UInt64 = 512
    private static let efiSystemPartitionType = Data([0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                                      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b])

    static func validate(_ url: URL) throws {
        _ = try efiPartition(in: url)
    }

    static func efiPartition(in url: URL) throws -> (start: UInt64, sectors: UInt64) {
        let size = try regularFileSize(url)
        guard size >= VMRepository.minimumDiskBytes, size % sector == 0 else {
            throw VMError.message("The prepared disk must use the emulator’s 512-byte sectors.")
        }
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        try file.seek(toOffset: sector)
        guard let header = try file.read(upToCount: Int(sector)), header.count == Int(sector),
              header.prefix(8) == Data("EFI PART".utf8) else {
            throw VMError.message("The prepared disk has no GPT header.")
        }
        let headerBytes = header.littleEndian(at: 12, bytes: 4)
        let backupLBA = header.littleEndian(at: 32, bytes: 8)
        let firstLBA = header.littleEndian(at: 40, bytes: 8)
        let lastLBA = header.littleEndian(at: 48, bytes: 8)
        let tableLBA = header.littleEndian(at: 72, bytes: 8)
        let entryCount = header.littleEndian(at: 80, bytes: 4)
        let entryBytes = header.littleEndian(at: 84, bytes: 4)
        let diskSectors = size / sector
        guard (92...sector).contains(headerBytes), header.littleEndian(at: 24, bytes: 8) == 1,
              backupLBA < diskSectors, firstLBA <= lastLBA, lastLBA < backupLBA,
              tableLBA >= 2, tableLBA < firstLBA, (1...16384).contains(entryCount),
              (128...4096).contains(entryBytes), entryBytes % 128 == 0 else {
            throw VMError.message("The prepared disk has an invalid GPT layout.")
        }
        let tableBytes = entryCount * entryBytes
        let tableOffset = tableLBA * sector
        guard tableBytes <= 8 << 20, tableBytes <= (firstLBA * sector) - tableOffset else {
            throw VMError.message("The prepared disk has an invalid or oversized partition table.")
        }
        var crcHeader = Data(header.prefix(Int(headerBytes)))
        crcHeader.replaceSubrange(16..<20, with: [0, 0, 0, 0])
        try file.seek(toOffset: tableOffset)
        guard let table = try file.read(upToCount: Int(tableBytes)), table.count == Int(tableBytes),
              crc(crcHeader) == header.littleEndian(at: 16, bytes: 4),
              crc(table) == header.littleEndian(at: 88, bytes: 4) else {
            throw VMError.message("The prepared disk’s GPT checksums do not match.")
        }
        for offset in stride(from: 0, to: table.count, by: Int(entryBytes))
            where table[offset..<(offset + 16)] == efiSystemPartitionType {
            let first = table.littleEndian(at: offset + 32, bytes: 8)
            let last = table.littleEndian(at: offset + 40, bytes: 8)
            if first >= firstLBA, first <= last, last <= lastLBA { return (first, last - first + 1) }
        }
        throw VMError.message("The prepared disk has no valid EFI system partition.")
    }

    private static let crcTable: [UInt32] = (0..<256).map { byte in
        (0..<8).reduce(UInt32(byte)) { value, _ in
            (value >> 1) ^ (value & 1 == 0 ? 0 : 0xedb88320)
        }
    }

    private static func crc(_ data: Data) -> UInt64 {
        let value = data.reduce(UInt32.max) { crc, byte in
            crcTable[Int((crc ^ UInt32(byte)) & 0xff)] ^ (crc >> 8)
        }
        return UInt64(~value)
    }
}
