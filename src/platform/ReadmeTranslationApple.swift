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
    let parts: [String]
    let samples: [Int]
    let labels: [String: Any]
    @Published var configuration: TranslationSession.Configuration?
    let target: String
    var source = ""
    var supportedSources = Set<String>()
    var index = 0
    var output = ReadmeTranslationOutput(maximumBytes: 4 * 1024 * 1024)
    let callback: TranslationCallback
    var session: TranslationSession?
    var finished = false
#if os(iOS)
    var controller: UIViewController?
#else
    var panel: NSPanel?
#endif
    init(id: UInt64, parts: [String], labels: [String: Any], target: String,
         callback: @escaping TranslationCallback) {
        self.id = id
        self.parts = parts
        self.samples = labels["samples"] as? [Int] ?? Array(parts.indices)
        self.labels = labels
        self.callback = callback
        self.target = target
    }
    private func configure(source: String) {
        self.source = source
        let language = Locale.Language(identifier: source)
        let destination = Locale.Language(identifier: target)
        if #available(iOS 26.4, macOS 26.4, *) {
            configuration = .init(source: language, target: destination, preferredStrategy: .highFidelity)
        } else {
            configuration = .init(source: language, target: destination)
        }
    }
    // Each session has one source language. Switch its configuration at language
    // boundaries instead of letting an English heading determine the whole file.
    private func nextSource() async throws -> String? {
        while index < parts.count && !finished {
            try Task.checkCancellation()
            let part = parts[index]
            if part.unicodeScalars.contains(where: { CharacterSet.letters.contains($0) }),
               let language = NLLanguageRecognizer.dominantLanguage(for: parts[samples[index]])?.rawValue,
               language != target,
               let code = Locale.Language(identifier: language).languageCode?.identifier,
               supportedSources.contains(code) {
                return language
            }
            guard append(part) else { return nil }
            index += 1
            await Task.yield()
        }
        if !finished { finish(2, output.text) }
        return nil
    }
    private func append(_ text: String) -> Bool {
        guard output.append(text) else { finish(3); return false }
        return true
    }
    func begin() async {
        do {
            let availability: LanguageAvailability
            if #available(iOS 26.4, macOS 26.4, *) {
                availability = LanguageAvailability(preferredStrategy: .highFidelity)
            } else { availability = LanguageAvailability() }
            let languages = await availability.supportedLanguages
            try Task.checkCancellation()
            guard !finished else { return }
            supportedSources = Set(languages.compactMap { $0.languageCode?.identifier })
            guard !supportedSources.isEmpty else { finish(3); return }
            if let source = try await nextSource() { configure(source: source) }
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
            while !finished {
                guard let language = try await nextSource() else { return }
                if language != source {
                    self.session = nil
                    configure(source: language)
                    return
                }
                guard append(try await session.translate(parts[index]).targetText) else { return }
                index += 1
            }
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

@_cdecl("ReadmeAppleTranslate")
func readmeAppleTranslate(_ id: UInt64, _ json: UnsafePointer<CChar>, _ language: UnsafePointer<CChar>,
                         _ callback: @escaping @convention(c) (UInt64, Int32, UnsafePointer<CChar>?) -> Void) {
    let data = Data(String(cString: json).utf8)
    let target = String(cString: language)
    DispatchQueue.main.async {
        guard #available(iOS 18.0, macOS 15.0, *),
              let labels = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let parts = labels["parts"] as? [String] else { callback(id, 3, nil); return }
        ReadmeTranslationHost.current?.finish(4)
        let host = ReadmeTranslationHost(id: id, parts: parts, labels: labels, target: target, callback: callback)
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
