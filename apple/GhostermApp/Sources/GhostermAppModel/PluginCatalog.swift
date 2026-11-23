import Foundation

/// Signed-catalog payload kept separate from release contents. Release bytes remain authoritative.
struct PluginCatalogEntry: Codable, Hashable, Sendable {
    let source: PluginGitHubSource
    let displayName: String
    let defaultRole: String
    let publisherFingerprint: String
    let sortOrder: Int
    let automaticUpdates: Bool
}

enum PluginCatalog {
    static let builtIn: [PluginCatalogEntry] = [
        .init(source: try! PluginGitHubSource("hanxi/ghosterm-preview"), displayName: "官方 Preview", defaultRole: "both", publisherFingerprint: "", sortOrder: 10, automaticUpdates: true),
        .init(source: try! PluginGitHubSource("hanxi/ghosterm-table"), displayName: "官方 Table", defaultRole: "client", publisherFingerprint: "", sortOrder: 20, automaticUpdates: true)
    ]
}
