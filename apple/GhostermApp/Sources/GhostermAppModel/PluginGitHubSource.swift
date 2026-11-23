import Foundation

/// Canonical public GitHub source accepted by the plugin installer.
public struct PluginGitHubSource: Codable, Hashable, Sendable {
    public let owner: String
    public let repository: String
    public let plugin: String?
    public let tag: String?

    enum Failure: Error, Equatable {
        case invalidAddress
        case unsupportedHost
        case invalidOwner
        case invalidRepository
        case invalidPlugin
        case invalidTag
    }

    public init(_ input: String) throws {
        var value = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty else { throw Failure.invalidAddress }
        if value.hasPrefix("https://") || value.hasPrefix("http://") {
            guard value.hasPrefix("https://github.com/") else { throw Failure.unsupportedHost }
            value.removeFirst("https://github.com/".count)
            value = value.split(separator: "?", maxSplits: 1).first.map(String.init) ?? value
            value = value.split(separator: "#", maxSplits: 1).first.map(String.init) ?? value
            while value.hasSuffix("/") { value.removeLast() }
        }
        let pieces = value.split(separator: ":", maxSplits: 1, omittingEmptySubsequences: false)
        let path = String(pieces[0])
        let suffix = pieces.count == 2 ? String(pieces[1]) : nil
        let pathParts = path.split(separator: "/", omittingEmptySubsequences: false)
        guard pathParts.count == 2 else { throw Failure.invalidAddress }
        let owner = String(pathParts[0]), repositoryAndPlugin = String(pathParts[1])
        guard Self.validSegment(owner) else { throw Failure.invalidOwner }
        let repoParts = repositoryAndPlugin.split(separator: "@", maxSplits: 1, omittingEmptySubsequences: false)
        let repo = String(repoParts[0])
        var tag = repoParts.count == 2 ? String(repoParts[1]) : nil
        var plugin: String? = nil
        if let suffix {
            let pluginParts = suffix.split(separator: "@", maxSplits: 1, omittingEmptySubsequences: false)
            plugin = String(pluginParts[0])
            if pluginParts.count == 2 { tag = String(pluginParts[1]) }
            guard let plugin, Self.validSegment(plugin) else { throw Failure.invalidPlugin }
        }
        guard Self.validSegment(repo), !repo.hasSuffix(".git") else { throw Failure.invalidRepository }
        if let tag { guard Self.validSegment(tag) else { throw Failure.invalidTag } }
        self.owner = owner; self.repository = repo; self.plugin = plugin; self.tag = tag
    }

    private static func validSegment(_ value: String) -> Bool {
        !value.isEmpty && value.utf8.count <= 128 && value.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || "-_ .".contains($0)) && $0 != " " }
    }
    public var repositoryPath: String { "\(owner)/\(repository)" }
    public func indexURL() -> URL {
        let base = "https://github.com/\(repositoryPath)/releases/"
        let path = tag.map { "download/\($0)/plugin-index.json" } ?? "latest/download/plugin-index.json"
        return URL(string: base + path)!
    }
    public func assetURL(named name: String, tag: String) -> URL? {
        guard !name.isEmpty, !name.contains(".."), !name.contains("/") else { return nil }
        return URL(string: "https://github.com/\(repositoryPath)/releases/download/\(tag)/\(name)")
    }
}

public struct PluginIndex: Codable, Sendable {
    public let schemaVersion: Int
    public let repository: String
    public let releaseTag: String
    public let publisher: String
    public let publisherFingerprint: String
    public let publisherPublicKey: String
    public let plugins: [PluginIndexEntry]
    public let signatureDomain: String
    public let signature: String

    func validate(for source: PluginGitHubSource) throws {
        guard schemaVersion == 1, repository == source.repositoryPath,
              source.tag == nil || source.tag == releaseTag,
              !publisher.isEmpty, publisherFingerprint.count == 64, Data(base64Encoded: publisherPublicKey)?.count == 32,
              signatureDomain == "ghosterm.plugin-index.v1", !signature.isEmpty,
              !plugins.isEmpty, plugins.allSatisfy({ $0.validate() }) else { throw PluginGitHubSource.Failure.invalidAddress }
    }
}

public struct PluginIndexEntry: Codable, Sendable {
    public let id: String
    public let name: String
    public let version: String
    public let role: String
    public let archive: String
    public let releaseJSON: String
    public let releaseSignature: String
    public let archiveBytes: UInt64
    public let archiveSHA256: String
    let permissions: [String]
    let capabilities: [String]

    func validate() -> Bool {
        !id.isEmpty && !name.isEmpty && !version.isEmpty && ["client", "executor", "both"].contains(role) &&
        archiveBytes > 0 && archiveSHA256.count == 64 && !archive.contains("/") && !releaseJSON.contains("/") && !releaseSignature.contains("/")
    }
}
