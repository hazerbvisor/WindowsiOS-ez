// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

// Keep physical, touchscreen, and native button controls independent.
struct GuestPointerState {
    private var held: [AnyHashable: UInt8] = [:]

    var buttons: UInt8 { held.values.reduce(0, |) }

    mutating func set(_ mask: UInt8, source: AnyHashable, down: Bool) {
        let previous = held[source, default: 0]
        let updated = down ? previous | mask : previous & ~mask
        held[source] = updated == 0 ? nil : updated
    }

    mutating func release(_ source: AnyHashable) { held[source] = nil }
    mutating func reset() { held.removeAll() }
}

struct GuestDisplayGeometry {
    enum Scaling: String, CaseIterable, Codable {
        case fit, fill, pixelPerfect, zoom

        var title: String {
            switch self {
            case .fit: "Fit"
            case .fill: "Fill"
            case .pixelPerfect: "Pixel Perfect"
            case .zoom: "Zoom"
            }
        }
    }

    static func frame(viewport: CGSize, pixels: CGSize, displayScale: CGFloat,
                      scaling: Scaling, zoom: CGFloat = 1, offset: CGSize = .zero) -> CGRect {
        guard viewport.width > 0, viewport.height > 0, pixels.width > 0, pixels.height > 0 else {
            return CGRect(origin: .zero, size: viewport)
        }
        let fit = min(viewport.width / pixels.width, viewport.height / pixels.height)
        let ratio: CGFloat
        switch scaling {
        case .fit: ratio = fit
        case .fill: ratio = max(viewport.width / pixels.width, viewport.height / pixels.height)
        case .pixelPerfect: ratio = 1 / max(1, displayScale)
        case .zoom: ratio = fit * max(1, zoom)
        }
        let width = pixels.width * ratio
        let height = pixels.height * ratio
        let limitX = max(0, (width - viewport.width) / 2)
        let limitY = max(0, (height - viewport.height) / 2)
        return CGRect(x: (viewport.width - width) / 2 + min(limitX, max(-limitX, offset.width)),
                      y: (viewport.height - height) / 2 + min(limitY, max(-limitY, offset.height)),
                      width: width, height: height)
    }

    static func normalized(_ point: CGPoint, in bounds: CGRect) -> CGPoint {
        CGPoint(x: max(0, min(1, (point.x - bounds.minX) / max(1, bounds.width))),
                y: max(0, min(1, (point.y - bounds.minY) / max(1, bounds.height))))
    }
}
