import Foundation
import SwiftUI
import Translation
import NaturalLanguage
#if os(iOS)
import UIKit
#else
import AppKit
#endif

private typealias TranslationCallback = @convention(c) (UInt64, Int32, UnsafePointer<CChar>?) -> Void

@available(iOS 18.0, macOS 15.0, *)
@MainActor
private final class ReadmeTranslationHost: ObservableObject {
    static var current: ReadmeTranslationHost?
    let id: UInt64
    let input: String
    let labels: [String: Any]
    @Published var configuration: TranslationSession.Configuration?
    let target: String
    var output = ReadmeTranslationOutput(maximumBytes: 4 * 1024 * 1024)
    let callback: TranslationCallback
    var session: TranslationSession?
    var finished = false
#if os(iOS)
    var controller: UIViewController?
#else
    var panel: NSPanel?
#endif
    init(id: UInt64, input: String, labels: [String: Any], target: String,
         callback: @escaping TranslationCallback) {
        self.id = id
        self.input = input
        self.labels = labels
        self.callback = callback
        self.target = target
    }
    @available(iOS 26.4, macOS 26.4, *)
    private var preferredStrategy: TranslationSession.Strategy {
        (labels["highFidelity"] as? Bool ?? false) ? .highFidelity : .lowLatency
    }
    private func configure(source: String) {
        let language = Locale.Language(identifier: source)
        let destination = Locale.Language(identifier: target)
        if #available(iOS 26.4, macOS 26.4, *) {
            configuration = .init(source: language, target: destination, preferredStrategy: preferredStrategy)
        } else {
            configuration = .init(source: language, target: destination)
        }
    }
    private func append(_ text: String) -> Bool {
        guard output.append(text) else { finish(3); return false }
        return true
    }
    func begin() async {
        do {
            let availability: LanguageAvailability
            if #available(iOS 26.4, macOS 26.4, *) {
                availability = LanguageAvailability(preferredStrategy: preferredStrategy)
            } else { availability = LanguageAvailability() }
            let languages = await availability.supportedLanguages
            try Task.checkCancellation()
            guard !finished else { return }
            let selected = labels["source"] as? String ?? "auto"
            let detected = selected == "auto"
                ? NLLanguageRecognizer.dominantLanguage(for: input)?.rawValue
                : selected
            guard let source = detected,
                  let code = Locale.Language(identifier: source).languageCode?.identifier,
                  languages.contains(where: { $0.languageCode?.identifier == code }) else { finish(3); return }
            if Locale.Language(identifier: source) == Locale.Language(identifier: target) {
                finish(2, input)
            } else { configure(source: source) }
        } catch { finish(4) }
    }
    func label(_ name: String) -> String { labels[name] as? String ?? name }
    func publish(_ state: Int32, _ text: String = "") {
        guard !finished else { return }
        text.withCString { callback(id, state, $0) }
    }
    func finish(_ state: Int32, _ text: String = "") {
        guard !finished else { return }
        if state == 2 && text.utf8.count > 4 * 1024 * 1024 { publish(3) }
        else { publish(state, text) }
        finished = true
        if #available(iOS 26.0, macOS 26.0, *) { session?.cancel() }
        session = nil
#if os(iOS)
        controller?.dismiss(animated: true)
        controller = nil
#else
        if let panel {
            panel.sheetParent?.endSheet(panel)
            panel.orderOut(nil)
        }
        panel = nil
#endif
        if Self.current === self { Self.current = nil }
    }
    func present() {
        let view = ReadmeTranslationProgress(host: self)
#if os(iOS)
        let sceneWindows = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            .filter { $0.activationState == .foregroundActive }.flatMap { $0.windows }
        // SDL also supports the legacy application lifecycle without a scene manifest.
        let window = sceneWindows.first(where: { $0.isKeyWindow }) ??
            UIApplication.shared.windows.first(where: { $0.isKeyWindow })
        guard var presenter = window?.rootViewController else { finish(3); return }
        while let presented = presenter.presentedViewController { presenter = presented }
        let controller = UIHostingController(rootView: view)
        controller.isModalInPresentation = true
        controller.modalPresentationStyle = .formSheet
        self.controller = controller
        presenter.present(controller, animated: true)
#else
        guard let parent = NSApp.keyWindow ?? NSApp.mainWindow ??
                NSApp.windows.first(where: { $0.isVisible && $0.canBecomeMain }) else { finish(3); return }
        let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 420, height: 180),
                            styleMask: [.titled], backing: .buffered, defer: false)
        panel.title = label("translate")
        panel.contentViewController = NSHostingController(rootView: view)
        self.panel = panel
        parent.beginSheet(panel)
#endif
    }
    func run(_ session: TranslationSession) async {
        guard !finished else { return }
        self.session = session
        do {
            try Task.checkCancellation()
            try await session.prepareTranslation()
            publish(1)
            let translated = try await session.translate(input).targetText
            if append(translated) { finish(2, output.text) }
        } catch is CancellationError {
            finish(4)
        } catch {
            NSLog("Readme translation failed: %@", String(describing: error))
            finish(3)
        }
    }
}

@available(iOS 18.0, macOS 15.0, *)
private struct ReadmeTranslationProgress: View {
    @ObservedObject var host: ReadmeTranslationHost
    var body: some View {
        VStack(spacing: 20) {
            ProgressView()
            Text(host.label("preparing")).multilineTextAlignment(.center)
            Button(host.label("cancel")) { host.finish(4) }
        }
        .padding(24)
        .frame(minWidth: 300, minHeight: 150)
        .task { await host.begin() }
        .translationTask(host.configuration) { session in await host.run(session) }
    }
}

@_cdecl("ReadmeAppleTranslationSupported")
func readmeAppleTranslationSupported() -> Bool {
    if #available(iOS 18.0, macOS 15.0, *) { return true }
    return false
}

@_cdecl("ReadmeAppleTranslationQualitySupported")
func readmeAppleTranslationQualitySupported() -> Bool {
    if #available(iOS 26.4, macOS 26.4, *) { return true }
    return false
}

@_cdecl("ReadmeAppleTranslate")
func readmeAppleTranslate(_ id: UInt64, _ json: UnsafePointer<CChar>, _ language: UnsafePointer<CChar>,
                         _ callback: @escaping @convention(c) (UInt64, Int32, UnsafePointer<CChar>?) -> Void) {
    let data = Data(String(cString: json).utf8)
    let target = String(cString: language)
    DispatchQueue.main.async {
        guard #available(iOS 18.0, macOS 15.0, *),
              let labels = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let input = labels["text"] as? String else { callback(id, 3, nil); return }
        ReadmeTranslationHost.current?.finish(4)
        let host = ReadmeTranslationHost(id: id, input: input, labels: labels, target: target, callback: callback)
        ReadmeTranslationHost.current = host
        host.present()
    }
}

@_cdecl("ReadmeAppleTranslationStop")
func readmeAppleTranslationStop(_ id: UInt64) {
    DispatchQueue.main.async {
        if #available(iOS 18.0, macOS 15.0, *), let host = ReadmeTranslationHost.current, host.id == id {
            host.finish(4)
        }
    }
}
