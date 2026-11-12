import Foundation

/// Parses only explicitly identified Markdown resources. HTML remains a separate
/// format and is never interpreted as Markdown or executable web content.
public enum PluginResourceMarkdownDecoder {
    public static let maximumBytes = 262_144

    public static func supports(mime: String, displayName: String) -> Bool {
        let normalizedMIME = mime.split(separator: ";", maxSplits: 1).first
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() } ?? ""
        let ext = URL(fileURLWithPath: displayName).pathExtension.lowercased()
        return normalizedMIME == "text/markdown" || normalizedMIME == "text/x-markdown"
            || ext == "md" || ext == "markdown"
    }

    public static func decode(_ data: Data, mime: String, displayName: String,
                              baseURL: URL? = nil) -> AttributedString? {
        guard supports(mime: mime, displayName: displayName), data.count <= maximumBytes,
              let text = String(data: data, encoding: .utf8) else { return nil }
        return try? AttributedString(markdown: text, options: .init(interpretedSyntax: .full),
                                     baseURL: baseURL)
    }
}
