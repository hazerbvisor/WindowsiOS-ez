// SPDX-License-Identifier: GPL-2.0-or-later
import GameController
import SwiftUI
import UIKit

/// Owns all guest input for the visible session. Physical input uses GameController;
/// UIKit gesture recognizers handle fingers and never synthesize physical button edges.
@MainActor
final class GuestInput: ObservableObject {
    @Published private(set) var hasExternalInput = false
    @Published private(set) var modifiers: Set<UInt8> = []
    private var pointer = GuestPointerState()
    private var keys: Set<UInt8> = []
    private var syntheticKeys: [UUID: Set<UInt8>] = [:]
    private var sentKeys: Set<UInt8> = []
    private var mice: [ObjectIdentifier: GCMouse] = [:]
    private var hover: ObjectIdentifier?
    private var active = false
    private var suspended = false
    private var scrollRemainder: CGFloat = 0
    private var lastWheel: [ObjectIdentifier: Float] = [:]
    private var position = CGPoint(x: 0.5, y: 0.5)
    private var typing: Task<Void, Never>?

    var sensitivity: Double = 1
    var scrollSpeed: Double = 1
    var naturalScroll = true

    func start() {
        guard !active else { return }
        active = true
        suspended = false
        GCMouse.mice().forEach(connect)
        refreshAccessories()
    }

    func stop() {
        reset()
        active = false
        hover = nil
        for mouse in Array(mice.values) { disconnect(mouse) }
    }

    func suspend(_ value: Bool) {
        if value && !suspended { reset(); hover = nil }
        suspended = value
    }

    func refreshAccessories() {
        hasExternalInput = !GCMouse.mice().isEmpty || GCKeyboard.coalesced != nil
    }

    func connect(_ mouse: GCMouse) {
        let id = ObjectIdentifier(mouse)
        guard active, mice[id] == nil, let input = mouse.mouseInput else { return }
        mice[id] = mouse
        mouse.handlerQueue = .main
        input.mouseMovedHandler = { [weak self] _, dx, dy in
            MainActor.assumeIsolated {
                guard let self, self.mice[id] != nil, self.hover != nil else { return }
                let bridge = WinIOSBridge.shared
                self.move(dx: CGFloat(dx), dy: CGFloat(-dy),
                          width: CGFloat(max(640, bridge.framebufferWidth)),
                          height: CGFloat(max(480, bridge.framebufferHeight)))
            }
        }
        func bind(_ button: GCControllerButtonInput?, _ mask: UInt8) {
            button?.pressedChangedHandler = { [weak self] _, _, pressed in
                MainActor.assumeIsolated {
                    guard let self, self.mice[id] != nil, !pressed || self.hover != nil else { return }
                    self.set(mask, source: id, down: pressed)
                }
            }
        }
        bind(input.leftButton, 1)
        bind(input.rightButton, 2)
        bind(input.middleButton, 4)
        input.scroll.valueChangedHandler = { [weak self] _, vertical, _ in
            MainActor.assumeIsolated {
                guard let self, self.mice[id] != nil else { return }
                let previous = self.lastWheel.updateValue(vertical, forKey: id)
                guard self.hover != nil, vertical != previous, vertical != 0 else { return }
                self.scroll(points: CGFloat(vertical) * 12)
            }
        }
        refreshAccessories()
    }

    func disconnect(_ mouse: GCMouse) {
        let id = ObjectIdentifier(mouse)
        guard mice.removeValue(forKey: id) != nil else { return }
        let input = mouse.mouseInput
        input?.mouseMovedHandler = nil
        input?.leftButton.pressedChangedHandler = nil
        input?.rightButton?.pressedChangedHandler = nil
        input?.middleButton?.pressedChangedHandler = nil
        input?.scroll.valueChangedHandler = nil
        release(id)
        lastWheel[id] = nil
        refreshAccessories()
    }

    func setHover(_ surface: ObjectIdentifier, inside: Bool, point: CGPoint?) {
        guard active, !suspended else { return }
        if inside {
            if hover != surface, pointer.buttons == 0, let point { absolute(point) }
            hover = surface
        } else if hover == surface {
            hover = nil
            for id in mice.keys { pointer.release(id) }
            report()
        }
    }

    func absolute(_ point: CGPoint) {
        guard active, !suspended else { return }
        position = CGPoint(x: min(1, max(0, point.x)), y: min(1, max(0, point.y)))
        report()
    }

    func move(dx: CGFloat, dy: CGFloat, width: CGFloat, height: CGFloat) {
        absolute(CGPoint(x: position.x + dx * sensitivity / max(1, width),
                         y: position.y + dy * sensitivity / max(1, height)))
    }

    func set(_ mask: UInt8, source: AnyHashable, down: Bool) {
        guard active, !suspended else { return }
        pointer.set(mask, source: source, down: down)
        report()
    }

    func release(_ source: AnyHashable) {
        let before = pointer.buttons
        pointer.release(source)
        if before != pointer.buttons { report() }
    }

    func click(_ mask: UInt8) {
        let source = UUID()
        set(mask, source: source, down: true)
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(40))
            self?.release(source)
        }
    }

    func scroll(points: CGFloat) {
        guard active, !suspended, points.isFinite else { return }
        scrollRemainder += points * scrollSpeed * (naturalScroll ? 1 : -1)
        let steps = Int(min(16, max(-16, scrollRemainder / 12)))
        if steps != 0 {
            scrollRemainder -= CGFloat(steps) * 12
            report(wheel: Int8(steps))
        }
    }

    func key(_ code: UInt8, down: Bool) {
        guard active, !suspended else { return }
        if down { keys.insert(code) } else { keys.remove(code) }
        updateKeys()
    }

    func toggleModifier(_ code: UInt8) {
        if modifiers.remove(code) == nil { modifiers.insert(code) }
        updateKeys()
    }

    func keyStroke(_ code: UInt8, with extra: [UInt8] = []) {
        guard active, !suspended else { return }
        let token = UUID()
        syntheticKeys[token] = Set(extra + [code])
        updateKeys()
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(35))
            guard let self, self.syntheticKeys.removeValue(forKey: token) != nil else { return }
            self.updateKeys()
        }
    }

    func typeText(_ text: String) {
        let strokes = text.compactMap(MetalCanvasUIView.asciiToHID)
        typing?.cancel()
        typing = Task { @MainActor [weak self] in
            for (code, shifted) in strokes {
                guard !Task.isCancelled else { return }
                self?.keyStroke(code, with: shifted ? [0xe1] : [])
                try? await Task.sleep(for: .milliseconds(55))
            }
        }
    }

    func reset() {
        typing?.cancel()
        pointer.reset()
        report()
        keys.removeAll()
        modifiers.removeAll()
        syntheticKeys.removeAll()
        updateKeys()
        scrollRemainder = 0
    }

    private func updateKeys() {
        let next = syntheticKeys.values.reduce(keys.union(modifiers)) { $0.union($1) }
        for code in sentKeys.subtracting(next).sorted() { WinIOSBridge.shared.sendKeyHid(code, down: false) }
        for code in next.subtracting(sentKeys).sorted(by: >) { WinIOSBridge.shared.sendKeyHid(code, down: true) }
        sentKeys = next
    }

    private func report(wheel: Int8 = 0) {
        WinIOSBridge.shared.sendTabletNormalizedX(Double(position.x), y: Double(position.y), buttons: pointer.buttons, wheel: wheel)
    }
}

struct GuestMouseButton: UIViewRepresentable {
    let input: GuestInput
    let label: String
    let mask: UInt8

    func makeUIView(context: Context) -> MouseControl {
        let button = MouseControl(type: .system)
        button.configure()
        return button
    }

    func updateUIView(_ view: MouseControl, context: Context) {
        view.input = input
        view.mask = mask
        view.setTitle(label, for: .normal)
    }

    static func dismantleUIView(_ view: MouseControl, coordinator: ()) { view.releaseButton() }

    final class MouseControl: UIButton {
        weak var input: GuestInput?
        var mask: UInt8 = 0
        private let source = UUID()

        func configure() {
            backgroundColor = .tertiarySystemBackground
            addTarget(self, action: #selector(pressButton), for: .touchDown)
            addTarget(self, action: #selector(releaseButton), for: [.touchUpInside, .touchUpOutside, .touchCancel])
        }

        @objc private func pressButton() { input?.set(mask, source: source, down: true) }

        @objc func releaseButton() { input?.release(source) }

        override func accessibilityActivate() -> Bool {
            input?.click(mask)
            return true
        }
    }
}
