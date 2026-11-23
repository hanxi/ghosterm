import Foundation
import GhostermCore

public struct PluginGitHubClient: Sendable {
    enum Failure: Error, LocalizedError, Equatable {
        case invalidStatus(Int)
        case responseTooLarge
        case invalidResponse
        var errorDescription: String? {
            switch self { case .invalidStatus(let code): return "GitHub 返回 HTTP \(code)。"; case .responseTooLarge: return "GitHub 响应超过允许大小。"; case .invalidResponse: return "GitHub 返回内容无效。" }
        }
    }
    public var session: URLSession = .shared
    public var maxBytes: Int
    public init(session: URLSession = .shared, maxBytes: Int = 4 * 1024 * 1024) { self.session = session; self.maxBytes = maxBytes }
    public func fetch(_ url: URL) async throws -> Data {
        var request = URLRequest(url: url); request.httpMethod = "GET"; request.setValue("application/json", forHTTPHeaderField: "Accept")
        let (bytes, response) = try await session.bytes(for: request)
        guard let http = response as? HTTPURLResponse else { throw Failure.invalidResponse }
        guard (200..<300).contains(http.statusCode) else { throw Failure.invalidStatus(http.statusCode) }
        var data = Data(); data.reserveCapacity(min(maxBytes, 65536))
        for try await byte in bytes {
            guard data.count < maxBytes else { throw Failure.responseTooLarge }
            data.append(byte)
        }
        return data
    }
    public func fetchIndex(for source: PluginGitHubSource) async throws -> PluginIndex {
        let raw = try await fetch(source.indexURL())
        let index = try JSONDecoder().decode(PluginIndex.self, from: raw)
        try index.validate(for: source)
        guard let key = Data(base64Encoded: index.publisherPublicKey), let signature = Data(base64Encoded: index.signature),
              let statement = Self.signedStatement(index, source: source), ReleaseSignature.verify(publicKey: key, signature: signature, descriptor: statement) == 0 else {
            throw PluginGitHubSource.Failure.invalidAddress
        }
        return index
    }
}

private extension PluginGitHubClient {
    static func signedStatement(_ index: PluginIndex, source: PluginGitHubSource) -> Data? {
        var value = index
        value = PluginIndex(schemaVersion: index.schemaVersion, repository: index.repository, releaseTag: index.releaseTag, publisher: index.publisher, publisherFingerprint: index.publisherFingerprint, publisherPublicKey: index.publisherPublicKey, plugins: index.plugins, signatureDomain: index.signatureDomain, signature: "")
        return try? JSONEncoder().encode(value)
    }
}

extension PluginGitHubClient {
    struct VerifiedAssets: Sendable {
        let archive: Data
        let releaseJSON: Data
        let releaseSignature: Data
    }
    public func fetchAssets(for entry: PluginIndexEntry, source: PluginGitHubSource, releaseTag: String) async throws -> VerifiedAssets {
        guard let archiveURL = source.assetURL(named: entry.archive, tag: releaseTag),
              let descriptorURL = source.assetURL(named: entry.releaseJSON, tag: releaseTag),
              let signatureURL = source.assetURL(named: entry.releaseSignature, tag: releaseTag) else { throw PluginGitHubSource.Failure.invalidAddress }
        async let archive = fetch(archiveURL)
        async let descriptor = fetch(descriptorURL)
        async let signature = fetch(signatureURL)
        let (a, d, s) = try await (archive, descriptor, signature)
        guard UInt64(a.count) == entry.archiveBytes, PluginReleaseCatalog.sha(a) == entry.archiveSHA256,
              d.count > 0 && d.count <= 65536, s.count == 64 else { throw PluginGitHubSource.Failure.invalidAddress }
        return VerifiedAssets(archive: a, releaseJSON: d, releaseSignature: s)
    }
}
