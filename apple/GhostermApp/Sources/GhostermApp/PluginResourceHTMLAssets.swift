import Foundation
import CryptoKit
import GhostermAppModel
import WebKit

/// Paths identify snapshot bytes, not linguistically equivalent text. ASCII
/// separators must also stay visible when followed by a combining scalar.
private enum PluginResourcePath {
    static func canonical(_ bytes: Data) -> Bool {
        guard !bytes.isEmpty, bytes.count <= 1024, bytes.first != 47,
              !bytes.contains(92), !bytes.contains(0) else { return false }
        for part in bytes.split(separator: 47, omittingEmptySubsequences: false) {
            if part.isEmpty || part.elementsEqual([46]) || part.elementsEqual([46, 46]) { return false }
        }
        return true
    }

    static func parentPrefix(_ path: String) -> Data? {
        let bytes = Data(path.utf8)
        guard canonical(bytes) else { return nil }
        guard let slash = bytes.lastIndex(of: 47) else { return Data() }
        return Data(bytes.prefix(through: slash))
    }
}

struct PluginResourceHTMLAssetResolver: Sendable {
    struct Asset: Sendable {
        let data: Data
        let mime: String
    }

    private let directory: Data?
    private let assets: [Data: Asset]
    let omittedAttachments: Bool

    var fingerprint: Data {
        var digest = SHA256()
        for key in assets.keys.sorted(by: { $0.lexicographicallyPrecedes($1) }) {
            guard let asset = assets[key] else { continue }
            for bytes in [key, Data(asset.mime.utf8), asset.data] {
                var length = UInt64(bytes.count).bigEndian
                withUnsafeBytes(of: &length) { digest.update(bufferPointer: $0) }
                digest.update(data: bytes)
            }
        }
        return Data(digest.finalize())
    }

    init(document: ExtensionController.ReceivedResource,
         resources: [ExtensionController.ReceivedResource],
         transform: ((ExtensionController.ReceivedResource) -> Asset?)? = nil) {
        directory = PluginResourcePath.parentPrefix(document.displayName)
        var candidates: [Data: Asset] = [:]
        var seen = Set<Data>()
        var omitted = false
        let attached = PluginResourcePreviewAssets.after(document, in: resources)
        for resource in attached {
            let key = Data(resource.displayName.utf8)
            guard let directory, PluginResourcePath.canonical(key), key.starts(with: directory) else {
                omitted = true; continue
            }
            guard seen.insert(key).inserted else {
                candidates.removeValue(forKey: key)
                omitted = true; continue
            }
            guard Self.isAllowedMime(resource.mime), resource.size <= 1_048_576,
                  UInt64(resource.bytes.count) == resource.size else { omitted = true; continue }
            if let transform {
                guard let asset = transform(resource) else { omitted = true; continue }
                candidates[key] = asset
            } else { candidates[key] = Asset(data: resource.bytes, mime: Self.normalizedMime(resource.mime)) }
        }
        assets = candidates
        omittedAttachments = omitted
    }

    func resolve(_ url: URL) -> Asset? {
        guard url.scheme == "ghosterm-resource", url.host == "preview",
              url.user == nil, url.password == nil, url.port == nil,
              let directory, let path = URLComponents(url: url, resolvingAgainstBaseURL: false)?.path else { return nil }
        let bytes = Data(path.utf8)
        guard bytes.first == 47 else { return nil }
        let relative = Data(bytes.dropFirst())
        guard PluginResourcePath.canonical(relative), relative.starts(with: directory) else { return nil }
        return assets[relative]
    }

    static func documentURL(_ path: String) -> URL? {
        let bytes = Data(path.utf8)
        guard PluginResourcePath.canonical(bytes) else { return nil }
        let allowed = CharacterSet.urlPathAllowed.subtracting(CharacterSet(charactersIn: "/?#%"))
        let encoded = bytes.split(separator: 47, omittingEmptySubsequences: false)
            .map { String(decoding: $0, as: UTF8.self).addingPercentEncoding(withAllowedCharacters: allowed) ?? "" }
            .joined(separator: "/")
        return URL(string: "ghosterm-resource://preview/\(encoded)")
    }

    private static func normalizedMime(_ mime: String) -> String {
        mime.split(separator: ";", maxSplits: 1).first
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() } ?? ""
    }

    private static func isAllowedMime(_ mime: String) -> Bool {
        let normalized = normalizedMime(mime)
        return normalized == "text/css" || [
            "image/png", "image/jpeg", "image/gif", "image/webp",
        ].contains(normalized)
    }

}

enum PluginResourcePreviewAssets {
    static func after(_ document: ExtensionController.ReceivedResource,
                      in resources: [ExtensionController.ReceivedResource])
        -> [ExtensionController.ReceivedResource] {
        guard let index = resources.firstIndex(where: { $0.resourceID == document.resourceID }),
              let directory = PluginResourcePath.parentPrefix(document.displayName) else {
            return []
        }
        return resources.dropFirst(index + 1)
            .prefix(while: { !isDocument($0) })
            .filter { $0.displayName.utf8.starts(with: directory) }
            .prefix(6)
            .map { $0 }
    }

    private static func isDocument(_ resource: ExtensionController.ReceivedResource) -> Bool {
        PluginResourceHTMLDecoder.supports(mime: resource.mime) ||
            PluginResourceMarkdownDecoder.supports(mime: resource.mime, displayName: resource.displayName)
    }

}

enum PluginResourceHTMLNavigationPolicy {
    static func permitsAction(type: WKNavigationType, isMainFrame: Bool, url: URL?,
                              documentURL: URL?) -> Bool {
        guard isMainFrame, type == .other || type == .reload, let url else { return false }
        if url.absoluteString == "about:blank" { return true }
        return documentURL != nil && url == documentURL
    }

    static func permitsResponse(url: URL?, isMainFrame: Bool, documentURL: URL?) -> Bool {
        guard isMainFrame, let url else { return false }
        return url.absoluteString == "about:blank" || (documentURL != nil && url == documentURL)
    }
}

@MainActor
enum PluginResourceHTMLWebViewConfiguration {
    static func make(schemeHandler: PluginResourceHTMLSchemeHandler) -> WKWebViewConfiguration {
        let configuration = WKWebViewConfiguration()
        // Resource snapshots belong to this preview, never the application's
        // persistent WebKit store or another preview's in-memory store.
        configuration.websiteDataStore = .nonPersistent()
        configuration.defaultWebpagePreferences.allowsContentJavaScript = false
        configuration.preferences.javaScriptCanOpenWindowsAutomatically = false
        configuration.setURLSchemeHandler(schemeHandler, forURLScheme: "ghosterm-resource")
        return configuration
    }
}

@MainActor
final class PluginResourceHTMLCoordinator: NSObject, WKNavigationDelegate, WKUIDelegate {
    private var closed = false
    private var resourceFingerprint: Data?
    private var baseURL: URL?
    let schemeHandler: PluginResourceHTMLSchemeHandler

    init(snapshot: PluginResourceHTMLSnapshot) {
        schemeHandler = PluginResourceHTMLSchemeHandler(snapshot: snapshot)
        super.init()
    }

    func update(snapshot: PluginResourceHTMLSnapshot, in webView: WKWebView) {
        guard !closed else { return }
        let target = PluginResourceHTMLAssetResolver.documentURL(snapshot.resource.displayName)
        guard resourceFingerprint != snapshot.fingerprint, let target else { return }
        resourceFingerprint = snapshot.fingerprint
        schemeHandler.update(snapshot: snapshot)
        baseURL = target
        webView.loadHTMLString(snapshot.document, baseURL: target)
    }

    func close(in webView: WKWebView) {
        guard !closed else { return }
        closed = true
        webView.stopLoading()
        webView.navigationDelegate = nil
        webView.uiDelegate = nil
        schemeHandler.close()
        resourceFingerprint = nil
        baseURL = nil
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        let allowed = !closed && PluginResourceHTMLNavigationPolicy.permitsAction(
            type: navigationAction.navigationType,
            isMainFrame: navigationAction.targetFrame?.isMainFrame == true,
            url: navigationAction.request.url, documentURL: baseURL)
        decisionHandler(allowed ? .allow : .cancel)
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationResponse: WKNavigationResponse,
                 decisionHandler: @escaping (WKNavigationResponsePolicy) -> Void) {
        let allowed = !closed && PluginResourceHTMLNavigationPolicy.permitsResponse(
            url: navigationResponse.response.url, isMainFrame: navigationResponse.isForMainFrame,
            documentURL: baseURL)
        decisionHandler(allowed ? .allow : .cancel)
    }

    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
                 for navigationAction: WKNavigationAction,
                 windowFeatures: WKWindowFeatures) -> WKWebView? {
        nil
    }
}

final class PluginResourceHTMLSchemeHandler: NSObject, WKURLSchemeHandler, @unchecked Sendable {
    private let lock = NSLock()
    private var resolver: PluginResourceHTMLAssetResolver?

    init(snapshot: PluginResourceHTMLSnapshot) {
        resolver = snapshot.resolver
        super.init()
    }

    func update(snapshot: PluginResourceHTMLSnapshot) {
        lock.lock()
        defer { lock.unlock() }
        guard resolver != nil else { return }
        resolver = snapshot.resolver
    }

    func close() {
        lock.lock()
        resolver = nil
        lock.unlock()
    }

    func webView(_ webView: WKWebView, start urlSchemeTask: WKURLSchemeTask) {
        lock.lock()
        let requestedURL = urlSchemeTask.request.url
        let asset = requestedURL.flatMap { resolver?.resolve($0) }
        lock.unlock()
        guard let asset, let url = requestedURL else {
            urlSchemeTask.didFailWithError(URLError(.fileDoesNotExist))
            return
        }
        guard let response = HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1",
                                             headerFields: [
                                                 "Content-Type": asset.mime,
                                                 "Cache-Control": "no-store, no-cache, must-revalidate",
                                                 "Content-Length": String(asset.data.count),
                                             ]) else {
            urlSchemeTask.didFailWithError(URLError(.badServerResponse))
            return
        }
        urlSchemeTask.didReceive(response)
        urlSchemeTask.didReceive(asset.data)
        urlSchemeTask.didFinish()
    }

    func webView(_ webView: WKWebView, stop urlSchemeTask: WKURLSchemeTask) {}
}
