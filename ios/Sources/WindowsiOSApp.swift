// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

@main
struct WindowsiOSApp: App {
    @UIApplicationDelegateAdaptor(WindowsDownloadAppDelegate.self) private var downloadDelegate
    @StateObject private var viewModel = VMViewModel()
    @Environment(\.scenePhase) private var scenePhase

    private static let recoverStorage: Void = {
        let repository = VMFiles.repository
        removeAbandonedStaging(in: repository.root)
        if let machines = try? repository.list() {
            for machine in machines {
                removeAbandonedStaging(in: repository.directory(for: machine))
            }
        }
    }()

    init() {
        _ = Self.recoverStorage
    }

    var body: some Scene {
        WindowGroup {
            WindowsLibraryView(vm: viewModel)
                .onAppear {
                    viewModel.onAppBecameActive()
                }
                .onChange(of: scenePhase) { _, newPhase in
                    if newPhase == .active {
                        viewModel.onAppBecameActive()
                    } else if newPhase == .background {
                        WinIOSBridge.shared.flushDisks()
                    }
                }
        }
    }
}
