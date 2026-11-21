import CryptoKit
import Darwin
import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel
@testable import GhostermApp

final class PluginReleaseCapacityTests: XCTestCase {
    func testFullVolumePreservesInstalledVersionAndDiscardsPartialStage() throws {
        guard let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_INSTALL_CAPACITY_VOLUME"] else {
            throw XCTSkip("requires an owned small test volume")
        }
        let volume = URL(fileURLWithPath: path)
        let root = volume.appendingPathComponent(UUID().uuidString)
        defer {
            try? PluginReleaseInstallTests.thaw(root)
            try? FileManager.default.removeItem(at: root)
        }
        let installed = try PluginReleaseStage(directory: root)
        let prior = Data("installed version".utf8)
        try installed.write(prior, path: "payload/prior")
        let release = try installed.publish(id: String(repeating: "a", count: 64))
        XCTAssertTrue(release.durabilityConfirmed)

        let candidate = try PluginReleaseStage(directory: root)
        defer { try? candidate.discard() }
        try candidate.write(Data([1, 2, 3]), path: "payload/complete")
        let attributes = try FileManager.default.attributesOfFileSystem(forPath: root.path)
        let capacity = try XCTUnwrap(attributes[.systemSize] as? NSNumber).intValue
        XCTAssertTrue((1...64 * 1024 * 1024).contains(capacity))
        guard (1...64 * 1024 * 1024).contains(capacity) else { return }
        let pathInStage = "payload/partial"
        XCTAssertThrowsError(try candidate.write(Data(repeating: 0xa5, count: capacity), path: pathInStage)) {
            XCTAssertEqual($0 as? PluginReleaseStage.Failure, .io)
        }
        let partial = candidate.payloadURL.appendingPathComponent("partial")
        let size = try XCTUnwrap(FileManager.default.attributesOfItem(atPath: partial.path)[.size] as? NSNumber)
        XCTAssertLessThan(size.intValue, capacity)
        XCTAssertEqual(try Data(contentsOf: candidate.payloadURL.appendingPathComponent("complete")),
                       Data([1, 2, 3]))
        try candidate.discard()
        XCTAssertFalse(FileManager.default.fileExists(atPath: partial.path))
        XCTAssertEqual(try Data(contentsOf: release.directory.appendingPathComponent("payload/prior")), prior)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: root.path)
            .filter { $0 != PluginInstallJournal.directoryName }, [release.directory.lastPathComponent])
    }
}

@MainActor
final class PluginReleaseInstallTests: XCTestCase {
    private func fixture() throws -> URL {
        guard ExtensionRuntime.supportStatus == 0,
              let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] else {
            throw XCTSkip("requires explicit JS-enabled Core and signed release fixtures")
        }
        return URL(fileURLWithPath: path)
    }
    private func directory() throws -> URL {
        let root = sessionTestDirectory().appendingPathComponent("install-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try Self.thaw(root) }
        return root
    }
    nonisolated static func thaw(_ root: URL) throws {
        var value = stat()
        guard lstat(root.path, &value) == 0, value.st_mode & S_IFMT == S_IFDIR else { return }
        guard chmod(root.path, 0o700) == 0 else { throw PluginReleaseStage.Failure.io }
        for child in try FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil) { try thaw(child) }
    }
    private func controller(_ root: URL, channelTrust: ExtensionController.ChannelTrust? = nil) throws -> ExtensionController {
        try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1],
            runtimes: ["portable-js"], capabilities: ["resource.receive"]), resourceDirectory: root,
            installationDirectory: root.appendingPathComponent("installed"), channelTrust: channelTrust)
    }
    private func prepare(_ controller: ExtensionController, _ root: URL) async throws -> ExtensionController.PreparedRelease {
        try await controller.prepareSignedRelease(artifact: root.appendingPathComponent("r/plugin.zip"),
            descriptor: root.appendingPathComponent("r/release.json"), signature: root.appendingPathComponent("r/release.sig"),
            trustedPolicy: root.appendingPathComponent("policy.json"))
    }
    private func names(_ root: URL) throws -> [String] {
        guard FileManager.default.fileExists(atPath: root.path) else { return [] }
        let names = try FileManager.default.contentsOfDirectory(atPath: root.path).sorted()
        if names.contains(PluginInstallJournal.directoryName) {
            var value = stat()
            XCTAssertEqual(lstat(root.appendingPathComponent(PluginInstallJournal.directoryName).path, &value), 0)
            XCTAssertEqual(value.st_mode & S_IFMT, S_IFDIR); XCTAssertEqual(value.st_mode & 0o077, 0)
        }
        return names.filter { $0 != PluginInstallJournal.directoryName }
    }
    private func hash(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }

    func testChannelGenerationAdvancePreventsPreparedReleasePublication() async throws {
        let input = try fixture(), root = try directory()
        let stateDirectory = root.appendingPathComponent("channel")
        try FileManager.default.createDirectory(at: stateDirectory, withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])
        let policy = try Data(contentsOf: input.appendingPathComponent("policy.json"))
        let fields = try XCTUnwrap(JSONSerialization.jsonObject(with: policy) as? [String: Any])
        let channel = try XCTUnwrap(fields["channel"] as? String)
        let key = Curve25519.Signing.PrivateKey()
        let owner = try controller(root, channelTrust: .init(channel: channel, root: key.publicKey.rawRepresentation,
                                                              stateDirectory: stateDirectory))
        let now = UInt64(Date().timeIntervalSince1970)
        func proof(_ generation: String, policy: Data, expiresAt: UInt64? = nil) throws -> PluginDirectorySignatureVerifier.VerifiedChannelPolicy {
            let statement = try JSONSerialization.data(withJSONObject: [
                "schemaVersion": 1, "channel": channel, "generation": generation,
                "issuedAt": String(now), "expiresAt": String(expiresAt ?? (now + 600)), "policySHA256": hash(policy),
            ])
            var message = Data("ghosterm.channel-policy.v1\0".utf8)
            message.append(statement)
            return try PluginDirectorySignatureVerifier.verifyChannelPolicy(statement: statement,
                signature: key.signature(for: message), policy: policy, channel: channel,
                channelRoot: key.publicKey.rawRepresentation, now: now)
        }
        func prepare(_ generation: String) async throws -> ExtensionController.PreparedRelease {
            try await owner.prepareSignedRelease(artifact: input.appendingPathComponent("r/plugin.zip"),
                descriptor: input.appendingPathComponent("r/release.json"), signature: input.appendingPathComponent("r/release.sig"),
                channelPolicy: proof(generation, policy: policy), channelStateDirectory: stateDirectory)
        }

        let stale = try await prepare("1")
        do {
            let state = try XCTUnwrap(PluginReleaseCatalog(directory: stateDirectory, exclusive: true))
            try state.recordChannelPolicy(proof("2", policy: policy), now: UInt64(Date().timeIntervalSince1970))
        }
        do {
            _ = try await owner.installPreparedRelease(id: stale.id)
            XCTFail("publication must reject a superseded channel policy")
            return
        } catch PluginDirectorySignatureVerifier.Failure.channelHistory {}
        let store = root.appendingPathComponent("installed")
        XCTAssertFalse(try names(store).contains(stale.signature.descriptorSHA256))
        try await owner.discardPreparedRelease(id: stale.id)
        let draft = PluginReleaseInstallDraft()
        draft.useConfiguredChannel = true
        draft.artifactPath = input.appendingPathComponent("r/plugin.zip").path
        draft.descriptorPath = input.appendingPathComponent("r/release.json").path
        draft.signaturePath = input.appendingPathComponent("r/release.sig").path
        XCTAssertTrue(draft.canInspect)
        await draft.inspect(using: owner)
        let current = try XCTUnwrap(draft.prepared, draft.issue ?? "configured channel preparation failed")
        XCTAssertEqual(current.channelGeneration, 2)
        let result = await draft.install(using: owner)
        let installed = try XCTUnwrap(result, draft.issue ?? "configured channel installation failed")
        XCTAssertEqual(installed.signature, current.signature)
        let offline = input.appendingPathComponent("policy.json")
        let releaseID = installed.signature.descriptorSHA256
        let unconfigured = try controller(root)
        do {
            _ = try await unconfigured.selectInstalledVersion(releaseID: releaseID, expectedRevision: nil, trustedPolicy: offline)
            XCTFail("a channel release must require host trust configuration")
        } catch PluginDirectorySignatureVerifier.Failure.channelHistory {}
        let review = try await owner.previewInstalledSelection(releaseID: releaseID, expectedRevision: nil)
        XCTAssertEqual(review.channelGeneration, 2)
        let selected = try await owner.confirmInstalledSelection(id: review.id, acknowledged: true)
        let staleReview = try await owner.previewInstalledSelection(releaseID: releaseID, expectedRevision: selected.catalog.revision)
        do {
            let state = try XCTUnwrap(PluginReleaseCatalog(directory: stateDirectory, exclusive: true))
            try state.recordChannelPolicy(proof("3", policy: policy), now: UInt64(Date().timeIntervalSince1970))
        }
        do {
            _ = try await owner.confirmInstalledSelection(id: staleReview.id, acknowledged: true)
            XCTFail("a newer statement must require a fresh review even with identical policy bytes")
            return
        } catch PluginReleaseCatalog.Failure.changed {}
        var revoked = fields
        revoked["revokedKeyIds"] = [installed.signature.keyID]
        let revokedPolicy = try JSONSerialization.data(withJSONObject: revoked)
        do {
            let state = try XCTUnwrap(PluginReleaseCatalog(directory: stateDirectory, exclusive: true))
            try state.recordChannelPolicy(proof("4", policy: revokedPolicy), now: UInt64(Date().timeIntervalSince1970))
        }
        do {
            _ = try await owner.selectInstalledVersion(releaseID: releaseID, expectedRevision: selected.catalog.revision, trustedPolicy: offline)
            XCTFail("an offline policy must not bypass a newer revocation")
        } catch PluginDirectorySignatureVerifier.Failure.revokedKey {}
        let unchanged = try await owner.installedVersions()
        XCTAssertEqual(unchanged.revision, selected.catalog.revision)

        let expiry = UInt64(Date().timeIntervalSince1970) + 5
        let expiring = try await owner.prepareSignedRelease(artifact: input.appendingPathComponent("v2/r/plugin.zip"),
            descriptor: input.appendingPathComponent("v2/r/release.json"), signature: input.appendingPathComponent("v2/r/release.sig"),
            channelPolicy: proof("5", policy: policy, expiresAt: expiry), channelStateDirectory: stateDirectory)
        let remaining = max(0, Double(expiry) - Date().timeIntervalSince1970)
        try await Task.sleep(for: .seconds(remaining))
        do {
            _ = try await owner.installPreparedRelease(id: expiring.id)
            XCTFail("a prepared release must not publish after its channel policy expires")
        } catch PluginDirectorySignatureVerifier.Failure.channelFreshness {}
        XCTAssertFalse(try names(store).contains(expiring.signature.descriptorSHA256))
        try await owner.discardPreparedRelease(id: expiring.id)
    }

    func testPreparedReleaseIsPrivateAndCancelCleansIt() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let prepared = try await prepare(controller, input)
        let store = root.appendingPathComponent("installed")
        XCTAssertTrue(prepared.permissions.contains { $0.side == "client" && $0.capability == "resource.receive" && !$0.purpose.isEmpty })
        XCTAssertNotNil(prepared.executorDeclarations)
        XCTAssertEqual(try names(store).filter { $0.hasPrefix(".prepare-") }.count, 1)
        XCTAssertFalse(try names(store).contains(prepared.signature.descriptorSHA256))
        let resolution = try await controller.resolve(command: "preview", selectedID: prepared.review.id, selectedDigest: prepared.review.digest)
        XCTAssertNotEqual(resolution.status, 0, "preparation must not expose the package to resolution")
        do { try await controller.discardPreparedRelease(id: prepared.id) }
        catch { XCTFail("cancel must finish owned namespace and journal cleanup: \(error)"); return }
        XCTAssertEqual(try names(store), [], "cancel must remove the owned stage")
        let records = try await controller.installationResidues(); XCTAssertTrue(records.entries.isEmpty); XCTAssertTrue(records.issues.isEmpty)
        do { _ = try await controller.installPreparedRelease(id: prepared.id); XCTFail("cancelled preparation installed") } catch {}
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testInstallPublishesReadOnlyVersionWithoutSourceApproval() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let prepared = try await prepare(controller, input)
        let installed: ExtensionController.InstalledRelease
        do { installed = try await controller.installPreparedRelease(id: prepared.id) }
        catch { return XCTFail("A valid read-only version must publish: \(error)") }
        XCTAssertTrue(installed.durabilityConfirmed)
        XCTAssertEqual(installed.signature, prepared.signature)
        XCTAssertEqual(try names(root.appendingPathComponent("installed")), [prepared.signature.descriptorSHA256])
        XCTAssertEqual(hash(try Data(contentsOf: installed.directory.appendingPathComponent("integrity.json"))), prepared.review.digest)
        var value = stat()
        XCTAssertEqual(lstat(installed.directory.path, &value), 0)
        XCTAssertEqual(value.st_mode & 0o777, 0o500, "published package directory must be read-only")
        XCTAssertEqual(lstat(installed.directory.appendingPathComponent("plugin.json").path, &value), 0)
        XCTAssertEqual(value.st_mode & 0o222, 0, "published members must be read-only")
        do {
            let resolution = try await controller.resolve(command: "preview", selectedID: prepared.review.id, selectedDigest: prepared.review.digest)
            XCTAssertNotEqual(resolution.status, 0, "installation must not approve source")
        } catch { XCTFail("published read-only version must remain inspectable: \(error)") }
        XCTAssertFalse(installed.review.authorizationGranted)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testDuplicatePublicationCannotOverwriteExistingVersion() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let first = try await prepare(controller, input)
        let installed = try await controller.installPreparedRelease(id: first.id)
        let bytes = try Data(contentsOf: installed.directory.appendingPathComponent("integrity.json"))
        let second = try await prepare(controller, input)
        do { _ = try await controller.installPreparedRelease(id: second.id); XCTFail("existing release was overwritten") }
        catch { XCTAssertEqual(error as? PluginReleaseStage.Failure, .conflict) }
        try await controller.discardPreparedRelease(id: second.id)
        XCTAssertEqual(try Data(contentsOf: installed.directory.appendingPathComponent("integrity.json")), bytes)
        XCTAssertEqual(try names(root.appendingPathComponent("installed")), [first.signature.descriptorSHA256])
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testSecondVersionKeepsOldCheckedSnapshotAndDirectory() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let first = try await prepare(controller, input)
        let old = try await controller.installPreparedRelease(id: first.id)
        _ = try await controller.selectInstalledVersion(releaseID: first.signature.descriptorSHA256, expectedRevision: nil, trustedPolicy: input.appendingPathComponent("policy.json"))
        _ = try await controller.approve(id: first.review.id, digest: first.review.digest)
        let second = try await prepare(controller, input.appendingPathComponent("v2"))
        let next = try await controller.installPreparedRelease(id: second.id)
        XCTAssertNotEqual(old.review.digest, next.review.digest)
        XCTAssertEqual(try names(root.appendingPathComponent("installed")).filter(PluginReleaseCatalog.digest).count, 2)
        XCTAssertEqual(hash(try Data(contentsOf: old.directory.appendingPathComponent("integrity.json"))), old.review.digest)
        let selected = try await controller.resolve(command: "preview", selectedID: old.review.id, selectedDigest: old.review.digest)
        XCTAssertEqual(selected.status, 0, "new publication must retain the exact old checked package")
        let unapproved = try await controller.resolve(command: "preview", selectedID: next.review.id, selectedDigest: next.review.digest)
        XCTAssertNotEqual(unapproved.status, 0, "new digest needs its own source approval")
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testDeflatedReleaseUsesTheSameCheckedIdentity() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let stored = try await prepare(controller, input)
        try await controller.discardPreparedRelease(id: stored.id)
        let compressed = try await prepare(controller, input.appendingPathComponent("compressed"))
        XCTAssertEqual(stored.review.digest, compressed.review.digest)
        let installed = try await controller.installPreparedRelease(id: compressed.id)
        XCTAssertEqual(hash(try Data(contentsOf: installed.directory.appendingPathComponent("integrity.json"))), stored.review.digest)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testBadSignedArchivesNeverPublishOrLeaveOwnedStage() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        for name in ["signature", "bytehash", "crc", "hidden", "path", "case", "file-parent", "link", "size", "extra", "compressed-tail", "identity"] {
            do { _ = try await prepare(controller, input.appendingPathComponent("bad/" + name)); XCTFail("bad signed release accepted: \(name)") }
            catch {
                XCTAssertEqual(try names(root.appendingPathComponent("installed")), [], "failed release must not publish or retain a stage: \(name)")
            }
        }
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testCloseDiscardsPendingAndFurtherPrepareFails() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        _ = try await prepare(controller, input)
        let closed = await controller.close(); XCTAssertTrue(closed)
        XCTAssertEqual(try names(root.appendingPathComponent("installed")), [])
        do { _ = try await prepare(controller, input); XCTFail("closed owner accepted release") } catch {}
    }

    func testFailedDiscardRetainsOwnerForExplicitRetry() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let value = try await prepare(controller, input)
        let store = root.appendingPathComponent("installed")
        let name = try XCTUnwrap(names(store).first)
        let original = store.appendingPathComponent(name), held = store.appendingPathComponent("held")
        try FileManager.default.moveItem(at: original, to: held)
        do { try await controller.discardPreparedRelease(id: value.id); XCTFail("changed stage location was removed") }
        catch { XCTAssertTrue(error.localizedDescription.contains("安装暂存清理失败")) }
        XCTAssertTrue(FileManager.default.fileExists(atPath: held.path))
        let unfinished = await controller.close(); XCTAssertFalse(unfinished)
        try FileManager.default.moveItem(at: held, to: original)
        do { try await controller.retryFailedInstallCleanup() }
        catch { XCTFail("same owned stage must remain retryable: \(error)") }
        XCTAssertEqual(try names(store), [], "retry must clean the original owned namespace")
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testFailedReplacementPreservesOldDirectoryAndResolution() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let first = try await prepare(controller, input)
        let installed = try await controller.installPreparedRelease(id: first.id)
        _ = try await controller.selectInstalledVersion(releaseID: first.signature.descriptorSHA256, expectedRevision: nil, trustedPolicy: input.appendingPathComponent("policy.json"))
        _ = try await controller.approve(id: first.review.id, digest: first.review.digest)
        for bad in ["crc", "identity"] {
            do { _ = try await prepare(controller, input.appendingPathComponent("bad/" + bad)); XCTFail("bad replacement accepted") } catch {}
            XCTAssertEqual(try names(root.appendingPathComponent("installed")), [first.signature.descriptorSHA256, PluginReleaseCatalog.selectionName].sorted())
            XCTAssertEqual(hash(try Data(contentsOf: installed.directory.appendingPathComponent("integrity.json"))), first.review.digest)
            let selected = try await controller.resolve(command: "preview", selectedID: first.review.id, selectedDigest: first.review.digest)
            XCTAssertEqual(selected.status, 0, "failed replacement must retain old source decision and checked bytes")
        }
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testChangedPreparedFilesMustNotPublish() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let value = try await prepare(controller, input)
        let store = root.appendingPathComponent("installed")
        let name = try XCTUnwrap(names(store).first)
        let file = store.appendingPathComponent(name + "/payload/client/index.js")
        XCTAssertEqual(chmod(file.path, 0o600), 0)
        try Data("changed after inspection".utf8).write(to: file)
        XCTAssertEqual(chmod(file.path, 0o400), 0)
        do { _ = try await controller.installPreparedRelease(id: value.id); XCTFail("changed prepared bytes were published") }
        catch { XCTAssertEqual(error as? PluginReleaseStage.Failure, .changed) }
        XCTAssertFalse(try names(store).contains(value.signature.descriptorSHA256))
        try await controller.discardPreparedRelease(id: value.id)
        XCTAssertEqual(try names(store), [])
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testAddedOrMissingPreparedMembersMustNotPublish() throws {
        for extra in [false, true] {
            let root = try directory().appendingPathComponent("store"), stage = try PluginReleaseStage(directory: root)
            try stage.write(Data([1]), path: "payload/a")
            let payload = stage.payloadURL
            if extra { try Data([2]).write(to: payload.appendingPathComponent("extra")); XCTAssertEqual(chmod(payload.appendingPathComponent("extra").path, 0o400), 0) }
            else { try FileManager.default.removeItem(at: payload.appendingPathComponent("a")) }
            XCTAssertThrowsError(try stage.publish(id: String(repeating: "d", count: 64))) { XCTAssertEqual($0 as? PluginReleaseStage.Failure, .changed) }
            try stage.discard()
        }
    }

    func testStageLockFreezeAndFailedPublishCleanup() throws {
        let root = try directory().appendingPathComponent("store"), id = String(repeating: "a", count: 64)
        var journalPrepared = false
        let stage = try PluginReleaseStage(directory: root, checkpoint: { point in
            if point == "before-publication-rename" { journalPrepared = true }
        })
        try stage.write(Data("snapshot".utf8), path: "payload/a")
        XCTAssertThrowsError(try PluginReleaseStage(directory: root)) { XCTAssertEqual($0 as? PluginReleaseStage.Failure, .busy) }
        enum Injected: Error { case beforeRename }
        XCTAssertThrowsError(try stage.publish(id: id, beforeRename: {
            XCTAssertTrue(journalPrepared, "final authorization must follow the publication journal")
            throw Injected.beforeRename
        })) { XCTAssertEqual($0 as? Injected, .beforeRename) }
        XCTAssertFalse(try names(root).contains(id), "failure before rename must not publish")
        try stage.discard()
        XCTAssertEqual(try names(root), [], "failed frozen stage must still clean")
        let next = try PluginReleaseStage(directory: root)
        try next.write(Data([1]), path: "payload/a")
        let member = next.payloadURL.appendingPathComponent("a")
        do {
            _ = try next.publish(id: id, beforeRename: {
                guard chmod(member.path, 0o600) == 0 else { throw PluginReleaseStage.Failure.io }
                try Data([2]).write(to: member)
                guard chmod(member.path, 0o400) == 0 else { throw PluginReleaseStage.Failure.io }
            })
            return XCTFail("A member changed during final authorization must not publish")
        } catch {
            XCTAssertEqual(error as? PluginReleaseStage.Failure, .changed)
        }
        try next.discard()
        XCTAssertEqual(try names(root), [])
    }

    func testPermissionRetryRestoresSourceModeForSuccessAndConflict() throws {
        let id = String(repeating: "a", count: 64)
        for conflict in [false, true] {
            let root = try directory().appendingPathComponent("store")
            var attempts: [mode_t] = []
            let stage = try PluginReleaseStage(directory: root, renameExclusive: { parent, source, target in
                var status = stat()
                guard fstatat(parent, source, &status, AT_SYMLINK_NOFOLLOW) == 0 else { return -1 }
                attempts.append(status.st_mode & 0o777)
                if attempts.count == 1 { errno = EACCES; return -1 }
                return renameatx_np(parent, source, parent, target, UInt32(RENAME_EXCL))
            })
            try stage.write(Data([42]), path: "payload/file")
            let target = root.appendingPathComponent(id)
            let source = root.appendingPathComponent(stage.name)
            if conflict {
                try FileManager.default.createDirectory(at: target, withIntermediateDirectories: false)
                let sentinel = target.appendingPathComponent("existing")
                try Data([7]).write(to: sentinel)
                XCTAssertThrowsError(try stage.publish(id: id)) { XCTAssertEqual($0 as? PluginReleaseStage.Failure, .conflict) }
                XCTAssertEqual(try Data(contentsOf: sentinel), Data([7]))
            } else {
                XCTAssertTrue(try stage.publish(id: id).durabilityConfirmed)
                XCTAssertEqual(try Data(contentsOf: target.appendingPathComponent("payload/file")), Data([42]))
            }
            XCTAssertEqual(attempts, [0o500, 0o700])
            let restored = try FileManager.default.attributesOfItem(atPath: (conflict ? source : target).path)
            XCTAssertEqual(restored[.posixPermissions] as? NSNumber, NSNumber(value: 0o500))
            if conflict { try stage.discard() }
        }
    }

    func testPublishedButUnsyncedParentIsExplicitlyUnknown() throws {
        let root = try directory().appendingPathComponent("store")
        let stage = try PluginReleaseStage(directory: root, parentSync: { _ in -1 })
        try stage.write(Data([1]), path: "payload/a")
        let result = try stage.publish(id: String(repeating: "b", count: 64))
        XCTAssertFalse(result.durabilityConfirmed, "post-rename fsync failure must not claim durable success")
        XCTAssertEqual(try Data(contentsOf: result.directory.appendingPathComponent("payload/a")), Data([1]))
        try stage.discard()
        XCTAssertTrue(FileManager.default.fileExists(atPath: result.directory.path), "discard must never delete an already-published release")

        let replacementRoot = try directory().appendingPathComponent("store")
        let id = String(repeating: "d", count: 64)
        let destination = replacementRoot.appendingPathComponent(id)
        let detached = replacementRoot.appendingPathComponent("detached")
        let replacedStage = try PluginReleaseStage(directory: replacementRoot, checkpoint: { phase in
            if phase == "after-publication-sync" {
                guard chmod(destination.path, 0o700) == 0 else { throw PluginReleaseStage.Failure.io }
                try FileManager.default.moveItem(at: destination, to: detached)
                try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: false)
            }
        })
        try replacedStage.write(Data([1]), path: "payload/a")
        let replacement = try replacedStage.publish(id: id)
        XCTAssertFalse(replacement.durabilityConfirmed, "A replaced version cannot report durable installation")
        guard !replacement.durabilityConfirmed else { return }
        let journal = replacementRoot.appendingPathComponent(PluginInstallJournal.directoryName)
        XCTAssertEqual(try names(journal).filter { $0.hasSuffix(".json") }.count, 1)
        XCTAssertEqual(try Data(contentsOf: detached.appendingPathComponent("payload/a")), Data([1]))
        XCTAssertEqual(try names(destination), [])
    }

    func testExistingEmptySlotIsNeverAdopted() throws {
        let root = try directory().appendingPathComponent("store"), id = String(repeating: "c", count: 64)
        let stage = try PluginReleaseStage(directory: root)
        try stage.write(Data([1]), path: "payload/a")
        let existing = root.appendingPathComponent(id)
        try FileManager.default.createDirectory(at: existing, withIntermediateDirectories: false)
        XCTAssertThrowsError(try stage.publish(id: id)) { XCTAssertEqual($0 as? PluginReleaseStage.Failure, .conflict) }
        XCTAssertEqual(try names(existing), [], "existing empty slot must not be overwritten")
        try stage.discard()
    }

    func testPrivateRootAndRetainedVersionLimit() throws {
        let parent = try directory(), root = parent.appendingPathComponent("store")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o755])
        XCTAssertThrowsError(try PluginReleaseStage(directory: root))
        XCTAssertEqual(chmod(root.path, 0o700), 0)
        for index in 0..<64 { try FileManager.default.createDirectory(at: root.appendingPathComponent(String(index)), withIntermediateDirectories: false) }
        XCTAssertThrowsError(try PluginReleaseStage(directory: root)) { XCTAssertEqual($0 as? PluginReleaseStage.Failure, .limit) }
    }

    func testPortableArchivePathsHaveExplicitBounds() {
        for path in ["../x", "/x", "a//b", "a/./b", "a/../b", "a\\b", "a:b", "a.", "a ", "e\u{301}"] {
            XCTAssertFalse(PluginReleaseArchive.validPath(path), path)
        }
        XCTAssertTrue(PluginReleaseArchive.validPath(Array(repeating: "x", count: 32).joined(separator: "/")))
        XCTAssertFalse(PluginReleaseArchive.validPath(Array(repeating: "x", count: 33).joined(separator: "/")))
    }
}

@MainActor
final class PluginOfficialReleaseInstallTests: XCTestCase {
    func testSevenOfficialCandidatesInstallWithoutApprovingTheirSources() async throws {
        guard let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_OFFICIAL_RELEASES"] else {
            throw XCTSkip("requires explicit signed official candidate fixtures")
        }
        let fixture = URL(fileURLWithPath: path)
        let root = sessionTestDirectory().appendingPathComponent("official-install-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock {
            try PluginReleaseInstallTests.thaw(root)
            try FileManager.default.removeItem(at: root)
        }
        let owner = try ExtensionController(
            profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1],
                runtimes: ["portable-js"], capabilities: AppModel.extensionHostCapabilities),
            resourceDirectory: root, installationDirectory: root.appendingPathComponent("installed"))
        addTeardownBlock { let closed = await owner.close(); XCTAssertTrue(closed) }
        for name in ["preview", "table", "diff", "task", "views", "ai-codex", "ai-claude"] {
            let release = fixture.appendingPathComponent("official-\(name)-release")
            let prepared = try await owner.prepareSignedRelease(artifact: release.appendingPathComponent("plugin.zip"),
                descriptor: release.appendingPathComponent("release.json"), signature: release.appendingPathComponent("release.sig"),
                trustedPolicy: fixture.appendingPathComponent("official-\(name)-policy.json"))
            XCTAssertEqual(prepared.review.id, "org.ghosterm." + name)
            let installed = try await owner.installPreparedRelease(id: prepared.id)
            XCTAssertTrue(installed.durabilityConfirmed)
            XCTAssertFalse(installed.review.authorizationGranted)
            let manifest = try Data(contentsOf: installed.directory.appendingPathComponent("integrity.json"))
            XCTAssertEqual(SHA256.hash(data: manifest).map { String(format: "%02x", $0) }.joined(), prepared.review.digest)
            let command = try XCTUnwrap(prepared.review.commands.first?.path)
            let resolution = try await owner.resolve(command: command, selectedID: prepared.review.id, selectedDigest: prepared.review.digest)
            XCTAssertNotEqual(resolution.status, 0, name)
        }
    }
}
