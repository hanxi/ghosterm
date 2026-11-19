import Foundation
import Security
import XCTest
@testable import GhostermAppModel

private final class LoopbackChannelDelegate: PluginChannelRefresh.Redirects, @unchecked Sendable {
    private let anchor: SecCertificate

    init(anchor: SecCertificate) {
        self.anchor = anchor
    }

    override func urlSession(_ session: URLSession, didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        guard challenge.protectionSpace.host == "127.0.0.1",
              challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
              let trust = challenge.protectionSpace.serverTrust,
              SecTrustSetAnchorCertificates(trust, [anchor] as CFArray) == errSecSuccess,
              SecTrustSetAnchorCertificatesOnly(trust, true) == errSecSuccess,
              SecTrustSetPolicies(trust, SecPolicyCreateSSL(true, challenge.protectionSpace.host as CFString)) == errSecSuccess else {
            XCTFail("unexpected loopback TLS challenge: \(challenge.protectionSpace.authenticationMethod) at \(challenge.protectionSpace.host)")
            completionHandler(.cancelAuthenticationChallenge, nil)
            return
        }
        var error: CFError?
        guard SecTrustEvaluateWithError(trust, &error) else {
            XCTFail("fixture TLS trust failed: \(String(describing: error))")
            completionHandler(.cancelAuthenticationChallenge, nil)
            return
        }
        completionHandler(.useCredential, URLCredential(trust: trust))
    }
}

final class PluginChannelTLSLoopbackTests: XCTestCase {
    func testHTTPSRefreshAndFailurePreserveCache() async throws {
        let env = ProcessInfo.processInfo.environment
        try XCTSkipIf(env["GHOSTERM_TEST_CHANNEL_ORIGIN"] == nil, "requires the owned loopback TLS fixture")
        let origin = try XCTUnwrap(env["GHOSTERM_TEST_CHANNEL_ORIGIN"])
        let root = try XCTUnwrap(env["GHOSTERM_TEST_CHANNEL_ROOT"].flatMap { Data(base64Encoded: $0) })
        let bytes = try XCTUnwrap(env["GHOSTERM_TEST_CHANNEL_CERT"].flatMap { Data(base64Encoded: $0) })
        let certificate = try XCTUnwrap(SecCertificateCreateWithData(nil, bytes as CFData))
        let state = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: state, withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: state) }

        func refresh(_ route: String) async throws -> PluginDirectorySignatureVerifier.VerifiedChannelPolicy {
            let endpoint = try XCTUnwrap(URL(string: origin + "/" + route + "/"))
            let trust = ExtensionController.ChannelTrust(channel: "fixture", root: root, stateDirectory: state,
                                                         refreshDirectory: endpoint)
            return try await PluginChannelRefresh.refresh(trust, delegate: LoopbackChannelDelegate(anchor: certificate))
        }
        let vectorPath = try XCTUnwrap(env["GHOSTERM_TEST_CHANNEL_RELEASE_VECTOR"])
        let vector = try JSONDecoder().decode([String: String].self, from: Data(contentsOf: URL(fileURLWithPath: vectorPath)))
        let descriptor = Data(try XCTUnwrap(vector["descriptor"]).utf8)
        let signature = try XCTUnwrap(vector["signature"].flatMap { Data(base64Encoded: $0) })
        let identity = try PluginDirectorySignatureVerifier.declaration(descriptor)
        func verify(_ policy: PluginDirectorySignatureVerifier.VerifiedChannelPolicy) throws {
            _ = try PluginDirectorySignatureVerifier.verify(descriptor: descriptor, signature: signature,
                channelPolicy: policy, identity: identity, now: UInt64(Date().timeIntervalSince1970))
        }
        let valid = try await refresh("valid")
        XCTAssertEqual(valid.generation, 2)
        try verify(valid)
        let untrustedEndpoint = try XCTUnwrap(URL(string: origin + "/revoked/"))
        let untrusted = ExtensionController.ChannelTrust(channel: "fixture", root: root, stateDirectory: state,
                                                         refreshDirectory: untrustedEndpoint)
        do {
            _ = try await PluginChannelRefresh.refresh(untrusted)
            XCTFail("production channel client trusted the uninstalled loopback certificate")
        } catch let error as URLError {
            XCTAssertEqual(error.code, .serverCertificateUntrusted)
        }
        do {
            _ = try await refresh("redirect")
            XCTFail("channel client followed a redirect")
        } catch PluginChannelRefresh.Failure.response {}
        do {
            _ = try await refresh("oversized")
            XCTFail("channel client accepted an oversized response")
        } catch PluginChannelRefresh.Failure.length {}
        do {
            _ = try await refresh("mixed")
            XCTFail("channel client accepted mixed statement and policy bytes")
        } catch PluginDirectorySignatureVerifier.Failure.channelStatement {}
        do {
            _ = try await refresh("missing-policy")
            XCTFail("channel client accepted a release statement without its policy")
        } catch PluginChannelRefresh.Failure.response {}
        let restored = try PluginDirectorySignatureVerifier.loadChannelPolicy(channel: "fixture", channelRoot: root,
            now: UInt64(Date().timeIntervalSince1970), stateDirectory: state)
        XCTAssertEqual(restored.statementSHA256, valid.statementSHA256)
        let revoked = try await refresh("revoked")
        XCTAssertEqual(revoked.generation, 3)
        XCTAssertThrowsError(try verify(revoked)) {
            XCTAssertEqual($0 as? PluginDirectorySignatureVerifier.Failure, .revokedKey)
        }
        do {
            _ = try await refresh("valid")
            XCTFail("channel client accepted a generation preceding the revocation")
        } catch PluginDirectorySignatureVerifier.Failure.channelHistory {}
        let reopened = try PluginDirectorySignatureVerifier.loadChannelPolicy(channel: "fixture", channelRoot: root,
            now: UInt64(Date().timeIntervalSince1970), stateDirectory: state)
        XCTAssertEqual(reopened.statementSHA256, revoked.statementSHA256)
        XCTAssertThrowsError(try verify(reopened)) {
            XCTAssertEqual($0 as? PluginDirectorySignatureVerifier.Failure, .revokedKey)
        }
    }
}
