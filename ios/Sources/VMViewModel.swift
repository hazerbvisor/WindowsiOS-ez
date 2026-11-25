// SPDX-License-Identifier: GPL-2.0-or-later
import Combine
import Foundation
import SafariServices
import SwiftUI
import UIKit

struct IdentifiableURL: Identifiable {
    let id = UUID()
    let url: URL
}

struct SafariSheet: UIViewControllerRepresentable {
    let url: URL
    func makeUIViewController(context: Context) -> SFSafariViewController {
        let vc = SFSafariViewController(url: url)
        vc.preferredBarTintColor = UIColor(red: 0.045, green: 0.058, blue: 0.096, alpha: 1.0)
        vc.preferredControlTintColor = UIColor(red: 0.82, green: 0.90, blue: 1.0, alpha: 1.0)
        vc.dismissButtonStyle = .done
        return vc
    }
    func updateUIViewController(_ uiViewController: SFSafariViewController, context: Context) {}
}

@MainActor
final class VMViewModel: ObservableObject {
    @Published var isJITLive: Bool = false
    @Published var isDebuggerAttached: Bool = false
    @Published var isVMRunning: Bool = false
    @Published var needsRelaunchToStart = false
    @Published private(set) var installationSession: WindowsInstallationMethod?

    var bootActionTitle: String {
        switch installationSession {
        case .direct: "START DISM PROVISIONING"
        case .graphical: "START GRAPHICAL SETUP"
        case nil: "BOOT WINDOWS 11"
        }
    }
    @Published var isUnpacking: Bool = false
    @Published var unpackProgress: Double = 0
    @Published var jitStatusText: String = "Checking JIT..."
    @Published var vmStatusText: String = "Initializing..."
    @Published var fbWidth: UInt32 = 0
    @Published var fbHeight: UInt32 = 0
    @Published var presentedFrames: UInt64 = 0
    @Published var availableMemoryMB: UInt64 = 0

    @Published var usbKeyboardReady: Bool = false
    @Published var usbTabletReady: Bool = false
    @Published var usbEventsDelivered: UInt64 = 0

    @Published var activeSafariURL: IdentifiableURL? = nil
    @Published var clipboardError: String?
    @Published var useSafariForOAuth: Bool = WinIOSBridge.shared.useSafariForGuestLinks

    private var bootDirectory: URL?
    private var bootInstaller: URL?
    private var bootUsesTPM = false
    private var bootHardware: VMHardware?
    private(set) var initialScaling: GuestDisplayGeometry.Scaling = .fit

    var hardwareSummary: String {
        guard let bootHardware else { return "Unavailable" }
        return bootHardware.ramMiB == 0 ? "Automatic" : "\(bootHardware.ramMiB) MiB"
    }

    var requestedJITSummary: String {
        bootHardware.map { "\($0.jitMiB) MiB requested" } ?? "Unavailable"
    }
    private var oauthPollTimer: Timer? = nil

    init() {
        let bridge = WinIOSBridge.shared
        bridge.installTrapGuard()
        bridge.onStateChanged = { [weak self] in
            Task { @MainActor in
                self?.syncFromBridge()
            }
        }
        syncFromBridge()
        startOAuthPoller()
        Task { @MainActor [weak self] in
            self?.onAppBecameActive()
        }
    }

    private func startOAuthPoller() {
        oauthPollTimer?.invalidate()
        oauthPollTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                let b = WinIOSBridge.shared
                if b.takeGuestOAuthDone() {
                    if self.activeSafariURL != nil {
                        UINotificationFeedbackGenerator().notificationOccurred(.success)
                        self.activeSafariURL = nil
                    }
                }
                if let urlStr = b.takeGuestOpenURL(), let u = URL(string: urlStr) {
                    UIImpactFeedbackGenerator(style: .medium).impactOccurred()
                    self.activeSafariURL = IdentifiableURL(url: u)
                }
            }
        }
    }

    func toggleOAuthBrowserMode() {
        useSafariForOAuth.toggle()
        WinIOSBridge.shared.useSafariForGuestLinks = useSafariForOAuth
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
    }

    func syncFromBridge() {
        let b = WinIOSBridge.shared
        isJITLive = b.isJITLive
        isDebuggerAttached = b.isDebuggerAttached
        isVMRunning = b.isVMRunning
        needsRelaunchToStart = b.needsRelaunchToStart
        isUnpacking = b.isUnpacking
        unpackProgress = b.unpackProgress
        jitStatusText = b.jitStatusText
        vmStatusText = b.vmStatusText
        fbWidth = b.framebufferWidth
        fbHeight = b.framebufferHeight
        presentedFrames = b.presentedFrames
        availableMemoryMB = b.availableMemoryMB
        usbKeyboardReady = b.usbKeyboardReady
        usbTabletReady = b.usbTabletReady
        usbEventsDelivered = b.usbEventsDelivered
    }

    func onAppBecameActive() {
        let b = WinIOSBridge.shared
        if let bootHardware, !b.isJITLive && b.isDebuggerAttached {
            _ = b.tryAcquireJIT(detachAfter: true, jitMiB: UInt64(bootHardware.jitMiB),
                                ramMiB: UInt64(bootHardware.ramMiB))
        }
        syncFromBridge()
    }

    func open(directory: URL?, hardware: VMHardware, installer: URL? = nil, useTPM: Bool = false,
              method: WindowsInstallationMethod? = nil, scaling: GuestDisplayGeometry.Scaling = .fit) {
        initialScaling = scaling
        bootDirectory = directory
        bootInstaller = installer
        bootUsesTPM = useTPM || installer != nil
        installationSession = installer == nil ? nil : (method ?? .graphical)
        bootHardware = hardware
        startOrRetry()
    }

    func startOrRetry() {
        guard let bootHardware else { return }
        WinIOSBridge.shared.start(directory: bootDirectory?.path, installer: bootInstaller?.path, useTPM: bootUsesTPM, ramMiB: UInt64(bootHardware.ramMiB),
                                  jitMiB: UInt64(bootHardware.jitMiB), cpus: Int32(bootHardware.vCPUs))
        syncFromBridge()
    }

    func saveDesktopPreview() {
        guard let bootDirectory else { return }
        WinIOSBridge.shared.saveDesktopPreview(to: bootDirectory.appendingPathComponent(VMRepository.preview))
    }

    func requestStikDebugJIT() {
        guard let bundleID = Bundle.main.bundleIdentifier,
              let script = Bundle.main.url(forResource: "henge-jit", withExtension: "js"),
              let data = try? Data(contentsOf: script) else {
            jitStatusText = "The bundled JIT debugger script could not be loaded."
            return
        }
        let scriptData = data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .trimmingCharacters(in: CharacterSet(charactersIn: "="))
        var components = URLComponents()
        components.scheme = "stikjit"
        components.host = "enable-jit"
        components.queryItems = [
            URLQueryItem(name: "bundle-id", value: bundleID),
            URLQueryItem(name: "pid", value: String(getpid())),
            URLQueryItem(name: "script-data", value: scriptData),
        ]
        guard let url = components.url else { return }
        UIApplication.shared.open(url, options: [:]) { [weak self] opened in
            if !opened {
                Task { @MainActor [weak self] in
                    self?.jitStatusText = "StikDebug did not open. Install and pair the authorized debugger, then try again."
                }
            }
        }
    }
}
