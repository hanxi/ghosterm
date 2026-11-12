import Foundation
import GhostermAppModel
import WebKit

struct PluginResourceHTMLAssetResolver {
    struct Asset {
        let data: Data
        let mime: String
    }

    private let directory: String
    private let assets: [String: Asset]

    init(document: ExtensionController.ReceivedResource,
         resources: [ExtensionController.ReceivedResource]) {
        let parent = Self.parentPath(document.displayName)
        directory = parent.isEmpty ? "" : parent + "/"
        var candidates: [String: Asset] = [:]
        var duplicatePaths = Set<String>()
        let attached = PluginResourcePreviewAssets.after(document, in: resources)
        for resource in attached {
            guard resource.displayName.hasPrefix(directory),
                  Self.isAllowedMime(resource.mime), resource.size <= 1_048_576,
                  UInt64(resource.bytes.count) == resource.size else { continue }
            let key = resource.displayName
            guard candidates[key] == nil else {
                candidates.removeValue(forKey: key)
                duplicatePaths.insert(key)
                continue
            }
            guard !duplicatePaths.contains(key) else { continue }
            candidates[key] = Asset(data: resource.bytes, mime: Self.normalizedMime(resource.mime))
        }
        assets = candidates
    }

    func resolve(_ url: URL) -> Asset? {
        guard url.scheme == "ghosterm-resource", url.host == "preview",
              url.user == nil, url.password == nil, url.port == nil,
              let path = URLComponents(url: url, resolvingAgainstBaseURL: false)?.path,
              path.hasPrefix("/") else { return nil }
        let relative = String(path.dropFirst())
        guard Self.isCanonical(relative), relative.hasPrefix(directory) else { return nil }
        return assets[relative]
    }

    static func documentURL(_ path: String) -> URL? {
        guard isCanonical(path) else { return nil }
        let allowed = CharacterSet.urlPathAllowed.subtracting(CharacterSet(charactersIn: "/?#%"))
        let encoded = path.split(separator: "/", omittingEmptySubsequences: false)
            .map { String($0).addingPercentEncoding(withAllowedCharacters: allowed) ?? "" }
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

    private static func parentPath(_ path: String) -> String {
        guard isCanonical(path), let slash = path.lastIndex(of: "/") else { return "" }
        return String(path[..<slash])
    }

    private static func isCanonical(_ path: String) -> Bool {
        !path.isEmpty && path.utf8.count <= 1024 && !path.hasPrefix("/") &&
            !path.contains("\\") && !path.unicodeScalars.contains(where: { $0.value == 0 }) &&
            !path.split(separator: "/", omittingEmptySubsequences: false)
                .contains(where: { $0.isEmpty || $0 == "." || $0 == ".." })
    }
}

enum PluginResourcePreviewAssets {
    static func after(_ document: ExtensionController.ReceivedResource,
                      in resources: [ExtensionController.ReceivedResource])
        -> [ExtensionController.ReceivedResource] {
        guard let index = resources.firstIndex(where: { $0.resourceID == document.resourceID }) else {
            return []
        }
        return resources.dropFirst(index + 1)
            .prefix(while: { !isDocument($0) })
            .filter { $0.displayName.hasPrefix(parentPrefix(document.displayName)) }
            .prefix(6)
            .map { $0 }
    }

    private static func isDocument(_ resource: ExtensionController.ReceivedResource) -> Bool {
        PluginResourceHTMLDecoder.supports(mime: resource.mime) ||
            PluginResourceMarkdownDecoder.supports(mime: resource.mime, displayName: resource.displayName)
    }

    private static func parentPrefix(_ path: String) -> String {
        guard let slash = path.lastIndex(of: "/") else { return "" }
        return String(path[...slash])
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

final class PluginResourceHTMLSchemeHandler: NSObject, WKURLSchemeHandler, @unchecked Sendable {
    private let lock = NSLock()
    private var resolver: PluginResourceHTMLAssetResolver

    init(document: ExtensionController.ReceivedResource,
         resources: [ExtensionController.ReceivedResource]) {
        resolver = PluginResourceHTMLAssetResolver(document: document, resources: resources)
        super.init()
    }

    func update(document: ExtensionController.ReceivedResource,
                resources: [ExtensionController.ReceivedResource]) {
        let replacement = PluginResourceHTMLAssetResolver(document: document, resources: resources)
        lock.lock()
        resolver = replacement
        lock.unlock()
    }

    func webView(_ webView: WKWebView, start urlSchemeTask: WKURLSchemeTask) {
        lock.lock()
        let requestedURL = urlSchemeTask.request.url
        let asset = requestedURL.flatMap(resolver.resolve)
        lock.unlock()
        guard let asset, let url = requestedURL else {
            urlSchemeTask.didFailWithError(URLError(.fileDoesNotExist))
            return
        }
        let response = URLResponse(url: url, mimeType: asset.mime,
                                   expectedContentLength: asset.data.count,
                                   textEncodingName: asset.mime == "text/css" ? "utf-8" : nil)
        urlSchemeTask.didReceive(response)
        urlSchemeTask.didReceive(asset.data)
        urlSchemeTask.didFinish()
    }

    func webView(_ webView: WKWebView, stop urlSchemeTask: WKURLSchemeTask) {}
}
