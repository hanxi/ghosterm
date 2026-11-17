import CryptoKit
import Darwin
import Foundation
import GhostermCore
import Security
import XCTest
@testable import GhostermAppModel

final class NativePrivateKeyStoreTests: XCTestCase {
    // Deliberately non-cryptographic material; tests compare hashes, never PEM snapshots.
    private func material(_ tag: String) -> String {
        "-----BEGIN RSA PRIVATE KEY-----\n" + tag + "\n-----END RSA PRIVATE KEY-----\n"
    }
    private func digest(_ value: String?) -> Data { Data(SHA256.hash(data: Data((value ?? "").utf8))) }
    private func fixture(_ text: String? = nil) throws -> String {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("native-key-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let path = dir.appendingPathComponent("identity").path
        if let text { try Data(text.utf8).write(to: URL(fileURLWithPath: path)); XCTAssertEqual(chmod(path, 0o600), 0) }
        addTeardownBlock {
            KeychainStore.removePrivateKeyPEM(forPath: path)
            var missing = false
            do { _ = try KeychainStore.privateKeyPEM(forPath: path) }
            catch KeychainStoreError.notFound { missing = true }
            catch { XCTFail("owned Keychain record cleanup failed: \(error)") }
            XCTAssertTrue(missing, "test must not leave a Keychain record")
            try? FileManager.default.removeItem(at: dir)
        }
        return path
    }

    func testFirstImportThenAuthenticationSurvivesRemovedSource() throws {
        let expected = material("FIRST"), path = try fixture(material("FIRST"))
        let first = try NativePrivateKeyStore.load(path)
        try FileManager.default.removeItem(atPath: path)
        let cached = try NativePrivateKeyStore.load(path)
        XCTAssertTrue(digest(first) == digest(expected) && digest(cached) == digest(expected),
                      "both first and later loads must use stored material; source removal must not break it")
    }

    func testOnlyExplicitReimportReplacesStoredMaterialAndNeverWritesSource() throws {
        let first = material("FIRST"), second = material("SECOND"), path = try fixture(material("FIRST"))
        _ = try NativePrivateKeyStore.load(path)
        try Data(second.utf8).write(to: URL(fileURLWithPath: path))
        let before = try NativePrivateKeyStore.load(path)
        try NativePrivateKeyStore.reimport(path)
        let after = try NativePrivateKeyStore.load(path)
        XCTAssertTrue(digest(before) == digest(first) && digest(after) == digest(second) &&
                      (try? Data(contentsOf: URL(fileURLWithPath: path))) == Data(second.utf8),
                      "Keychain is authoritative; explicit replacement must preserve the user's source bytes")
    }

    func testConcurrentFirstImportUsesWinningKeychainRecord() throws {
        let winner = material("WINNER"), path = try fixture(material("LOSER"))
        var access = NativePrivateKeyStore.Access.system
        access.insert = { candidate, path in
            try KeychainStore.insertPrivateKeyPEM(winner, forPath: path)
            try KeychainStore.insertPrivateKeyPEM(candidate, forPath: path)
        }
        XCTAssertTrue(digest(try NativePrivateKeyStore.load(path, access: access)) == digest(winner),
                      "a losing import must not overwrite or submit its file snapshot")
    }

    func testKeychainWriteFailureCannotYieldFileMaterial() throws {
        let path = try fixture(material("FIRST"))
        var access = NativePrivateKeyStore.Access.system
        access.insert = { _, _ in throw KeychainStoreError.unexpectedStatus(errSecAuthFailed) }
        var rejected = false
        do { _ = try NativePrivateKeyStore.load(path, access: access) }
        catch KeychainStoreError.unexpectedStatus(let status) { rejected = status == errSecAuthFailed }
        catch { }
        XCTAssertTrue(rejected, "a denied secure-store write cannot fall back to submitting a disk key")
    }

    func testPostImportReadFailureCannotYieldFileMaterial() throws {
        let path = try fixture(material("FIRST"))
        var access = NativePrivateKeyStore.Access.system
        var reads = 0
        access.get = { path in
            reads += 1
            if reads == 1 { throw KeychainStoreError.notFound }
            throw KeychainStoreError.unexpectedStatus(errSecInteractionNotAllowed)
        }
        var rejected = false
        do { _ = try NativePrivateKeyStore.load(path, access: access) }
        catch { rejected = true }
        XCTAssertTrue(rejected && reads == 2, "the imported file's bytes cannot substitute for a Keychain unlock/read")
    }

    func testFailedExplicitReplacementKeepsExistingMaterial() throws {
        let original = material("FIRST"), path = try fixture(material("FIRST"))
        _ = try NativePrivateKeyStore.load(path)
        try Data(material("SECOND").utf8).write(to: URL(fileURLWithPath: path))
        var access = NativePrivateKeyStore.Access.system
        access.replace = { _, _ in throw KeychainStoreError.unexpectedStatus(errSecAuthFailed) }
        var rejected = false
        do { try NativePrivateKeyStore.reimport(path, access: access) }
        catch { rejected = true }
        let observed = try NativePrivateKeyStore.load(path)
        XCTAssertTrue(rejected && digest(observed) == digest(original))
    }

    func testInvalidKeychainUTF8CannotBeReplacedByAFileFallback() throws {
        let path = try fixture(material("FIRST"))
        let malformed = Data("-----BEGIN RSA PRIVATE KEY-----\n".utf8) + Data([0xff]) + Data("\n-----END RSA PRIVATE KEY-----\n".utf8)
        try KeychainStoreTestHooks.set(malformed, account: KeychainStore.keyAccount(path))
        var rejected = false
        do { _ = try NativePrivateKeyStore.load(path) }
        catch { rejected = true }
        XCTAssertTrue(rejected, "corrupt cached material is an explicit error, never missing")
    }

    func testMissingPrivateFilesRemainMissing() throws {
        let path = try fixture()
        XCTAssertNil(try NativePrivateKeyStore.load(path))
    }

    func testUnsupportedAndroidContainerReferenceIsExplicit() throws {
        var unsupported = false
        do { _ = try NativePrivateKeyStore.load("ghosterm-identity:00000000-0000-0000-0000-000000000000") }
        catch GhostermError.status(let code, let message) { unsupported = code == 7 && message.contains("Android") }
        XCTAssertTrue(unsupported)
    }

    func testSourcePermissionsSymlinksAndMalformedBytesAreRefused() throws {
        let path = try fixture(material("FIRST"))
        XCTAssertEqual(chmod(path, 0o644), 0)
        var permission = false, symlinkRefused = false, invalid = false
        do { _ = try NativePrivateKeyStore.load(path) } catch { permission = true }
        try FileManager.default.removeItem(atPath: path)
        let target = path + ".target"
        try Data(material("FIRST").utf8).write(to: URL(fileURLWithPath: target))
        XCTAssertEqual(chmod(target, 0o600), 0)
        try FileManager.default.createSymbolicLink(atPath: path, withDestinationPath: target)
        do { _ = try NativePrivateKeyStore.load(path) } catch { symlinkRefused = true }
        try FileManager.default.removeItem(atPath: path)
        try Data([0xff, 0]).write(to: URL(fileURLWithPath: path)); XCTAssertEqual(chmod(path, 0o600), 0)
        do { _ = try NativePrivateKeyStore.load(path) } catch { invalid = true }
        XCTAssertTrue(permission && symlinkRefused && invalid)
    }
    func testSourceReplacementDuringReadCannotBeImported() throws {
        let path = try fixture(material("FIRST"))
        var io = NativePrivateKeyStore.ImportIO(), changed = false
        io.read = { fd, buffer, capacity in
            let count = Darwin.read(fd, buffer, capacity)
            if !changed {
                changed = true
                do {
                    try FileManager.default.moveItem(atPath: path, toPath: path + ".old")
                    try Data(self.material("REPLACEMENT").utf8).write(to: URL(fileURLWithPath: path))
                    _ = chmod(path, 0o600)
                } catch { XCTFail("owned source replacement failed") }
            }
            return count
        }
        var rejected = false
        do { _ = try NativePrivateKeyStore.load(path, io: io) } catch { rejected = true }
        XCTAssertTrue(rejected && changed, "import must bind the read fd to the unchanged source identity")
    }

    func testCancelledLookupCannotReadOrImportTheFile() throws {
        let path = try fixture(material("FIRST")), token = SSHCancellation()
        var access = NativePrivateKeyStore.Access.system, io = NativePrivateKeyStore.ImportIO()
        var reads = 0, writes = 0, cancelled = false
        access.get = { _ in token.cancel(); throw KeychainStoreError.notFound }
        access.insert = { _, _ in writes += 1 }
        io.read = { _, _, _ in reads += 1; return 0 }
        do { _ = try NativePrivateKeyStore.load(path, access: access, io: io, cancellation: token) }
        catch GhostermError.status(let code, _) { cancelled = code == 8 }
        catch { }
        XCTAssertTrue(cancelled && reads == 0 && writes == 0)
    }

    func testCancellationDuringFileReadStopsBeforeKeychainWrite() throws {
        let path = try fixture(material("FIRST")), token = SSHCancellation()
        var access = NativePrivateKeyStore.Access.system, io = NativePrivateKeyStore.ImportIO()
        var writes = 0, cancelled = false
        access.insert = { _, _ in writes += 1 }
        io.read = { fd, buffer, capacity in
            let count = Darwin.read(fd, buffer, capacity)
            token.cancel()
            return count
        }
        do { _ = try NativePrivateKeyStore.load(path, access: access, io: io, cancellation: token) }
        catch GhostermError.status(let code, _) { cancelled = code == 8 }
        catch { }
        XCTAssertTrue(cancelled && writes == 0)
    }

    func testCompletedImportMayRemainButCancelledRequestGetsNoMaterial() throws {
        let path = try fixture(material("FIRST")), token = SSHCancellation()
        var access = NativePrivateKeyStore.Access.system, cancelled = false
        access.insert = { value, path in try KeychainStore.insertPrivateKeyPEM(value, forPath: path); token.cancel() }
        do { _ = try NativePrivateKeyStore.load(path, access: access, cancellation: token) }
        catch GhostermError.status(let code, _) { cancelled = code == 8 }
        catch { }
        let stored = try KeychainStore.privateKeyPEM(forPath: path)
        XCTAssertTrue(cancelled && digest(stored) == digest(material("FIRST")))
    }

    @MainActor
    func testProductionSelectorUsesImportedMaterialAfterSourceRemoval() async throws {
        let expected = material("FIRST"), path = try fixture(material("FIRST"))
        _ = try NativePrivateKeyStore.load(path)
        try FileManager.default.removeItem(atPath: path)
        let target = SSHConnectionTarget(alias: "owned", host: "example.invalid", user: "user", port: 22, knownHosts: nil, identityPaths: [path])
        var deliveredDigest: Data?, asks = 0
        let done = expectation(description: "production private store delivers")
        let selector = SSHCredentialSelector(target: target, ask: { _ in asks += 1; return nil }, hostkey: { _, _ in .unknown }, notice: { _ in })
        selector.prompts.methods(1, SSHAuthMethods.publicKey.rawValue, 0)
        selector.prompts.credential(1, 1) { result in
            if case .success(.privateKey(let pem, _)) = result { deliveredDigest = self.digest(pem) }
            done.fulfill()
        }
        await fulfillment(of: [done], timeout: 3)
        XCTAssertTrue(deliveredDigest == digest(expected) && asks == 0)
    }

}
