import CryptoKit
import Darwin
import Foundation
import XCTest
@testable import GhostermAppModel
@testable import GhostermApp
import GhostermCore

final class PluginDirectorySignatureTests: XCTestCase {
    private typealias Verifier = PluginDirectorySignatureVerifier
    private typealias Failure = Verifier.Failure
    // Disposable private keys stay in CryptoKit memory; only public material is serialized.
    private struct Fixture {
        let key = Curve25519.Signing.PrivateKey()
        var identity = Verifier.Identity(id: "org.ghosterm.signed", version: "0.1.0",
            publisher: "Fixture", packageSHA256: String(repeating: "a", count: 64))
        var keyID: String { hash(key.publicKey.rawRepresentation) }
        var policy: [String: Any] { ["schemaVersion": 1, "channel": "test", "keys": [
            ["id": keyID, "algorithm": "ed25519", "publicKey": key.publicKey.rawRepresentation.base64EncodedString(),
             "publisher": "Fixture", "pluginIds": [identity.id]]], "revokedKeyIds": []] }
        var descriptor: [String: Any] { ["schemaVersion": 1, "channel": "test", "keyId": keyID,
            "pluginId": identity.id, "pluginVersion": identity.version, "publisher": identity.publisher,
            "packageSHA256": identity.packageSHA256,
            "artifact": ["format": "zip-v1", "bytes": 22, "sha256": String(repeating: "b", count: 64)]] }
        func sign(_ data: Data, domain: String = "GHOSTERM-PLUGIN-RELEASE-V1\0") throws -> Data {
            try key.signature(for: Data(domain.utf8) + data)
        }
        func verify(payload: Data? = nil, signature: Data? = nil, policy: Data? = nil,
                    identity: Verifier.Identity? = nil) throws -> PluginDirectorySignature {
            let payload = try payload ?? json(descriptor)
            return try Verifier.verify(descriptor: payload, signature: signature ?? sign(payload),
                trustedPolicy: policy ?? json(self.policy), identity: identity ?? self.identity)
        }
    }
    private static func hash(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    private static func json(_ value: Any) throws -> Data { try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]) }

    func testRealSignatureBindsRawDescriptorAndDomain() throws {
        let f = Fixture(), payload = try Self.json(f.descriptor)
        let signature = try f.sign(payload), proof = try f.verify(payload: payload, signature: signature)
        XCTAssertEqual(proof.packageSHA256, f.identity.packageSHA256)
        XCTAssertEqual(proof.policySHA256, Self.hash(try Self.json(f.policy)))
        XCTAssertEqual(proof.descriptorSHA256, Self.hash(payload))
        XCTAssertEqual(proof.artifactBytes, 22)
        rejects(.signature) { try f.verify(payload: payload + Data([10]), signature: signature) }
        rejects(.signature) { try f.verify(payload: payload, signature: f.sign(payload, domain: "OTHER\0")) }
        rejects(.signature) { try f.verify(signature: Data(repeating: 0, count: 64)) }
    }

    func testPolicyRevocationAndScope() throws {
        let f = Fixture()
        var policy = f.policy
        policy["revokedKeyIds"] = [f.keyID]
        rejects(.revokedKey) { try f.verify(policy: Self.json(policy)) }
        policy = f.policy; policy["channel"] = "another"
        rejects(.scope) { try f.verify(policy: Self.json(policy)) }
        var keys = try XCTUnwrap(policy["keys"] as? [[String: Any]])
        policy = f.policy; keys[0]["publisher"] = "Other"; policy["keys"] = keys
        rejects(.scope) { try f.verify(policy: Self.json(policy)) }
        keys[0]["publisher"] = "Fixture"; keys[0]["pluginIds"] = ["org.ghosterm.other"]; policy["keys"] = keys
        rejects(.scope) { try f.verify(policy: Self.json(policy)) }
        var descriptor = f.descriptor; descriptor["keyId"] = String(repeating: "c", count: 64)
        rejects(.unknownKey) { try f.verify(payload: Self.json(descriptor)) }
    }

    func testChannelRootBindsPolicyBytesAndFreshnessAtReleaseVerification() throws {
        let f = Fixture(), root = Fixture()
        let policy = try Self.json(f.policy)
        let statement = try Self.json(["schemaVersion": 1, "channel": "test", "generation": "2",
                                       "issuedAt": "100", "expiresAt": "200", "policySHA256": Self.hash(policy)])
        let signature = try root.sign(statement, domain: "ghosterm.channel-policy.v1\0")
        func verified(_ policy: Data, _ signature: Data, now: UInt64 = 150) throws -> Verifier.VerifiedChannelPolicy {
            try Verifier.verifyChannelPolicy(statement: statement, signature: signature, policy: policy,
                                             channel: "test", channelRoot: root.key.publicKey.rawRepresentation, now: now)
        }
        let proof = try verified(policy, signature)
        XCTAssertEqual(proof.generation, 2)
        let descriptor = try Self.json(f.descriptor), releaseSignature = try f.sign(descriptor)
        func verifyRelease(now: UInt64) throws -> PluginDirectorySignature {
            try Verifier.verify(descriptor: descriptor, signature: releaseSignature, channelPolicy: proof,
                                identity: f.identity, now: now)
        }
        XCTAssertEqual(try verifyRelease(now: 199).policySHA256, Self.hash(policy))
        rejects(.channelFreshness) { try verifyRelease(now: 200) }
        rejects(.channelFreshness) { try verified(policy, signature, now: 99) }
        rejects(.channelStatement) { try verified(policy + Data([10]), signature) }
        rejects(.channelStatement) { try verified(policy, f.sign(statement, domain: "ghosterm.channel-policy.v1\0")) }
        rejects(.channelStatement) { try verified(policy, root.sign(statement)) }
    }

    func testIdentityCannotBeReplacedByCaller() throws {
        let f = Fixture()
        for identity in [
            Verifier.Identity(id: "org.ghosterm.other", version: "0.1.0", publisher: "Fixture", packageSHA256: f.identity.packageSHA256),
            .init(id: f.identity.id, version: "0.2.0", publisher: "Fixture", packageSHA256: f.identity.packageSHA256),
            .init(id: f.identity.id, version: "0.1.0", publisher: "Other", packageSHA256: f.identity.packageSHA256),
            .init(id: f.identity.id, version: "0.1.0", publisher: "Fixture", packageSHA256: String(repeating: "d", count: 64))
        ] { rejects(.identity) { try f.verify(identity: identity) } }
    }

    func testDuplicateEscapedAndReservedKeys() throws {
        let f = Fixture(), original = String(decoding: try Self.json(f.descriptor), as: UTF8.self)
        for prefix in [#"{"schemaVersion":1,"# , #"{"schema\u0056ersion":1,"#, #"{"__proto__":{},"#] {
            rejects(.duplicateKey) { try f.verify(payload: Data((prefix + original.dropFirst()).utf8)) }
        }
        let policy = String(decoding: try Self.json(f.policy), as: UTF8.self)
        rejects(.duplicateKey) { try f.verify(policy: Data((#"{"channel":"test","# + policy.dropFirst()).utf8)) }
    }

    func testExactIntegerLexemesAndBooleanRejection() throws {
        let f = Fixture(), raw = String(decoding: try Self.json(f.descriptor), as: UTF8.self)
        for value in ["1.0", "1e0", "100e-2"] {
            XCTAssertNoThrow(try f.verify(payload: Data(raw.replacingOccurrences(of: #""schemaVersion":1"#, with: #""schemaVersion":"# + value).utf8)))
        }
        for value in ["true", "1.00000000000000000001", "0.99999999999999999999", "1e9999999999", "1e-9999999999"] {
            rejects(.descriptor) { try f.verify(payload: Data(raw.replacingOccurrences(of: #""schemaVersion":1"#, with: #""schemaVersion":"# + value).utf8)) }
        }
        for value in ["22.00000000000000001", "75497473", "21", "true", "-22"] {
            rejects(.descriptor) { try f.verify(payload: Data(raw.replacingOccurrences(of: #""bytes":22"#, with: #""bytes":"# + value).utf8)) }
        }
    }

    func testStrictStringsAndKeyMaterial() throws {
        let f = Fixture()
        for field in ["keyId", "packageSHA256", "pluginId", "channel"] {
            var descriptor = f.descriptor
            descriptor[field] = (descriptor[field] as! String) + "\n"
            rejects(.descriptor) { try f.verify(payload: Self.json(descriptor)) }
        }
        var policy = f.policy, keys = f.policy["keys"] as! [[String: Any]]
        keys[0]["publicKey"] = (keys[0]["publicKey"] as! String) + "\n"; policy["keys"] = keys
        rejects(.policy) { try f.verify(policy: Self.json(policy)) }
        var descriptor = f.descriptor; descriptor["publisher"] = "e\u{301}"
        rejects(.descriptor) { try f.verify(payload: Self.json(descriptor)) }
        descriptor = f.descriptor; descriptor["extra"] = 1
        rejects(.descriptor) { try f.verify(payload: Self.json(descriptor)) }
    }

    func testMalformedAndBoundedInputs() throws {
        let f = Fixture()
        rejects(.limit) { try f.verify(payload: Data(repeating: 32, count: 65537)) }
        rejects(.limit) { try f.verify(policy: Data(repeating: 32, count: 131073)) }
        rejects(.limit) { try f.verify(signature: Data(repeating: 0, count: 65)) }
        rejects(.limit) { try f.verify(payload: Data((String(repeating: "[", count: 34) + "0" + String(repeating: "]", count: 34)).utf8)) }
        for raw in [Data([0xff]), Data("{\"x\":01}".utf8), Data("{}x".utf8), Data("{\"x\":\"\\uD800\"}".utf8)] {
            XCTAssertThrowsError(try f.verify(payload: raw))
        }
    }

    @MainActor
    func testSelectedPublicFilesRejectLinksDirectoriesFIFOsAndLimit() throws {
        let root = sessionTestDirectory().appendingPathComponent("signature-files-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let regular = root.appendingPathComponent("public")
        try Data([1, 2, 3]).write(to: regular)
        XCTAssertEqual(try PluginSignatureFile.read(regular, limit: 3), Data([1, 2, 3]))
        rejects(.limit) { try PluginSignatureFile.read(regular, limit: 2) }
        rejects(.file) { try PluginSignatureFile.read(root, limit: 64) }
        let link = root.appendingPathComponent("link"), fifo = root.appendingPathComponent("fifo")
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: regular)
        XCTAssertEqual(mkfifo(fifo.path, 0o600), 0)
        rejects(.file) { try PluginSignatureFile.read(link, limit: 64) }
        rejects(.file) { try PluginSignatureFile.read(fifo, limit: 64) }
    }

    func testPageDraftInvalidatesProofAndLateResults() throws {
        let proof = try Fixture().verify()
        var draft = PluginSignatureDraft()
        draft.descriptorPath = "/release.json"; draft.signaturePath = "/release.sig"; draft.policyPath = "/policy.json"
        XCTAssertNil(draft.begin())
        draft.trustedPolicy = true
        let first = try XCTUnwrap(draft.begin())
        XCTAssertNil(draft.begin())
        draft.descriptorPath = "/new.json"
        draft.finish(first, result: .success(proof))
        XCTAssertNil(draft.receipt)
        let second = try XCTUnwrap(draft.begin())
        draft.finish(second, result: .success(proof))
        XCTAssertEqual(draft.receipt, proof)
        draft.policyPath = "/new-policy.json"
        XCTAssertFalse(draft.trustedPolicy)
        XCTAssertNil(draft.receipt)
        draft.trustedPolicy = true
        let third = try XCTUnwrap(draft.begin())
        draft.finish(third, result: .failure(Failure.signature))
        XCTAssertNotNil(draft.issue); XCTAssertNil(draft.receipt)
        let fourth = try XCTUnwrap(draft.begin())
        draft.trustedPolicy = false
        draft.finish(fourth, result: .success(proof))
        XCTAssertNil(draft.receipt)
    }

    @MainActor
    func testCLISignatureAgainstActualCoreSnapshotWithoutApproval() async throws {
        guard ExtensionRuntime.supportStatus == 0,
              let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] else {
            throw XCTSkip("requires explicit JS-enabled Core and CLI public release fixture")
        }
        let root = URL(fileURLWithPath: path), package = root.appendingPathComponent("p")
        let controller = try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0",
            protocols: [1], runtimes: ["portable-js"], capabilities: ["session.rpc", "resource.receive"]), resourceDirectory: root)
        let review = try await controller.review(directory: package)
        let proof = try await controller.verifyDirectorySignature(packageID: review.id, digest: review.digest,
            descriptor: root.appendingPathComponent("r/release.json"), signature: root.appendingPathComponent("r/release.sig"),
            trustedPolicy: root.appendingPathComponent("policy.json"))
        XCTAssertEqual(proof.packageSHA256, review.digest)
        XCTAssertFalse(review.publisherAuthenticated)
        XCTAssertFalse(review.authorizationGranted)
        let result = try await controller.resolve(command: "preview", selectedID: review.id, selectedDigest: review.digest)
        XCTAssertNotEqual(result.status, 0, "signature must not approve a source")
        do {
            _ = try await controller.verifyDirectorySignature(packageID: review.id, digest: String(repeating: "0", count: 64),
                descriptor: root.appendingPathComponent("r/release.json"), signature: root.appendingPathComponent("r/release.sig"),
                trustedPolicy: root.appendingPathComponent("policy.json"))
            XCTFail("unreviewed digest accepted")
        } catch { XCTAssertEqual(error as? Failure, .identity) }
        let closed = await controller.close()
        XCTAssertTrue(closed)
        do {
            _ = try await controller.verifyDirectorySignature(packageID: review.id, digest: review.digest,
                descriptor: root.appendingPathComponent("r/release.json"), signature: root.appendingPathComponent("r/release.sig"),
                trustedPolicy: root.appendingPathComponent("policy.json"))
            XCTFail("closed owner accepted")
        } catch {
            guard case ExtensionController.ControllerError.closed = error else { XCTFail("unexpected error: \(error)"); return }
        }
    }

    private func rejects<T>(_ expected: Failure, file: StaticString = #filePath, line: UInt = #line,
                            _ operation: () throws -> T) {
        XCTAssertThrowsError(try operation(), file: file, line: line) { error in
            XCTAssertEqual(error as? Failure, expected, file: file, line: line)
        }
    }
}

private func hash(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
private func json(_ value: Any) throws -> Data { try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]) }
