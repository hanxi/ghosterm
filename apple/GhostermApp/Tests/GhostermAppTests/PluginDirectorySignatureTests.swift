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
        let rootPolicy = try Self.json(root.policy)
        let rootStatement = try Self.json(["schemaVersion": 1, "channel": "test", "generation": "3",
                                           "issuedAt": "100", "expiresAt": "200", "policySHA256": Self.hash(rootPolicy)])
        rejects(.channelRootIsPublisher) {
            try Verifier.verifyChannelPolicy(statement: rootStatement,
                signature: root.sign(rootStatement, domain: "ghosterm.channel-policy.v1\0"), policy: rootPolicy,
                channel: "test", channelRoot: root.key.publicKey.rawRepresentation, now: 150)
        }
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: directory) }
        func verifyRecorded(_ policy: Verifier.VerifiedChannelPolicy, now: UInt64) throws -> PluginDirectorySignature {
            try Verifier.verify(descriptor: descriptor, signature: releaseSignature, channelPolicy: policy,
                                identity: f.identity, now: now, stateDirectory: directory)
        }
        let refreshed = try Verifier.refreshChannelPolicy(statement: statement, signature: signature, policy: policy,
            channel: "test", channelRoot: root.key.publicKey.rawRepresentation, now: 150, stateDirectory: directory)
        XCTAssertEqual(refreshed.statementSHA256, proof.statementSHA256)
        func load(_ now: UInt64, key: Data = root.key.publicKey.rawRepresentation) throws -> Verifier.VerifiedChannelPolicy {
            try Verifier.loadChannelPolicy(channel: "test", channelRoot: key, now: now, stateDirectory: directory)
        }
        func observe(_ now: UInt64) throws -> Verifier.VerifiedChannelPolicy {
            try Verifier.loadChannelPolicy(channel: "test", channelRoot: root.key.publicKey.rawRepresentation,
                                           now: now, stateDirectory: directory, recordObservation: false)
        }
        XCTAssertEqual((try? observe(155))?.statementSHA256, proof.statementSHA256)
        XCTAssertEqual(try load(151).statementSHA256, proof.statementSHA256)
        rejects(.channelHistory) { try observe(150) }
        rejects(.channelFreshness) { try load(200) }
        rejects(.channelHistory) { try load(150) }
        rejects(.channelStatement) { try load(152, key: f.key.publicKey.rawRepresentation) }
        _ = try verifyRecorded(proof, now: 151)
        rejects(.channelHistory) { try verifyRecorded(proof, now: 150) }
        let older = try Self.json(["schemaVersion": 1, "channel": "test", "generation": "1",
                                   "issuedAt": "100", "expiresAt": "200", "policySHA256": Self.hash(policy)])
        let oldProof = try Verifier.verifyChannelPolicy(statement: older,
            signature: root.sign(older, domain: "ghosterm.channel-policy.v1\0"), policy: policy,
            channel: "test", channelRoot: root.key.publicKey.rawRepresentation, now: 152)
        rejects(.channelHistory) { try verifyRecorded(oldProof, now: 152) }
        XCTAssertEqual(try load(152).generation, 2)
    }

    func testConcurrentSameGenerationChannelWritersPreserveOneWinner() async throws {
        let publisher = Fixture(), root = Fixture()
        let publicRoot = root.key.publicKey.rawRepresentation
        var revoked = publisher.policy
        revoked["revokedKeyIds"] = [publisher.keyID]
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: directory) }
        let candidates = try [publisher.policy, revoked].map { input in
            let policy = try Self.json(input)
            let statement = try Self.json(["schemaVersion": 1, "channel": "test", "generation": "1",
                "issuedAt": "100", "expiresAt": "200", "policySHA256": Self.hash(policy)])
            return (policy: policy, statement: statement,
                    signature: try root.sign(statement, domain: "ghosterm.channel-policy.v1\0"))
        }
        let winners = try await withThrowingTaskGroup(of: String?.self) { tasks in
            for (policy, statement, signature) in candidates {
                tasks.addTask {
                    do {
                        return try Verifier.refreshChannelPolicy(statement: statement, signature: signature,
                            policy: policy, channel: "test", channelRoot: publicRoot, now: 150,
                            stateDirectory: directory).statementSHA256
                    } catch Failure.channelHistory {
                        return nil
                    } catch PluginReleaseCatalog.Failure.busy {
                        return nil
                    }
                }
            }
            var committed: [String] = []
            for try await winner in tasks {
                if let winner { committed.append(winner) }
            }
            return committed
        }
        XCTAssertEqual(winners.count, 1)
        let winner = try XCTUnwrap(winners.first)
        let losing = try XCTUnwrap(candidates.first { Self.hash($0.statement) != winner })
        XCTAssertThrowsError(try Verifier.refreshChannelPolicy(statement: losing.statement, signature: losing.signature,
            policy: losing.policy, channel: "test", channelRoot: publicRoot, now: 150, stateDirectory: directory)) {
            XCTAssertEqual($0 as? Failure, .channelHistory)
        }
        let reopened = try Verifier.loadChannelPolicy(channel: "test", channelRoot: publicRoot, now: 150,
                                                       stateDirectory: directory)
        XCTAssertEqual(reopened.statementSHA256, winner)
    }

    func testChannelPublicationKeepsOldRecordAndReconcilesUncertainSync() throws {
        let publisher = Fixture(), root = Fixture()
        let policy = try Self.json(publisher.policy)
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: directory) }
        func verified(_ generation: String) throws -> Verifier.VerifiedChannelPolicy {
            let statement = try Self.json(["schemaVersion": 1, "channel": "test", "generation": generation,
                "issuedAt": "100", "expiresAt": "200", "policySHA256": Self.hash(policy)])
            return try Verifier.verifyChannelPolicy(statement: statement,
                signature: root.sign(statement, domain: "ghosterm.channel-policy.v1\0"), policy: policy,
                channel: "test", channelRoot: root.key.publicKey.rawRepresentation, now: 150)
        }
        let previous = try verified("1"), candidate = try verified("3")
        do {
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: directory, exclusive: true))
            try owner.recordChannelPolicy(previous, now: 150)
            let before = try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
            let record = try XCTUnwrap(before.first { $0.lastPathComponent.hasPrefix("channel-policy-v1-") })
            let oldBytes = try Data(contentsOf: record)
            do {
                try owner.recordChannelPolicy(candidate, now: 151, requireHistory: true,
                                              beforeRename: { throw POSIXError(.ENOSPC) })
                return XCTFail("A channel write must stop on its prepublication error")
            } catch {
                XCTAssertEqual((error as? POSIXError)?.code, .ENOSPC)
            }
            XCTAssertEqual(try Data(contentsOf: record), oldBytes)
            XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: directory.path)
                .contains { $0.hasPrefix(".selection-") })
            let externallyUpdated = oldBytes + Data([0x0a])
            do {
                try owner.recordChannelPolicy(candidate, now: 151, requireHistory: true,
                                              beforeRename: { try externallyUpdated.write(to: record) })
                return XCTFail("Channel publication must preserve a changed record")
            } catch {
                XCTAssertEqual(error as? PluginReleaseCatalog.Failure, .conflict)
            }
            XCTAssertEqual(try Data(contentsOf: record), externallyUpdated)
        }
        do {
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: directory, exclusive: true, parentSync: { _ in -1 }))
            XCTAssertThrowsError(try owner.recordChannelPolicy(candidate, now: 151, requireHistory: true),
                                 "A failed channel directory sync cannot report durable publication") {
                XCTAssertEqual($0 as? PluginReleaseCatalog.Failure, .io)
            }
        }
        let recovered = try Verifier.loadChannelPolicy(channel: "test", channelRoot: root.key.publicKey.rawRepresentation,
                                                       now: 151, stateDirectory: directory)
        XCTAssertEqual(recovered.statementSHA256, candidate.statementSHA256)
        let entries = try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
        let record = try XCTUnwrap(entries.first { $0.lastPathComponent.hasPrefix("channel-policy-v1-") })
        var inconsistent = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: record)) as? [String: Any])
        inconsistent["generation"] = 1
        let inconsistentBytes = try Self.json(inconsistent)
        try inconsistentBytes.write(to: record)
        let replay = try verified("2")
        let owner = try XCTUnwrap(PluginReleaseCatalog(directory: directory, exclusive: true))
        rejects(.channelHistory) { try owner.recordChannelPolicy(replay, now: 152, requireHistory: true) }
        XCTAssertEqual(try Data(contentsOf: record), inconsistentBytes, "Failed refresh must preserve the existing cache")
    }

    func testChannelRefreshCommitsOnlyVerifiedCompleteResponses() async throws {
        let fixture = Fixture(), root = Fixture(), policy = try Self.json(fixture.policy)
        let state = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: state, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: state) }
        let directory = try XCTUnwrap(URL(string: "https://channel.invalid/test/"))
        let now = UInt64(Date().timeIntervalSince1970)
        func responses(_ generation: String, body: Data) throws -> [String: Data] {
            let statement = try Self.json(["schemaVersion": 1, "channel": "test", "generation": generation,
                                           "issuedAt": String(now), "expiresAt": String(now + 600), "policySHA256": Self.hash(policy)])
            return ["channel-policy.json": statement, "channel-policy.sig": try root.sign(statement, domain: "ghosterm.channel-policy.v1\0"),
                    "policy.json": body]
        }
        let trust = ExtensionController.ChannelTrust(channel: "test", root: root.key.publicKey.rawRepresentation,
                                                     stateDirectory: state, refreshDirectory: directory)
        let bundledFields = ["Name": "test", "Root": trust.root.base64EncodedString(), "Directory": directory.absoluteString]
        for endpoint in ["https://name@channel.invalid/test/", "https://channel.invalid:0/test/",
                         "https://channel.invalid:65536/test/"] {
            let invalidBundle = bundledFields.merging(["Directory": endpoint]) { _, replacement in replacement }
            let rejectedBundle = await MainActor.run { AppModel.bundledPluginChannel(invalidBundle, supportDirectory: state) }
            XCTAssertNil(rejectedBundle, endpoint)
        }
        let bundled = await MainActor.run { AppModel.bundledPluginChannel(bundledFields, supportDirectory: state) }
        let channel = try XCTUnwrap(bundled)
        XCTAssertEqual(channel.root, trust.root)
        XCTAssertEqual(channel.refreshDirectory, directory)
        var mode = stat()
        XCTAssertEqual(lstat(channel.stateDirectory.path, &mode), 0)
        XCTAssertEqual(mode.st_mode & 0o777, 0o700)
        XCTAssertEqual(try channel.stateDirectory.resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup, true)
        let reopened = await MainActor.run { AppModel.bundledPluginChannel(bundledFields, supportDirectory: state) }
        XCTAssertEqual(reopened?.stateDirectory, channel.stateDirectory)
        let profile = try ExtensionController.HostProfile(side: .client, sdkVersion: "1.0.0", protocols: [1],
                                                           runtimes: ["portable-js"], capabilities: [])
        let unconfigured = try ExtensionController(profile: profile)
        XCTAssertFalse(unconfigured.canRefreshChannelPolicy)
        do {
            _ = try await unconfigured.refreshConfiguredChannelPolicy()
            XCTFail("missing host configuration must prevent channel refresh")
        } catch PluginChannelRefresh.Failure.endpoint {}
        let configured = try ExtensionController(profile: profile, channelTrust: trust)
        XCTAssertTrue(configured.canRefreshChannelPolicy)
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [ChannelRefreshProtocol.self]
        let session = URLSession(configuration: configuration)
        defer { session.invalidateAndCancel(); ChannelRefreshProtocol.set([:]) }
        ChannelRefreshProtocol.set(try responses("1", body: policy))
        for port in [0, 65536] {
            let invalidDirectory = try XCTUnwrap(URL(string: "https://channel.invalid:\(port)/test/"))
            let invalidTrust = ExtensionController.ChannelTrust(channel: trust.channel, root: trust.root,
                stateDirectory: state, refreshDirectory: invalidDirectory)
            do {
                _ = try await PluginChannelRefresh.refresh(invalidTrust, session: session)
                XCTFail("out-of-range port must prevent channel refresh: \(port)")
            } catch PluginChannelRefresh.Failure.endpoint {}
        }
        let first = try await PluginChannelRefresh.refresh(trust, session: session)
        XCTAssertEqual(first.generation, 1)
        let cachedByOwner = try await configured.cachedChannelPolicy()
        XCTAssertEqual(cachedByOwner.statementSHA256, first.statementSHA256)
        let affected = try await configured.runningInstancesWithRevokedPublisher(in: first)
        XCTAssertEqual(affected, 0)
        ChannelRefreshProtocol.set(try responses("2", body: policy + Data([10])))
        do {
            _ = try await PluginChannelRefresh.refresh(trust, session: session)
            XCTFail("mixed statement and policy responses must be rejected")
        } catch Failure.channelStatement {}
        ChannelRefreshProtocol.set(try responses("2", body: Data(repeating: 0, count: 131_073)))
        do {
            _ = try await PluginChannelRefresh.refresh(trust, session: session)
            XCTFail("oversized streamed response must be rejected")
        } catch PluginChannelRefresh.Failure.length {}
        let cached = try Verifier.loadChannelPolicy(channel: trust.channel, channelRoot: trust.root,
                                                     now: UInt64(Date().timeIntervalSince1970), stateDirectory: state)
        XCTAssertEqual(cached.generation, 1)
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
        let cancelledReview = Task {
            withUnsafeCurrentTask { $0?.cancel() }
            return try await controller.review(directory: package)
        }
        do {
            _ = try await cancelledReview.value
            XCTFail("A cancelled review must not enter the package worker")
        } catch is CancellationError {}

        let review = try await controller.review(directory: package)
        let repeated: ExtensionController.PackageReview
        do {
            repeated = try await controller.review(directory: package)
        } catch {
            return XCTFail("The same undecided review must be reusable: \(error)")
        }
        XCTAssertEqual(repeated.digest, review.digest)
        XCTAssertFalse(repeated.sourceApproved, "Reopening a pending review cannot approve its source")
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
        _ = try await controller.deny(id: review.id, digest: review.digest)
        do {
            _ = try await controller.review(directory: package)
            XCTFail("Repeating a review must not reopen a denied source")
        } catch {
            guard case ExtensionController.ControllerError.registry(let status) = error else { throw error }
            XCTAssertEqual(status, ExtensionRegistry.Status.conflict.rawValue)
        }
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

private final class ChannelRefreshProtocol: URLProtocol {
    private static let lock = NSLock()
    private static var bodies: [String: Data] = [:]

    static func set(_ values: [String: Data]) {
        lock.lock()
        defer { lock.unlock() }
        bodies = values
    }

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        guard let url = request.url, let response = HTTPURLResponse(url: url, statusCode: 200, httpVersion: nil, headerFields: nil) else {
            client?.urlProtocol(self, didFailWithError: URLError(.badServerResponse))
            return
        }
        Self.lock.lock()
        let data = Self.bodies[url.lastPathComponent]
        Self.lock.unlock()
        guard url.host == "channel.invalid", let data else {
            client?.urlProtocol(self, didFailWithError: URLError(.badURL))
            return
        }
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: data)
        client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}
