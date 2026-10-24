import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel

@MainActor final class SessionCredentialSelectorTests: XCTestCase {
    private let target = SessionRecord.Target(alias: "fixture", host: "example.invalid", user: "user", port: 2222, knownHosts: nil, identityPaths: ["/identity"])
    private func until(_ predicate: () -> Bool) async throws {
        let deadline = Date().addingTimeInterval(3)
        while !predicate(), Date() < deadline { try await Task.sleep(nanoseconds: 2_000_000) }
        XCTAssertTrue(predicate(), "credential owner did not settle")
    }
    func testMethodsMustPrecedeAnyStorageOrPrompt() {
        let touched = expectation(description: "no material before methods"); touched.isInverted = true
        let storage = SessionCredentialSelector.Storage(key: { _ in touched.fulfill(); return nil }, secret: { _, _ in touched.fulfill(); return nil }, save: { _, _, _ in touched.fulfill() }, remove: { _, _ in touched.fulfill() })
        let selector = SessionCredentialSelector(target: target, storage: storage, ask: { _ in touched.fulfill(); return nil }, hostkey: { _, _ in .unknown }, notice: { _ in })
        var failed = false
        selector.prompts.credential(1, 1) { if case .failure = $0 { failed = true } }
        XCTAssertTrue(failed)
        wait(for: [touched], timeout: 0.05)
    }
    func testUnencryptedKeyNeverUsesSecretStorageOrPromptsAndOnlyAcceptedReferencePersists() async throws {
        var delivered = false, asked = 0, secrets = 0
        let storage = SessionCredentialSelector.Storage(key: { _ in "-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n" }, secret: { _, _ in secrets += 1; return nil }, save: { _, _, _ in secrets += 1 }, remove: { _, _ in secrets += 1 })
        let selector = SessionCredentialSelector(target: target, storage: storage, ask: { _ in asked += 1; return nil }, hostkey: { _, _ in .unknown }, notice: { _ in })
        let prompts = selector.prompts
        prompts.methods(1, SSHAuthMethods.publicKey.rawValue, 0)
        prompts.credential(1, 1) { result in
            guard case .success(.privateKey(_, let pass)) = result else { return XCTFail("expected unencrypted key") }
            XCTAssertNil(pass); delivered = true
        }
        try await until { delivered }
        XCTAssertNil(selector.acceptedIdentityPaths, "delivery is not authentication success")
        prompts.accepted(999, 1); XCTAssertNil(selector.acceptedIdentityPaths)
        prompts.accepted(1, 1); XCTAssertEqual(selector.acceptedIdentityPaths, ["/identity"])
        XCTAssertEqual(asked, 0); XCTAssertEqual(secrets, 0)
    }
    func testPasswordSavedOnlyAfterSuccessAndRejectedCacheRemovedBeforeRetry() async throws {
        let lock = NSLock(); var saved: [String] = [], removed = false, reads = 0
        var asks = 0, deliveries = 0
        let storage = SessionCredentialSelector.Storage(key: { _ in XCTFail("password-only server must not read keys"); return nil }, secret: { _, _ in
            lock.lock(); defer { lock.unlock() }; reads += 1
            if reads == 2 { XCTAssertTrue(removed, "remove rejected secret before next lookup") }
            return nil
        }, save: { ref, _, value in
            XCTAssertEqual(ref, .password); lock.lock(); saved.append(value); lock.unlock()
        }, remove: { ref, _ in XCTAssertEqual(ref, .password); lock.lock(); removed = true; lock.unlock() })
        let selector = SessionCredentialSelector(target: target, storage: storage, ask: { _ in asks += 1; return .init(pem: "", password: "fixture-secret-\(asks)") }, hostkey: { _, _ in .unknown }, notice: { _ in })
        let prompts = selector.prompts
        func request(_ id: UInt64, _ error: Int32) { prompts.methods(id, SSHAuthMethods.password.rawValue, error); prompts.credential(id, UInt32(id)) { result in if case .success(.password) = result { deliveries += 1 } else { XCTFail("expected password") } } }
        request(1, 0); try await until { deliveries == 1 }
        lock.lock(); let before = saved; lock.unlock(); XCTAssertTrue(before.isEmpty)
        request(2, 4); try await until { deliveries == 2 }
        prompts.accepted(1, 1); XCTAssertNil(selector.acceptedIdentityPaths)
        prompts.accepted(2, 2)
        try await until { lock.lock(); defer { lock.unlock() }; return saved.count == 1 }
        lock.lock(); let after = saved; lock.unlock()
        XCTAssertEqual(after, ["fixture-secret-2"]); XCTAssertEqual(selector.acceptedIdentityPaths, [])
    }
    func testUnknownKeyFormatIsReportedAndNeverSubmitted() async throws {
        var warnings: [String] = [], status: Int32?
        let storage = SessionCredentialSelector.Storage(key: { _ in "not a key" }, secret: { _, _ in XCTFail("unknown format must not unlock secrets"); return nil }, save: { _, _, _ in }, remove: { _, _ in })
        let selector = SessionCredentialSelector(target: target, storage: storage, ask: { _ in XCTFail("public key only"); return nil }, hostkey: { _, _ in .unknown }, notice: { warnings.append($0) })
        let prompts = selector.prompts
        prompts.methods(1, SSHAuthMethods.publicKey.rawValue, 0)
        prompts.credential(1, 1) { result in
            switch result {
            case .success: status = 0
            case .failure(let error): if case GhostermError.status(let code, _) = error { status = code } else { status = 9 }
            }
        }
        try await until { status != nil }
        XCTAssertEqual(status, 4, "missing usable identity is an authentication failure")
        XCTAssertEqual(warnings.count, 1, "unknown format must remain explicit")
    }
    func testCancellationDiscardsLateStorageResultAndUnsupportedMethodsStayExplicit() async throws {
        let entered = expectation(description: "storage entered"), finished = expectation(description: "storage finished")
        entered.assertForOverFulfill = false; finished.assertForOverFulfill = false
        let release = DispatchSemaphore(value: 0)
        var deliveries = 0, asks = 0
        let storage = SessionCredentialSelector.Storage(key: { _ in entered.fulfill(); _ = release.wait(timeout: .now() + 3); finished.fulfill(); return "-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n" }, secret: { _, _ in nil }, save: { _, _, _ in }, remove: { _, _ in })
        let selector = SessionCredentialSelector(target: target, storage: storage, ask: { _ in asks += 1; return nil }, hostkey: { _, _ in .unknown }, notice: { _ in })
        let prompts = selector.prompts
        prompts.methods(1, SSHAuthMethods.publicKey.rawValue, 0); prompts.credential(1, 1) { _ in deliveries += 1 }
        await fulfillment(of: [entered], timeout: 2)
        prompts.cancelled(1); release.signal(); await fulfillment(of: [finished], timeout: 2)
        try await Task.sleep(nanoseconds: 30_000_000)
        XCTAssertEqual(deliveries, 0); XCTAssertEqual(asks, 0)
        prompts.methods(2, Int32.max & ~SSHAuthMethods.publicKey.rawValue & ~SSHAuthMethods.password.rawValue, 0)
        var unsupported = false
        prompts.credential(2, 1) { result in
            if case .failure(let error) = result, case GhostermError.status(let status, _) = error { unsupported = status == 7 }
        }
        XCTAssertTrue(unsupported)
        release.signal()
        try await Task.sleep(nanoseconds: 30_000_000)
    }
}
