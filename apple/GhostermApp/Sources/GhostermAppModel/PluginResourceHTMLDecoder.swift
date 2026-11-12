import Foundation

/// Prepares a bounded HTML document for a native, script-disabled preview.
/// The web view must still enforce the returned document's CSP and block navigation.
public enum PluginResourceHTMLDecoder {
    public static let maximumBytes = 262_144

    public static func supports(mime: String) -> Bool {
        let normalized = mime.split(separator: ";", maxSplits: 1).first
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() } ?? ""
        return normalized == "text/html"
    }

    public static func document(_ data: Data, mime: String) -> String? {
        guard supports(mime: mime), data.count <= maximumBytes,
              let source = String(data: data, encoding: .utf8) else { return nil }
        let policy = "default-src 'none'; script-src 'none'; style-src 'self'; img-src 'self'; " +
            "font-src 'none'; media-src 'none'; connect-src 'none'; object-src 'none'; " +
            "frame-src 'none'; child-src 'none'; form-action 'none'; base-uri 'none'"
        return "<!doctype html><html><head><meta charset=\"utf-8\">" +
            "<meta http-equiv=\"Content-Security-Policy\" content=\"\(policy)\"></head><body>" +
            source + "</body></html>"
    }
}
