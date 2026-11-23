import Foundation

struct PluginInstallPlan: Codable, Sendable, Equatable {
    enum Role: String, Codable, Sendable { case client, executor, both }
    struct Item: Codable, Sendable, Equatable, Identifiable {
        let id: String
        let version: String
        let role: Role
        let archiveBytes: UInt64
        let archiveSHA256: String
        let permissions: [String]
        let capabilities: [String]
        var target: String?
        var fixed: Bool
        var idValue: String { id }
    }
    let source: PluginGitHubSource
    let releaseTag: String
    let publisher: String
    let publisherFingerprint: String
    let items: [Item]
    let overwriteIDs: [String]
    let pinRelease: Bool

    var isValid: Bool {
        !items.isEmpty && items.allSatisfy { $0.archiveSHA256.count == 64 && ($0.role != .executor || $0.target != nil) } &&
        Set(items.map(\.id)).count == items.count && (!pinRelease || !releaseTag.isEmpty)
    }
}
