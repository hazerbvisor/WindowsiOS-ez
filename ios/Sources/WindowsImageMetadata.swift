// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
#if canImport(FoundationXML)
import FoundationXML
#endif

struct WindowsImage: Codable, Equatable {
    let name: String
    let index: Int
    let architecture: Int
    let build: Int

    static let arm64 = 12
    static let baselineBuild = 26100
}

enum WindowsImageMetadata {
    /// WIM/ESD header offsets and XML fields follow the Microsoft WIM format.
    static func read(length: UInt64, bytes: (UInt64, Int) throws -> Data) throws -> [WindowsImage] {
        let header = try bytes(0, 96)
        guard header.count == 96, header.prefix(8) == Data([77, 83, 87, 73, 77, 0, 0, 0]),
              header.littleEndian(at: 8, bytes: 4) >= 96,
              header.littleEndian(at: 40, bytes: 2) == 1,
              header.littleEndian(at: 42, bytes: 2) == 1 else {
            throw VMError.message("The Windows image header is invalid or uses split installation media.")
        }

        let storedSize = header.littleEndian(at: 72, bytes: 7)
        let flags = header[79]
        let offset = header.littleEndian(at: 80, bytes: 8)
        let size = header.littleEndian(at: 88, bytes: 8)
        guard flags & 0x04 == 0, storedSize == size, size > 0, size <= 16 << 20,
              offset >= header.littleEndian(at: 8, bytes: 4), offset <= length, size <= length - offset else {
            throw VMError.message("The Windows image metadata is invalid or compressed in an unsupported format.")
        }

        let xml = try bytes(offset, Int(size))
        guard xml.count == Int(size) else {
            throw VMError.message("The Windows image metadata is truncated.")
        }
        let delegate = WIMMetadataParser()
        let parser = XMLParser(data: xml)
        parser.shouldResolveExternalEntities = false
        parser.delegate = delegate
        let expectedImages = header.littleEndian(at: 44, bytes: 4)
        guard parser.parse(), delegate.isWIM, !delegate.images.isEmpty,
              delegate.images.count == expectedImages,
              Set(delegate.images.map(\.index)).count == delegate.images.count else {
            throw VMError.message("The Windows image edition metadata is invalid.")
        }
        return delegate.images
    }

    static func requireBaseline(_ images: [WindowsImage], media: String) throws {
        guard !images.isEmpty, images.allSatisfy({ $0.architecture == WindowsImage.arm64 }) else {
            throw VMError.message("\(media) is not Windows ARM64 installation media.")
        }
        guard images.allSatisfy({ $0.build == WindowsImage.baselineBuild }) else {
            let builds = Set(images.map(\.build)).sorted().map(String.init).joined(separator: ", ")
            throw VMError.message("\(media) contains build \(builds). Use Windows 11 24H2 build 26100.")
        }
    }
}

private final class WIMMetadataParser: NSObject, XMLParserDelegate {
    var isWIM = false
    var images: [WindowsImage] = []
    private var path: [String] = []
    private var index: Int?
    private var name = ""
    private var architecture: Int?
    private var build: Int?
    private var text = ""

    func parser(_ parser: XMLParser, didStartElement element: String, namespaceURI: String?,
                qualifiedName: String?, attributes: [String: String]) {
        path.append(element)
        if path.count == 1 { isWIM = element == "WIM" }
        if path == ["WIM", "IMAGE"] {
            index = attributes["INDEX"].flatMap(Int.init)
            name = ""
            architecture = nil
            build = nil
        }
        if path.count > 32 { parser.abortParsing() }
        text = ""
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        guard text.utf8.count + string.utf8.count <= 4096 else {
            parser.abortParsing()
            return
        }
        text += string
    }

    func parser(_ parser: XMLParser, didEndElement element: String, namespaceURI: String?, qualifiedName: String?) {
        let string = text.trimmingCharacters(in: .whitespacesAndNewlines)
        switch path {
        case ["WIM", "IMAGE", "NAME"]: name = string
        case ["WIM", "IMAGE", "WINDOWS", "ARCH"]: architecture = Int(string)
        case ["WIM", "IMAGE", "WINDOWS", "VERSION", "BUILD"]: build = Int(string)
        case ["WIM", "IMAGE"]:
            guard let index, index > 0, let architecture, let build, !name.isEmpty else {
                parser.abortParsing()
                return
            }
            images.append(WindowsImage(name: name, index: index, architecture: architecture, build: build))
        default: break
        }
        path.removeLast()
        text = ""
    }
}
