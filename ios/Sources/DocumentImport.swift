// SPDX-License-Identifier: GPL-2.0-or-later
import Observation
import OSLog
import SwiftUI
import UniformTypeIdentifiers
import UIKit

extension DocumentImportKind {
    var types: [UTType] {
        switch self {
        case .machine, .drivers: [.folder]
        case .iso: [UTType(filenameExtension: "iso") ?? .data, .data]
        case .disk, .firmware, .pairing: [.data]
        }
    }
}

struct DocumentPickerRequest: Identifiable {
    enum Operation {
        case open(DocumentImportKind)
        case exportCopy(URL)
    }

    let id = UUID()
    let operation: Operation

    init(kind: DocumentImportKind) { operation = .open(kind) }
    init(exporting url: URL) { operation = .exportCopy(url) }

    var label: String {
        switch operation {
        case .open(let kind): kind.rawValue
        case .exportCopy: "export"
        }
    }
}

/// Keeps the provider grant alive from the picker callback through background coordination.
final class ImportSelection: Sendable {
    let url: URL
    let scoped: Bool

    init(_ url: URL) {
        self.url = url
        scoped = url.startAccessingSecurityScopedResource()
    }

    deinit {
        if scoped { url.stopAccessingSecurityScopedResource() }
    }
}

@Observable @MainActor
final class ImportDiagnostics {
    static let shared = ImportDiagnostics()
    private let logger = Logger(subsystem: Bundle.main.bundleIdentifier ?? "io.github.hazerbvisor.windowsios-ez",
                                category: "DocumentImport")
    private(set) var events: [String] = []

    func record(_ event: String) {
        logger.info("\(event, privacy: .public)")
        events.append("\(Date().ISO8601Format()) \(event)")
        if events.count > 100 { events.removeFirst(events.count - 100) }
    }
}

extension View {
    func documentPicker(_ request: Binding<DocumentPickerRequest?>,
                        exported: @escaping @MainActor (Bool) -> Void = { _ in },
                        selected: @escaping @MainActor (DocumentImportKind, ImportSelection) -> Void) -> some View {
        background {
            DocumentPickerAnchor(request: request.wrappedValue) { response, selection, success in
                request.wrappedValue = nil
                switch response.operation {
                case .open(let kind):
                    if let selection { selected(kind, selection) }
                case .exportCopy:
                    exported(success)
                }
            }
            .frame(width: 1, height: 1)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
        }
    }
}

private struct DocumentPickerAnchor: UIViewControllerRepresentable {
    let request: DocumentPickerRequest?
    let finished: @MainActor (DocumentPickerRequest, ImportSelection?, Bool) -> Void

    func makeUIViewController(context: Context) -> DocumentPickerPresenter {
        DocumentPickerPresenter()
    }

    func updateUIViewController(_ controller: DocumentPickerPresenter, context: Context) {
        controller.finished = finished
        controller.enqueue(request)
    }
}

/// UIKit owns presentation. This anchor owns the picker and its delegate for the entire session.
private final class DocumentPickerPresenter: UIViewController,
                                             UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate {
    var finished: (@MainActor (DocumentPickerRequest, ImportSelection?, Bool) -> Void)?
    private var queued: DocumentPickerRequest?
    private var consumed: UUID?
    private var active: DocumentPickerRequest?
    private var picker: UIDocumentPickerViewController?
    private var settling = false
    private var logs: ImportDiagnostics { .shared }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        showQueuedPicker()
    }

    func enqueue(_ request: DocumentPickerRequest?) {
        guard let request, request.id != consumed, request.id != queued?.id else { return }
        queued = request
        logs.record("\(request.label) picker requested")
        Task { @MainActor [weak self] in self?.showQueuedPicker() }
    }

    private func showQueuedPicker() {
        guard let request = queued, active == nil,
              let root = viewIfLoaded?.window?.rootViewController else { return }
        var presenter = root
        while let presented = presenter.presentedViewController {
            presenter = presented
        }
        if let transition = presenter.transitionCoordinator, transition.isAnimated,
           transition.animate(alongsideTransition: nil, completion: { [weak self] _ in
               Task { @MainActor in self?.showQueuedPicker() }
           }) { return }
        guard !presenter.isBeingDismissed, presenter.viewIfLoaded?.window != nil else { return }

        let picker: UIDocumentPickerViewController
        switch request.operation {
        case .open(let kind):
            picker = UIDocumentPickerViewController(forOpeningContentTypes: kind.types, asCopy: false)
        case .exportCopy(let url):
            picker = UIDocumentPickerViewController(forExporting: [url], asCopy: true)
        }
        picker.allowsMultipleSelection = false
        picker.delegate = self
        picker.presentationController?.delegate = self
        self.picker = picker
        active = request
        consumed = request.id
        queued = nil
        logs.record("\(request.label) picker presenting")
        presenter.present(picker, animated: true) {
            picker.presentationController?.delegate = self
            self.logs.record("\(request.label) picker presented")
        }
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        guard controller === picker, let active, !settling else { return }
        let selection: ImportSelection?
        switch active.operation {
        case .open: selection = urls.first.map(ImportSelection.init)
        case .exportCopy: selection = nil
        }
        logs.record("\(active.label) picker callback (\(urls.count) URL, scoped \(selection?.scoped == true))")
        settle(selection, success: !urls.isEmpty)
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        guard controller === picker, let active, !settling else { return }
        logs.record("\(active.label) picker cancelled")
        settle(nil)
    }

    func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
        guard presentationController.presentedViewController === picker, let active, !settling else { return }
        logs.record("\(active.label) picker dismissed interactively")
        settle(nil)
    }

    private func settle(_ selection: ImportSelection?, success: Bool = false) {
        guard let picker, let request = active else { return }
        settling = true
        // A provider may start dismissal before or after the delegate callback.
        Task { @MainActor in
            let complete = {
                self.active = nil
                self.picker = nil
                self.settling = false
                self.logs.record("\(request.label) picker closed")
                self.finished?(request, selection, success)
            }
            if picker.isBeingDismissed, let transition = picker.transitionCoordinator,
               transition.animate(alongsideTransition: nil, completion: { state in
                   if state.isCancelled && picker.presentingViewController != nil {
                       picker.dismiss(animated: true, completion: complete)
                   } else {
                       complete()
                   }
               }) { return }
            if picker.presentingViewController != nil, !picker.isBeingDismissed {
                picker.dismiss(animated: true, completion: complete)
            } else {
                complete()
            }
        }
    }
}
