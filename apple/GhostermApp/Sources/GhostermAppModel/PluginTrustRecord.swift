import Foundation

struct PluginTrustRecord: Codable, Sendable, Equatable, Identifiable {
    let id: String
    let repository: String
    let releaseTag: String
    let pluginID: String
    let version: String
    let role: PluginInstallPlan.Role
    let publisherFingerprint: String
    let archiveSHA256: String
    let releaseJSONSHA256: String
    let installedAt: Date
    var pinned: Bool
    var lastCheckedAt: Date?

    var isValid: Bool {
        !repository.isEmpty && !releaseTag.isEmpty && !pluginID.isEmpty && version.isEmpty == false &&
        publisherFingerprint.count == 64 && archiveSHA256.count == 64 && releaseJSONSHA256.count == 64
    }
}
