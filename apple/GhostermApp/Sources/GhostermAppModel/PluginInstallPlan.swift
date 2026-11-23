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

extension PluginInstallPlan {
    static func from(index: PluginIndex, source: PluginGitHubSource, selectedIDs: Set<String>, targets: [String: String?] = [:], pinRelease: Bool) throws -> PluginInstallPlan {
        let selected = index.plugins.filter { selectedIDs.contains($0.id) }
        guard !selected.isEmpty else { throw PluginGitHubSource.Failure.invalidAddress }
        let items = try selected.map { entry -> Item in
            guard let role = Role(rawValue: entry.role) else { throw PluginGitHubSource.Failure.invalidAddress }
            return Item(id: entry.id, version: entry.version, role: role, archiveBytes: entry.archiveBytes,
                        archiveSHA256: entry.archiveSHA256, permissions: entry.permissions, capabilities: entry.capabilities,
                        target: targets[entry.id] ?? nil, fixed: pinRelease)
        }
        let plan = PluginInstallPlan(source: source, releaseTag: index.releaseTag, publisher: index.publisher,
                                     publisherFingerprint: index.publisherFingerprint, items: items, overwriteIDs: [], pinRelease: pinRelease)
        guard plan.isValid else { throw PluginGitHubSource.Failure.invalidAddress }
        return plan
    }
}
