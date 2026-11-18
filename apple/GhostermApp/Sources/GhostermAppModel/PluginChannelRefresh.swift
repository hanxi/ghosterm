import Foundation

/// Fetches the public materials for a host-configured channel.
public enum PluginChannelRefresh {
    enum Failure: LocalizedError {
        case endpoint, response, length
        var errorDescription: String? {
            switch self {
            case .endpoint: return "插件渠道缺少有效的 HTTPS 刷新地址。"
            case .response: return "插件渠道未返回所需的签名材料。"
            case .length: return "插件渠道返回的签名材料长度无效。"
            }
        }
    }

    private final class Redirects: NSObject, URLSessionTaskDelegate {
        func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                        newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
            completionHandler(nil)
        }
    }

    public static func refresh(_ trust: ExtensionController.ChannelTrust) async throws
        -> PluginDirectorySignatureVerifier.VerifiedChannelPolicy {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.urlCache = nil
        configuration.httpCookieStorage = nil
        configuration.urlCredentialStorage = nil
        configuration.timeoutIntervalForRequest = 15
        configuration.timeoutIntervalForResource = 30
        let session = URLSession(configuration: configuration, delegate: Redirects(), delegateQueue: nil)
        defer { session.invalidateAndCancel() }
        return try await refresh(trust, session: session)
    }

    static func refresh(_ trust: ExtensionController.ChannelTrust, session: URLSession) async throws
        -> PluginDirectorySignatureVerifier.VerifiedChannelPolicy {
        guard let directory = trust.refreshDirectory, let parts = URLComponents(url: directory, resolvingAgainstBaseURL: false),
              parts.scheme?.lowercased() == "https", let host = parts.host, !host.isEmpty,
              parts.user == nil, parts.password == nil, parts.query == nil, parts.fragment == nil,
              trust.root.count == 32 else { throw Failure.endpoint }
        async let statement = fetch("channel-policy.json", directory: directory, limit: 65_536, session: session)
        async let signature = fetch("channel-policy.sig", directory: directory, limit: 64, session: session)
        async let policy = fetch("policy.json", directory: directory, limit: 131_072, session: session)
        let (statementBytes, signatureBytes, policyBytes) = try await (statement, signature, policy)
        try Task.checkCancellation()
        let seconds = Date().timeIntervalSince1970
        guard seconds >= 0, seconds < Double(UInt64.max) else {
            throw PluginDirectorySignatureVerifier.Failure.channelFreshness
        }
        return try PluginDirectorySignatureVerifier.refreshChannelPolicy(statement: statementBytes, signature: signatureBytes,
            policy: policyBytes, channel: trust.channel, channelRoot: trust.root, now: UInt64(seconds),
            stateDirectory: trust.stateDirectory)
    }

    private static func fetch(_ name: String, directory: URL, limit: Int, session: URLSession) async throws -> Data {
        let url = directory.appendingPathComponent(name, isDirectory: false)
        let request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData)
        let (bytes, response) = try await session.bytes(for: request)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200, http.url == url else { throw Failure.response }
        guard response.expectedContentLength <= limit else { throw Failure.length }
        var result = Data()
        result.reserveCapacity(max(0, Int(response.expectedContentLength)))
        for try await byte in bytes {
            guard result.count < limit else { throw Failure.length }
            result.append(byte)
        }
        return result
    }
}
