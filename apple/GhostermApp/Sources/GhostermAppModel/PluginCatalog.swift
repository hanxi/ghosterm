import Foundation

/// Signed-catalog payload kept separate from release contents. Release bytes remain authoritative.
public struct PluginCatalogEntry: Codable, Hashable, Sendable {
    public let source: PluginGitHubSource
    public let displayName: String
    public let defaultRole: String
    public let publisherFingerprint: String
    public let sortOrder: Int
    public let automaticUpdates: Bool
}

public enum PluginCatalog {
    public static let builtIn: [PluginCatalogEntry] = [
        .init(source: try! PluginGitHubSource("hanxi/ghosterm:preview"), displayName: "官方 Preview", defaultRole: "both", publisherFingerprint: "", sortOrder: 10, automaticUpdates: true),
        .init(source: try! PluginGitHubSource("hanxi/ghosterm:table"), displayName: "官方 Table", defaultRole: "client", publisherFingerprint: "", sortOrder: 20, automaticUpdates: true)
    ]
}
