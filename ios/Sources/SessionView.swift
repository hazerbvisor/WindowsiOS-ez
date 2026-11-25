// SPDX-License-Identifier: GPL-2.0-or-later
import GameController
import SwiftUI
import UIKit

struct SessionView: View {
    @ObservedObject var vm: VMViewModel
    let leave: () -> Void
    @StateObject private var input = GuestInput()
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.displayScale) private var displayScale
    @AppStorage("session.hideHostPointer") private var hideHostPointer = true
    @AppStorage("session.relativeInput") private var defaultRelative = false
    @AppStorage("session.sensitivity") private var sensitivity = 1.0
    @AppStorage("session.scrollSpeed") private var scrollSpeed = 1.0
    @AppStorage("session.naturalScroll") private var naturalScroll = true
    @AppStorage("session.autoHide") private var autoHide = false
    @AppStorage("session.toolbarTop") private var toolbarTop = true
    @AppStorage("session.frameRate") private var frameRate = 0
    @State private var relative = false
    @State private var typing = false
    @State private var controls = true
    @State private var details = false
    @State private var power = false
    @State private var clipboard = false
    @State private var scaling: GuestDisplayGeometry.Scaling = .fit
    @State private var zoom: CGFloat = 1
    @State private var pinchStart: CGFloat?
    @State private var panStart: CGSize?
    @State private var offset: CGSize = .zero
    @State private var hideTask: Task<Void, Never>?
    @State private var isVisible = false

    private var focused: Bool {
        isVisible && vm.isVMRunning && scenePhase == .active && !details && !power && !clipboard && !editingZoom && vm.activeSafariURL == nil
    }

    var body: some View {
        GeometryReader { geometry in
            let portrait = geometry.size.height > geometry.size.width && !input.hasExternalInput && vm.isVMRunning
            VStack(spacing: 0) {
                ZStack {
                    display
                    if !vm.isVMRunning { bootPanel }
                    if !portrait && controls && vm.isVMRunning {
                        toolbar.frame(maxHeight: .infinity, alignment: toolbarTop ? .top : .bottom)
                            .padding(.top, toolbarTop ? max(8, geometry.safeAreaInsets.top) + 6 : 0)
                            .padding(.bottom, toolbarTop ? 0 : max(8, geometry.safeAreaInsets.bottom) + 6)
                    }
                }
                if portrait {
                    if controls { toolbar.padding(.vertical, 7) }
                    if !typing { trackpad.frame(height: max(105, geometry.size.height * 0.30)) }
                }
            }
        }
        .background(Color.black.ignoresSafeArea())
        .ignoresSafeArea(edges: [.horizontal, .top])
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .sheet(isPresented: $details) {
            SessionDetails(vm: vm, scaling: $scaling, relative: relative, leave: leave)
        }
        .sheet(item: $vm.activeSafariURL) { item in SafariSheet(url: item.url) }
        .confirmationDialog("Windows power", isPresented: $power, titleVisibility: .visible) {
            Button("Send Power Button") { WinIOSBridge.shared.pressPowerButton() }
            Button("Send Ctrl–Alt–Delete") { WinIOSBridge.shared.sendCtrlAltDel() }
            Button("Stop Virtual Machine", role: .destructive) { WinIOSBridge.shared.stopVM() }
            Button("Back to Machines") { leave() }
        } message: {
            Text("Use Windows to shut down cleanly. Stopping immediately can interrupt guest disk writes.")
        }
        .confirmationDialog("Clipboard", isPresented: $clipboard, titleVisibility: .visible) {
            Button("Type into Windows") { pasteText() }
            Button("Save as Shared Text File") { shareText() }
        } message: {
            Text("Typing uses the Windows keyboard layout and may submit lines in the focused app. Use a shared file for Unicode or large text.")
        }
        .alert("Clipboard", isPresented: Binding(get: { vm.clipboardError != nil },
                                                 set: { if !$0 { vm.clipboardError = nil } })) {
            Button("OK", role: .cancel) { vm.clipboardError = nil }
        } message: { Text(vm.clipboardError ?? "") }
        .onAppear {
            relative = defaultRelative
            scaling = vm.initialScaling
            input.sensitivity = sensitivity
            input.scrollSpeed = scrollSpeed
            input.naturalScroll = naturalScroll
            input.start()
            isVisible = true
            input.suspend(!focused)
            scheduleAutoHide()
        }
        .onDisappear {
            isVisible = false
            input.stop()
            hideTask?.cancel()
        }
        .onChange(of: focused) { _, value in input.suspend(!value) }
        .onChange(of: details) { _, presenting in if !presenting { reveal() } }
        .onChange(of: power) { _, presenting in if !presenting { reveal() } }
        .onChange(of: typing) { _, _ in scheduleAutoHide() }
        .onChange(of: scaling) { _, _ in zoom = 1; offset = .zero }
        .onChange(of: vm.isVMRunning) { _, running in
            if !running { controls = true; typing = false }
            scheduleAutoHide()
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCMouseDidConnect)) { note in
            if let mouse = note.object as? GCMouse { input.connect(mouse) }
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCMouseDidDisconnect)) { note in
            if let mouse = note.object as? GCMouse { input.disconnect(mouse) }
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCKeyboardDidConnect)) { _ in input.refreshAccessories() }
        .onReceive(NotificationCenter.default.publisher(for: .GCKeyboardDidDisconnect)) { _ in
            input.reset()
            input.refreshAccessories()
        }
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillHideNotification)) { _ in
            typing = false
        }
    }

    private var display: some View {
        GeometryReader { geometry in
            let pixels = CGSize(width: CGFloat(vm.fbWidth), height: CGFloat(vm.fbHeight))
            let frame = GuestDisplayGeometry.frame(viewport: geometry.size, pixels: pixels, displayScale: displayScale,
                                                   scaling: scaling, zoom: zoom, offset: offset)
            ZStack(alignment: .topLeading) {
                Color.black
                MetalDisplayView(input: input, relative: relative, hideHostPointer: hideHostPointer,
                                 typing: typing, frameRate: frameRate, focused: focused, reveal: reveal)
                    .frame(width: max(1, frame.width), height: max(1, frame.height))
                    .position(x: frame.midX, y: frame.midY)
                if scaling == .zoom {
                    zoomSurface(size: geometry.size, pixels: pixels)
                }
            }
            .contentShape(Rectangle())
            .clipped()
        }
    }

    private func zoomSurface(size: CGSize, pixels: CGSize) -> some View {
        // Zoom editing is explicit so two-finger scrolling and guest drags keep their input surface.
        ZoomEditingSurface(editing: $editingZoom, zoom: $zoom, offset: $offset, reveal: reveal,
                           zoomChanged: { value in
                               let first = pinchStart ?? zoom
                               pinchStart = first
                               zoom = max(1, min(4, first * value))
                           }, zoomEnded: { pinchStart = nil }, panChanged: { translation in
                               let first = panStart ?? offset
                               panStart = first
                               let fit = GuestDisplayGeometry.frame(viewport: size, pixels: pixels,
                                                                    displayScale: displayScale, scaling: .zoom, zoom: zoom)
                               let x = max(0, (fit.width - size.width) / 2)
                               let y = max(0, (fit.height - size.height) / 2)
                               offset = CGSize(width: max(-x, min(x, first.width + translation.width)),
                                               height: max(-y, min(y, first.height + translation.height)))
                           }, panEnded: { panStart = nil })
    }

    @State private var editingZoom = false

    private var trackpad: some View {
        VStack(spacing: 1) {
            GuestTrackpad(input: input, hideHostPointer: hideHostPointer, reveal: reveal)
                .background(Color(uiColor: .secondarySystemBackground))
            HStack(spacing: 1) {
                GuestMouseButton(input: input, label: "Left click", mask: 1)
                GuestMouseButton(input: input, label: "Right click", mask: 2)
            }
            .frame(height: 44)
        }
        .clipShape(.rect(cornerRadius: 18))
        .padding([.horizontal, .bottom], 12)
    }

    private var toolbar: some View {
        HStack(spacing: 4) {
            control("Hide Controls", "chevron.compact.up") { controls = false }
            control(typing ? "Hide Keyboard" : "Show Keyboard", "keyboard") { typing.toggle() }
            control("Clipboard", "clipboard") { clipboard = true }
            control("Zoom", "plus.magnifyingglass") {
                if scaling == .zoom && editingZoom { editingZoom = false }
                else { scaling = .zoom; editingZoom = true }
                if editingZoom { hideTask?.cancel() }
            }
            control(relative ? "Switch to Direct Touch" : "Switch to Trackpad", relative ? "trackpad" : "hand.point.up.left") {
                input.reset()
                relative.toggle()
            }
            control("Session Details", "info.circle") { details = true }
            control("Power and Exit", "power") { power = true }
        }
        .padding(4)
        .background(.regularMaterial, in: Capsule())
        .padding(.horizontal, 7)
        .overlay(alignment: toolbarTop ? .bottom : .top) {
            if typing && controls { modifierBar.offset(y: toolbarTop ? 49 : -49) }
        }
        .zIndex(1)
    }

    private func control(_ label: String, _ icon: String, action: @escaping () -> Void) -> some View {
        Button {
            action()
            scheduleAutoHide()
        } label: {
            Image(systemName: icon).frame(width: 39, height: 44)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
    }

    private var modifierBar: some View {
        ScrollView(.horizontal) {
            HStack(spacing: 9) {
                ForEach([UInt8(0xe0), 0xe2, 0xe1, 0xe3], id: \.self) { key in
                    let label = [0xe0: "Ctrl", 0xe2: "Alt", 0xe1: "Shift", 0xe3: "Win"][Int(key)] ?? "Modifier"
                    Button(label) { input.toggleModifier(key) }
                        .tint(input.modifiers.contains(key) ? .blue : .secondary)
                }
                Button("Esc") { input.keyStroke(0x29) }
                Button("Tab") { input.keyStroke(0x2b) }
                Menu("F keys") {
                    ForEach(1...12, id: \.self) { index in
                        Button("F\(index)") { input.keyStroke(UInt8(0x39 + index)) }
                    }
                }
                ForEach([UInt8(0x50), 0x52, 0x51, 0x4f], id: \.self) { key in
                    let label = [0x50: "←", 0x52: "↑", 0x51: "↓", 0x4f: "→"][Int(key)] ?? "Arrow"
                    Button(label) { input.keyStroke(key) }
                }
            }
            .padding(.horizontal, 10)
        }
        .scrollIndicators(.hidden)
        .buttonStyle(.bordered)
        .padding(.vertical, 4)
        .frame(maxWidth: 410)
        .background(.regularMaterial, in: Capsule())
    }

    private var bootPanel: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(vm.installationSession?.title ?? "Windows").font(.title.bold())
            Text(vm.vmStatusText)
            Text(vm.jitStatusText).font(.footnote).foregroundStyle(.secondary)
            if vm.isUnpacking {
                ProgressView(value: vm.unpackProgress)
            } else if !vm.needsRelaunchToStart {
                Button(vm.isJITLive ? vm.bootActionTitle : "Retry Boot") { vm.startOrRetry() }
                    .buttonStyle(.borderedProminent)
                if !vm.isJITLive {
                    Button("Open StikDebug") { vm.requestStikDebugJIT() }
                }
            }
            Button("Back to Machines", action: leave)
        }
        .padding(24)
        .frame(maxWidth: 415, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 20))
        .padding()
    }

    private func reveal() {
        controls = true
        scheduleAutoHide()
    }

    private func pasteText() {
        guard let text = UIPasteboard.general.string, !text.isEmpty else { return }
        let normalized = text.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n")
        guard vm.usbKeyboardReady, normalized.utf8.count <= 1024,
              normalized.utf8.allSatisfy({ $0 == 9 || $0 == 10 || (32...126).contains($0) }) else {
            vm.clipboardError = "Keyboard paste needs a connected guest keyboard and up to 1024 ASCII characters. Save a shared text file for other text."
            return
        }
        clipboard = false
        input.suspend(!focused)
        input.typeText(normalized)
    }

    private func shareText() {
        guard let text = UIPasteboard.general.string, !text.isEmpty else { return }
        let directory = URL.documentsDirectory.appendingPathComponent("Shared", isDirectory: true)
        let file = directory.appendingPathComponent("Clipboard-\(UUID().uuidString.prefix(8)).txt")
        Task {
            do {
                try await Task.detached(priority: .utility) {
                    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                    try Data(text.utf8).write(to: file, options: .withoutOverwriting)
                }.value
                vm.clipboardError = "Saved \(file.lastPathComponent) in Shared."
            } catch { vm.clipboardError = error.localizedDescription }
        }
    }

    private func scheduleAutoHide() {
        hideTask?.cancel()
        guard autoHide, vm.isVMRunning, controls, !typing, !editingZoom else { return }
        hideTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(6))
            if !Task.isCancelled { controls = false }
        }
    }
}

private struct ZoomEditingSurface: View {
    @Binding var editing: Bool
    @Binding var zoom: CGFloat
    @Binding var offset: CGSize
    let reveal: () -> Void
    let zoomChanged: (CGFloat) -> Void
    let zoomEnded: () -> Void
    let panChanged: (CGSize) -> Void
    let panEnded: () -> Void

    var body: some View {
        if editing {
            Color.clear.contentShape(Rectangle())
                .gesture(DragGesture().onChanged { panChanged($0.translation) }.onEnded { _ in panEnded() })
                .simultaneousGesture(MagnifyGesture().onChanged { zoomChanged($0.magnification) }.onEnded { _ in zoomEnded() })
                .overlay(alignment: .bottom) {
                    HStack {
                        Text("Pinch to zoom. Drag to pan.").font(.footnote)
                        Button("Reset") { zoom = 1; offset = .zero }
                        Button("Done") { editing = false; reveal() }
                    }
                    .padding(12).background(.regularMaterial, in: Capsule()).padding(.bottom, 20)
                }
        }
    }
}

private struct SessionDetails: View {
    @ObservedObject var vm: VMViewModel
    @Binding var scaling: GuestDisplayGeometry.Scaling
    let relative: Bool
    let leave: () -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section("Performance") {
                    LabeledContent("RAM requested", value: vm.hardwareSummary)
                    LabeledContent("Available host memory", value: "\(vm.availableMemoryMB) MiB")
                    LabeledContent("Requested JIT cache", value: vm.requestedJITSummary)
                    LabeledContent("Mapped JIT cache", value: "\(WinIOSBridge.shared.mappedJITMiB) MiB")
                    LabeledContent("JIT", value: vm.jitStatusText)
                }
                Section("Display") {
                    LabeledContent("Guest resolution", value: vm.fbWidth > 0 ? "\(vm.fbWidth) × \(vm.fbHeight)" : "Unavailable")
                    Picker("Scaling", selection: $scaling) {
                        ForEach(GuestDisplayGeometry.Scaling.allCases, id: \.self) { Text($0.title).tag($0) }
                    }
                    LabeledContent("Frames presented", value: vm.presentedFrames.formatted())
                }
                Section("Input") {
                    LabeledContent("Touch mode", value: relative ? "Relative trackpad" : "Direct")
                    LabeledContent("Guest USB keyboard", value: vm.usbKeyboardReady ? "Connected" : "Waiting")
                    LabeledContent("Guest USB tablet", value: vm.usbTabletReady ? "Connected" : "Waiting")
                }
                Section("Sharing") {
                    LabeledContent("Shared folder", value: WinIOSBridge.shared.guestShareAvailable ? "Available" : "Unavailable")
                    Text("On My iPad or On My iPhone → WindowsiOS-ez → Shared. In Windows, use a WebDAV client with http://10.0.2.4/.")
                }
                Section {
                    NavigationLink("Diagnostics") { DiagnosticsView(vm: vm) }
                    Button("Back to Machines") { dismiss(); leave() }
                }
            }
            .navigationTitle("Session Details")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
    }
}
