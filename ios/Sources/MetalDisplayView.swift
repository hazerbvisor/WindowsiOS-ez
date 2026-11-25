// SPDX-License-Identifier: GPL-2.0-or-later
import Metal
import QuartzCore
import SwiftUI
import UIKit

/// A guest input surface. Its touch recognizers only accept direct touches.
@MainActor
class GuestPointerSurface: UIView, UIPointerInteractionDelegate {
    weak var input: GuestInput?
    var relative = false
    var hideHostPointer = true {
        didSet { if oldValue != hideHostPointer { pointerStyle.invalidate() } }
    }
    var reveal: (() -> Void)?
    private lazy var pointerStyle = UIPointerInteraction(delegate: self)
    private var drag: CGPoint?
    private var previousScroll: CGFloat = 0
    private var lastTap = -Double.infinity
    private var source: ObjectIdentifier { ObjectIdentifier(self) }

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        addInteraction(pointerStyle)
        let tap = UITapGestureRecognizer(target: self, action: #selector(tapped(_:)))
        let right = UITapGestureRecognizer(target: self, action: #selector(rightTapped))
        right.numberOfTouchesRequired = 2
        let controls = UITapGestureRecognizer(target: self, action: #selector(revealControls))
        controls.numberOfTouchesRequired = 4
        let hold = UILongPressGestureRecognizer(target: self, action: #selector(held(_:)))
        hold.minimumPressDuration = 0.35
        let pan = UIPanGestureRecognizer(target: self, action: #selector(panned(_:)))
        pan.maximumNumberOfTouches = 1
        let scroll = UIPanGestureRecognizer(target: self, action: #selector(scrolled(_:)))
        scroll.minimumNumberOfTouches = 2
        scroll.maximumNumberOfTouches = 2
        tap.require(toFail: hold)
        tap.require(toFail: right)
        tap.require(toFail: controls)
        pan.require(toFail: hold)
        for gesture in [tap, right, controls, hold, pan, scroll] {
            gesture.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
            addGestureRecognizer(gesture)
        }
        addGestureRecognizer(UIHoverGestureRecognizer(target: self, action: #selector(hovered(_:))))
        isAccessibilityElement = true
        accessibilityLabel = "Windows input surface"
        accessibilityCustomActions = [UIAccessibilityCustomAction(name: "Show Windows controls", target: self,
                                                                  selector: #selector(accessibleReveal))]
    }

    required init?(coder: NSCoder) { fatalError("Constructed in code") }

    func pointerInteraction(_ interaction: UIPointerInteraction, styleFor region: UIPointerRegion) -> UIPointerStyle? {
        hideHostPointer ? .hidden() : nil
    }

    func pointerInteraction(_ interaction: UIPointerInteraction, regionFor request: UIPointerRegionRequest,
                            defaultRegion: UIPointerRegion) -> UIPointerRegion? { defaultRegion }

    func pointerInteraction(_ interaction: UIPointerInteraction, willExit region: UIPointerRegion,
                            animator: UIPointerInteractionAnimating) {
        input?.setHover(source, inside: false, point: nil)
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { clearInput() }
    }

    func clearInput() {
        input?.setHover(source, inside: false, point: nil)
        input?.release(source)
        drag = nil
    }

    @objc private func accessibleReveal() -> Bool { revealControls(); return true }
    @objc private func revealControls() { reveal?() }

    @objc private func hovered(_ gesture: UIHoverGestureRecognizer) {
        let inside = gesture.state == .began || gesture.state == .changed
        let point = relative ? nil : normalized(gesture)
        input?.setHover(source, inside: inside, point: point)
    }

    @objc private func tapped(_ gesture: UITapGestureRecognizer) {
        if !relative { input?.absolute(normalized(gesture)) }
        lastTap = CACurrentMediaTime()
        input?.click(1)
    }

    @objc private func rightTapped() { input?.click(2) }

    @objc private func held(_ gesture: UILongPressGestureRecognizer) {
        track(gesture, holdsButton: true)
    }

    @objc private func panned(_ gesture: UIPanGestureRecognizer) {
        track(gesture, holdsButton: !relative || CACurrentMediaTime() - lastTap < 0.32)
        if gesture.state == .began { lastTap = -Double.infinity }
    }

    private func track(_ gesture: UIGestureRecognizer, holdsButton: Bool) {
        let point = gesture.location(in: self)
        switch gesture.state {
        case .began:
            drag = point
            if !relative { input?.absolute(normalized(gesture)) }
            if holdsButton { input?.set(1, source: source, down: true) }
        case .changed:
            if relative, let previous = drag {
                input?.move(dx: point.x - previous.x, dy: point.y - previous.y,
                            width: bounds.width, height: bounds.height)
            } else if !relative { input?.absolute(normalized(gesture)) }
            drag = point
        case .ended, .cancelled, .failed:
            input?.release(source)
            drag = nil
        default: break
        }
    }

    @objc private func scrolled(_ gesture: UIPanGestureRecognizer) {
        let position = gesture.translation(in: self).y
        if gesture.state == .began { previousScroll = position }
        if gesture.state == .changed {
            input?.scroll(points: position - previousScroll)
            previousScroll = position
        }
    }

    private func normalized(_ gesture: UIGestureRecognizer) -> CGPoint {
        GuestDisplayGeometry.normalized(gesture.location(in: self), in: bounds)
    }
}

final class MetalCanvasUIView: GuestPointerSurface, UIKeyInput {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }
    private let keyboardSuppressor = UIView(frame: .zero)
    var acceptsGuestFocus = false
    var frameRate = 0 {
        didSet { if oldValue != frameRate { applyFrameRate() } }
    }
    var typing = false {
        didSet { if oldValue != typing && isFirstResponder { reloadInputViews() } }
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        metalLayer.device = MTLCreateSystemDefaultDevice()
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
    }

    required init?(coder: NSCoder) { fatalError("Constructed in code") }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            applyFrameRate()
            WinIOSBridge.shared.bindMetalLayer(metalLayer)
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let scale = window?.screen.scale ?? traitCollection.displayScale
        let maximum = window?.bounds.size ?? bounds.size
        let bounded = min(1, max(maximum.width, maximum.height) / max(1, bounds.width, bounds.height))
        let drawable = CGSize(width: max(1, bounds.width * scale * bounded), height: max(1, bounds.height * scale * bounded))
        if metalLayer.drawableSize != drawable { metalLayer.drawableSize = drawable }
        if let size = window?.bounds.size { WinIOSBridge.shared.updateScreenSize(size, scale: scale) }
    }

    private func applyFrameRate() {
        guard let maximum = window?.screen.maximumFramesPerSecond else { return }
        WinIOSBridge.shared.setPresentationRate(UInt32(max(1, min(120, maximum, frameRate == 0 ? 60 : frameRate))))
    }

    override var canBecomeFirstResponder: Bool { true }
    override var inputView: UIView? { typing ? nil : keyboardSuppressor }
    override var editingInteractionConfiguration: UIEditingInteractionConfiguration { .none }
    var hasText: Bool { true }

    func insertText(_ text: String) {
        guard typing else { return }
        input?.typeText(text)
    }

    func deleteBackward() { input?.keyStroke(0x2a) }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let remaining = send(presses, down: true)
        if !remaining.isEmpty { super.pressesBegan(remaining, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let remaining = send(presses, down: false)
        if !remaining.isEmpty { super.pressesEnded(remaining, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        input?.reset()
        let remaining = send(presses, down: false)
        if !remaining.isEmpty { super.pressesCancelled(remaining, with: event) }
    }

    private func send(_ presses: Set<UIPress>, down: Bool) -> Set<UIPress> {
        var unknown: Set<UIPress> = []
        var usages: [UInt8] = []
        for press in presses {
            if let code = press.key?.keyCode.rawValue, (0x04...0xe7).contains(code) {
                usages.append(UInt8(code))
            } else { unknown.insert(press) }
        }
        for usage in usages.sorted(by: down ? (>) : (<)) { input?.key(usage, down: down) }
        return unknown
    }

    static func asciiToHID(_ ch: Character) -> (UInt8, Bool)? {
        if let ascii = ch.asciiValue {
            switch ascii {
            case 97...122: return (UInt8(0x04 + Int(ascii - 97)), false) // a-z
            case 65...90:  return (UInt8(0x04 + Int(ascii - 65)), true)  // A-Z
            case 49...57:  return (UInt8(0x1e + Int(ascii - 49)), false) // 1-9
            case 48:       return (0x27, false) // 0
            case 10, 13:   return (0x28, false) // Enter
            case 9:        return (0x2b, false) // Tab
            case 32:       return (0x2c, false) // Space
            default: break
            }
        }
        let symbols: [Character: (UInt8, Bool)] = [
            "!": (0x1e, true), "@": (0x1f, true), "#": (0x20, true), "$": (0x21, true),
            "%": (0x22, true), "^": (0x23, true), "&": (0x24, true), "*": (0x25, true),
            "(": (0x26, true), ")": (0x27, true), "-": (0x2d, false), "_": (0x2d, true),
            "=": (0x2e, false), "+": (0x2e, true), "[": (0x2f, false), "{": (0x2f, true),
            "]": (0x30, false), "}": (0x30, true), "\\": (0x31, false), "|": (0x31, true),
            ";": (0x33, false), ":": (0x33, true), "'": (0x34, false), "\"": (0x34, true),
            "`": (0x35, false), "~": (0x35, true), ",": (0x36, false), "<": (0x36, true),
            ".": (0x37, false), ">": (0x37, true), "/": (0x38, false), "?": (0x38, true),
        ]
        return symbols[ch]
    }
}

struct MetalDisplayView: UIViewRepresentable {
    let input: GuestInput
    let relative: Bool
    let hideHostPointer: Bool
    let typing: Bool
    let frameRate: Int
    let focused: Bool
    let reveal: () -> Void

    func makeUIView(context: Context) -> MetalCanvasUIView { MetalCanvasUIView(frame: .zero) }

    func updateUIView(_ view: MetalCanvasUIView, context: Context) {
        view.input = input
        view.relative = relative
        view.hideHostPointer = hideHostPointer
        view.reveal = reveal
        view.typing = typing
        view.frameRate = frameRate
        if focused {
            if !view.isFirstResponder {
                DispatchQueue.main.async { [weak view] in
                    guard let view, view.window != nil, view.acceptsGuestFocus else { return }
                    _ = view.becomeFirstResponder()
                }
            }
        } else if view.isFirstResponder { view.resignFirstResponder() }
        view.acceptsGuestFocus = focused
    }

    static func dismantleUIView(_ view: MetalCanvasUIView, coordinator: ()) {
        view.acceptsGuestFocus = false
        view.clearInput()
        view.resignFirstResponder()
        WinIOSBridge.shared.bindMetalLayer(nil)
    }
}

struct GuestTrackpad: UIViewRepresentable {
    let input: GuestInput
    let hideHostPointer: Bool
    let reveal: () -> Void

    func makeUIView(context: Context) -> GuestPointerSurface { GuestPointerSurface(frame: .zero) }

    func updateUIView(_ view: GuestPointerSurface, context: Context) {
        view.input = input
        view.relative = true
        view.hideHostPointer = hideHostPointer
        view.reveal = reveal
        view.accessibilityLabel = "Windows trackpad"
    }
}
