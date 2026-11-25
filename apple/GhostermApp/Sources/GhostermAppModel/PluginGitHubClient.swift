import Foundation
import GhostermCore

public struct PluginGitHubClient: Sendable {
    enum Failure: Error, LocalizedError, Equatable {
        case invalidStatus(Int)
        case indexNotFound(repository: String, tag: String?)
        case network(host: String, code: URLError.Code)
        case responseTooLarge
        case invalidResponse
        var errorDescription: String? {
            switch self {
            case .invalidStatus(let code):
                return "GitHub 返回 HTTP \(code)。"
            case .indexNotFound(let repository, let tag):
                let release = tag.map { "Release \($0)" } ?? "最新正式 Release"
                return "未找到 \(repository) 的插件索引。请确认 \(release) 已公开发布并包含 plugin-index.json（HTTP 404）。"
            case .network(let host, let code):
                switch code {
                case .notConnectedToInternet:
                    return "当前未连接互联网。请联网后重试。"
                case .cannotFindHost, .dnsLookupFailed:
                    return "无法解析 \(host)。请检查 DNS 或代理设置后重试（\(code.rawValue)）。"
                case .cannotConnectToHost, .networkConnectionLost, .timedOut:
                    return "无法连接 \(host) 或连接已中断。请检查网络或代理设置后重试（\(code.rawValue)）。"
                default:
                    return "访问 \(host) 失败（\(code.rawValue)）。"
                }
            case .responseTooLarge:
                return "GitHub 响应超过允许大小。"
            case .invalidResponse:
                return "GitHub 返回内容无效。"
            }
        }
    }
    public var session: URLSession = .shared
    public var maxBytes: Int
    public init(session: URLSession = .shared, maxBytes: Int = 4 * 1024 * 1024) { self.session = session; self.maxBytes = maxBytes }
    public func fetch(_ url: URL) async throws -> Data {
        var request = URLRequest(url: url); request.httpMethod = "GET"; request.setValue("application/json", forHTTPHeaderField: "Accept")
        do {
            let (bytes, response) = try await session.bytes(for: request)
            guard let http = response as? HTTPURLResponse else { throw Failure.invalidResponse }
            guard (200..<300).contains(http.statusCode) else { throw Failure.invalidStatus(http.statusCode) }
            var data = Data(); data.reserveCapacity(min(maxBytes, 65536))
            for try await byte in bytes {
                guard data.count < maxBytes else { throw Failure.responseTooLarge }
                data.append(byte)
            }
            return data
        } catch let error as URLError where error.code != .cancelled {
            let host = error.failingURL?.host ?? url.host ?? "GitHub"
            throw Failure.network(host: host, code: error.code)
        }
    }
    public func fetchIndex(for source: PluginGitHubSource) async throws -> PluginIndex {
        let raw: Data
        do {
            raw = try await fetch(source.indexURL())
        } catch Failure.invalidStatus(404) {
            throw Failure.indexNotFound(repository: source.repositoryPath, tag: source.tag)
        }
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
    public struct VerifiedAssets: Sendable {
        public let archive: Data
        public let releaseJSON: Data
        public let releaseSignature: Data
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
