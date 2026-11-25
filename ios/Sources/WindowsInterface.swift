// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Centered content and quiet grouped cards, informed by Henge's Desk layout.
/// Original Windows controls and platform colors keep this interface distinct.
enum WindowsAppearance: String, CaseIterable {
    case system, light, dark

    var scheme: ColorScheme? {
        switch self {
        case .system: nil
        case .light: .light
        case .dark: .dark
        }
    }
}

extension View {
    func windowsColumn() -> some View {
        frame(maxWidth: 700).frame(maxWidth: .infinity)
    }

    func windowsCard() -> some View {
        padding(16)
            .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 20))
    }
}

struct WindowsMark: View {
    var body: some View {
        Grid(horizontalSpacing: 2, verticalSpacing: 2) {
            GridRow { Rectangle(); Rectangle() }
            GridRow { Rectangle(); Rectangle() }
        }
        .foregroundStyle(.tint)
        .frame(width: 25, height: 25)
        .accessibilityHidden(true)
    }
}

struct AppSettingsView: View {
    let vm: VMViewModel
    @AppStorage("appearance") private var appearance = WindowsAppearance.system
    @AppStorage("session.hideHostPointer") private var hidePointer = true
    @AppStorage("session.relativeInput") private var relative = false
    @AppStorage("session.sensitivity") private var sensitivity = 1.0
    @AppStorage("session.scrollSpeed") private var scrollSpeed = 1.0
    @AppStorage("session.naturalScroll") private var naturalScroll = true
    @AppStorage("session.autoHide") private var autoHide = false
    @AppStorage("session.toolbarTop") private var toolbarTop = true
    @AppStorage("session.defaultScaling") private var scaling = GuestDisplayGeometry.Scaling.fit
    @AppStorage("session.frameRate") private var frameRate = 0

    var body: some View {
        Form {
            Section("Appearance") {
                Picker("Theme", selection: $appearance) {
                    ForEach(WindowsAppearance.allCases, id: \.self) { Text($0.rawValue.capitalized).tag($0) }
                }
            }
            Section("Input") {
                Toggle("Hide iPadOS Pointer over Windows", isOn: $hidePointer)
                Toggle("Use Relative Touch by Default", isOn: $relative)
                Toggle("Natural Scrolling", isOn: $naturalScroll)
                LabeledContent("Pointer speed") { Slider(value: $sensitivity, in: 0.5...2, step: 0.1).accessibilityLabel("Pointer speed") }
                LabeledContent("Scroll speed") { Slider(value: $scrollSpeed, in: 0.5...2, step: 0.1).accessibilityLabel("Scroll speed") }
            }
            Section("Session") {
                Toggle("Automatically Hide Controls", isOn: $autoHide)
                Toggle("Toolbar at Top in Landscape", isOn: $toolbarTop)
                Picker("Default Scaling", selection: $scaling) {
                    ForEach(GuestDisplayGeometry.Scaling.allCases, id: \.self) { Text($0.title).tag($0) }
                }
                Picker("Presentation Limit", selection: $frameRate) {
                    Text("Automatic").tag(0)
                    Text("30 FPS").tag(30)
                    Text("60 FPS").tag(60)
                    Text("Up to 120 FPS").tag(120)
                }
            }
            Section("Engine") {
                NavigationLink("JIT Setup and Pairing") { JITSettingsView(vm: vm) }
                NavigationLink("Diagnostics") { DiagnosticsView(vm: vm) }
                NavigationLink("Storage") { ApplicationStorageView() }
            }
            Section("About") {
                if let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String {
                    LabeledContent("Version", value: version)
                }
                LabeledContent("Build commit", value: Bundle.main.object(forInfoDictionaryKey: "WINIOSBuildCommit") as? String ?? "Unavailable")
                Text("GPL-2.0-or-later")
                Link("WindowsiOS-ez on GitHub", destination: URL(string: "https://github.com/hazerbvisor/WindowsiOS-ez")!)
                Link("WindowsiOS upstream", destination: URL(string: "https://github.com/ammaarreshi/WindowsiOS")!)
                Link("Henge design reference", destination: URL(string: "https://github.com/Leviidev/Henge")!)
            }
        }
        .windowsColumn()
        .background(Color(uiColor: .systemGroupedBackground))
        .navigationTitle("Settings")
    }
}
