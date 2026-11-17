import AppKit
import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel

private final class OpeningObservation: @unchecked Sendable {
    private let lock = NSLock()
    private var milliseconds: UInt32 = 0
    private var value = false
    func record(_ milliseconds: UInt32, _ value: Bool) {
        lock.lock(); self.milliseconds = milliseconds; self.value = value; lock.unlock()
    }
    var snapshot: (UInt32, Bool) { lock.lock(); defer { lock.unlock() }; return (milliseconds, value) }
}

final class SSHConnectionOpeningTests: XCTestCase {
    override class func setUp() { super.setUp(); _ = NSApplication.shared; TestWindows.enterAutomationMode() }
    private func status(_ operation: () throws -> Void) -> Int32 {
        do { try operation(); return 0 }
        catch GhostermError.status(let code, _) { return code }
        catch { return -1 }
    }
    func testRemainingBudgetNeverRestartsAfterAuthentication() throws {
        var instant: TimeInterval = 100
        let opening = SSHConnectionOpening(timeout: 60, now: { instant })
        instant = 158
        XCTAssertEqual(try opening.remainingMilliseconds(), 2000)
    }
    func testSFTPCapDoesNotExceedRemainingLifetime() throws {
        var instant: TimeInterval = 100
        let opening = SSHConnectionOpening(timeout: 60, now: { instant })
        let cap = try opening.remainingMilliseconds(maximum: 5000)
        instant = 159
        let remaining = try opening.remainingMilliseconds(maximum: 5000)
        XCTAssertTrue(cap == 5000 && remaining == 1000)
    }
    func testExpiredAndInvalidBudgetCannotOpenAChannel() {
        let invalid: [TimeInterval] = [0, -1, .infinity, .nan]
        let codes = invalid.map { timeout in status { _ = try SSHConnectionOpening(timeout: timeout).remainingMilliseconds() } }
        XCTAssertEqual(codes, [3, 3, 3, 3])
    }
    func testCancellationWinsOverTimeout() {
        let opening = SSHConnectionOpening(timeout: -1); opening.cancel()
        XCTAssertEqual(status { _ = try opening.remainingMilliseconds() }, 8)
    }

    private func checkFlow(route: Bool, expired: Bool) {
        let owner = DispatchQueue(label: "opening.preflight")
        let completed = expectation(description: "opening stopped before DNS")
        owner.async {
            let opening = SSHConnectionOpening(timeout: expired ? -1 : 60)
            if !expired { opening.cancel() }
            var lookups = 0, resultCode: Int32 = 0
            let prompt = SSHAuthenticationPrompts(credential: { _, _, _ in XCTFail("credentials after cancellation") },
                hostkey: { _, _, _, _ in XCTFail("trust after cancellation") })
            let lookup: SSHAuthenticationFlow.AddressLookup = { _, _, _, _ in lookups += 1; return nil }
            let result: (Result<SSHAuthenticationFlow.Authenticated, SSHAuthenticationFlow.Failure>) -> Void = {
                if case .failure(let error) = $0 { resultCode = error.status }
            }
            let flow: SSHAuthenticationOperation
            if route {
                flow = SSHRouteAuthenticationFlow(owner: owner,
                    options: [.init(host: "first.invalid", username: "fixture"), .init(host: "inner.invalid", username: "fixture")],
                    prompts: [prompt, prompt], opening: opening, lookup: lookup, nextID: { nil }, completion: result)
            } else {
                flow = SSHAuthenticationFlow(owner: owner, options: .init(host: "first.invalid", username: "fixture"),
                    opening: opening, lookup: lookup, prompts: prompt, nextID: { nil }, completion: result)
            }
            flow.start()
            let stopped = resultCode == (expired ? 3 : 8) && lookups == 0
            flow.cancel()
            XCTAssertTrue(stopped, "cancelled/expired admission must stop synchronously before DNS")
            completed.fulfill()
        }
        wait(for: [completed], timeout: 3)
    }
    func testCancelledAuthenticationDoesNotResolve() { checkFlow(route: false, expired: false) }
    func testExpiredAuthenticationDoesNotGetAnotherMinute() { checkFlow(route: false, expired: true) }
    func testCancelledRouteDoesNotResolve() { checkFlow(route: true, expired: false) }
    func testExpiredRouteDoesNotGetAnotherMinute() { checkFlow(route: true, expired: true) }

    private func checkLateDNS(route: Bool) {
        let owner = DispatchQueue(label: "opening.late-dns")
        let complete = expectation(description: "late DNS rejected")
        owner.async {
            let opening = SSHConnectionOpening()
            var callback: ((Result<SSHAddress, SSHAddressResolver.Failure>) -> Void)?
            var resultCode: Int32 = 0
            let prompt = SSHAuthenticationPrompts(credential: { _, _, _ in XCTFail("late credentials") }, hostkey: { _, _, _, _ in XCTFail("late trust") })
            let lookup: SSHAuthenticationFlow.AddressLookup = { _, _, _, done in callback = done; return nil }
            let result: (Result<SSHAuthenticationFlow.Authenticated, SSHAuthenticationFlow.Failure>) -> Void = {
                if case .failure(let error) = $0 { resultCode = error.status }
            }
            let flow: SSHAuthenticationOperation
            if route {
                flow = SSHRouteAuthenticationFlow(owner: owner,
                    options: [.init(host: "first.invalid", username: "fixture"), .init(host: "inner.invalid", username: "fixture")],
                    prompts: [prompt, prompt], opening: opening, lookup: lookup, nextID: { nil }, completion: result)
            } else {
                flow = SSHAuthenticationFlow(owner: owner, options: .init(host: "first.invalid", username: "fixture"),
                    opening: opening, lookup: lookup, prompts: prompt, nextID: { nil }, completion: result)
            }
            flow.start(); opening.cancel()
            callback?(.failure(.unavailable(-1)))
            let rejected = resultCode == 8
            flow.cancel()
            XCTAssertTrue(rejected, "late resolver failure must not replace cancellation before the next timer tick")
            complete.fulfill()
        }
        wait(for: [complete], timeout: 3)
    }
    func testLateAuthenticationDNSCannotReplaceCancellation() { checkLateDNS(route: false) }
    func testLateRouteDNSCannotReplaceCancellation() { checkLateDNS(route: true) }

    @MainActor func testOldOpeningCompletionCannotRemoveReplacement() throws {
        let directory = sessionTestDirectory()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let model = isolatedAppModel(configPath: directory.appendingPathComponent("config").path,
            knownHostsPath: directory.appendingPathComponent("known_hosts").path,
            settings: SettingsStore(terminalConfigPath: directory.appendingPathComponent("ghostty").path))
        defer { model.disconnectForTesting() }
        let id = UUID(), old = model.beginConnectionOpening(id), new = model.beginConnectionOpening(id)
        model.finishConnectionOpening(id, opening: old)
        model.cancelConnection(id)
        XCTAssertTrue(old.cancellation.isCancelled && new.cancellation.isCancelled,
            "an old completion must neither retain its old scope nor detach the replacement's cancellation")
    }
    @MainActor func testOldAuthenticationCompletionCannotRemoveReplacement() throws {
        final class Operation: SSHAuthenticationOperation {
            var cancelled = false
            func start() {}
            func cancel() { cancelled = true }
        }
        let directory = sessionTestDirectory()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let model = isolatedAppModel(configPath: directory.appendingPathComponent("config").path,
            knownHostsPath: directory.appendingPathComponent("known_hosts").path,
            settings: SettingsStore(terminalConfigPath: directory.appendingPathComponent("ghostty").path))
        defer { model.disconnectForTesting() }
        let id = UUID(), oldGeneration = UUID(), old = Operation(), new = Operation()
        let owner = DispatchQueue(label: "opening.auth-generation")
        model.registerAuthentication(id, owner: owner, flow: old, generation: oldGeneration)
        model.registerAuthentication(id, owner: owner, flow: new, generation: UUID())
        model.finishAuthentication(id, generation: oldGeneration)
        model.cancelConnection(id)
        let cancelled = owner.sync { old.cancelled && new.cancelled }
        XCTAssertTrue(cancelled, "an old authentication completion must not remove the new flow")
    }

    private struct Fixture { let host, user, key: String; let port: UInt16 }
    private func fixture() throws -> Fixture {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let key = env["GHOSTERM_SSH_KEY"], let raw = env["GHOSTERM_SSH_PORT"], let port = UInt16(raw) else {
            throw XCTSkip("requires the owned loopback sshd fixture")
        }
        return Fixture(host: host, user: user, key: key, port: port)
    }
    @MainActor private func model(_ fixture: Fixture) throws -> AppModel {
        let directory = sessionTestDirectory()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: directory) }
        let path = directory.appendingPathComponent("config").path
        try "Host fixture\n HostName \(fixture.host)\n User \(fixture.user)\n Port \(fixture.port)\n IdentityFile /fixture/memory-key\n".write(toFile: path, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: path, knownHostsPath: directory.appendingPathComponent("known_hosts").path,
            settings: SettingsStore(terminalConfigPath: directory.appendingPathComponent("ghostty").path))
        let pem = try String(contentsOfFile: fixture.key, encoding: .utf8)
        model.interaction = RecordingInteraction()
        model.sshCredentialStorage = .init(key: { $0 == "/fixture/memory-key" ? pem : nil }, secret: { _, _ in nil },
            save: { _, _, _ in XCTFail("unexpected save") }, remove: { _, _ in })
        return model
    }
    @MainActor private func wait(_ condition: () -> Bool, seconds: Double = 12) -> Bool {
        let end = Date().addingTimeInterval(seconds)
        while Date() < end { if condition() { return true }; RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
        return condition()
    }
    @MainActor private func waitAsync(_ condition: () -> Bool, seconds: Double = 12) async -> Bool {
        let end = Date().addingTimeInterval(seconds)
        while Date() < end {
            if condition() { return true }
            try? await Task.sleep(nanoseconds: 10_000_000)
        }
        return condition()
    }
    @MainActor func testTerminalProductUsesRemainingBudgetAndSharedToken() throws {
        let model = try model(fixture()); defer { model.disconnectForTesting() }
        let opening = SSHConnectionOpening(timeout: 12)
        model.makeSSHConnectionOpening = { opening }
        let observed = OpeningObservation(), production = SSHOpeningOperations()
        model.sshOpeningOperations.shell = { ssh, ms, token in
            observed.record(ms, token === opening.cancellation)
            return try production.shell(ssh, ms, token)
        }
        model.connect(to: .host("fixture"))
        let ready = wait { model.activePane?.isRunning == true }
        let (ms, token) = observed.snapshot
        XCTAssertTrue(ready && (1..<12_000).contains(ms) && token, "ordinary terminal must pass its original remaining budget and cancel token to controlled Core")
    }
    @MainActor func testSFTPProductUsesRemainingBudgetAndSharedToken() async throws {
        let fixture = try fixture(), model = try model(fixture)
        defer { model.disconnectForTesting() }
        let opening = SSHConnectionOpening(timeout: 4)
        model.makeSSHConnectionOpening = { opening }
        let observed = OpeningObservation(), production = SSHOpeningOperations()
        model.sshOpeningOperations.sftp = { ssh, ms, token in
            observed.record(ms, token === opening.cancellation)
            return try production.sftp(ssh, ms, token)
        }
        let backend = try await model.makeSFTPFileBackend(user: fixture.user, host: fixture.host, port: fixture.port,
            identityFiles: ["/fixture/memory-key"], knownHosts: model.knownHostsPath, cancelID: UUID(), alias: "fixture")
        let (ms, token) = observed.snapshot
        let root = try backend.realpath(".")
        XCTAssertTrue((1..<4000).contains(ms) && token && root.hasPrefix("/"), "SFTP must keep the remaining budget instead of restarting five seconds")
    }
    @MainActor func testTerminalStopCancelsWhileOpeningOwnerIsBusy() throws {
        let model = try model(fixture()); defer { model.disconnectForTesting() }
        let entered = OpeningObservation(), stopped = OpeningObservation()
        let release = DispatchSemaphore(value: 0); defer { release.signal() }
        let production = SSHOpeningOperations()
        model.sshOpeningOperations.shell = { ssh, ms, token in
            entered.record(1, true)
            _ = release.wait(timeout: .now() + 8)
            let wasCancelled = token.isCancelled
            do {
                let transport = try production.shell(ssh, ms, token)
                transport.releaseOn(DispatchQueue.global()); stopped.record(1, wasCancelled)
                return transport
            } catch { stopped.record(8, wasCancelled); throw error }
        }
        model.connect(to: .host("fixture"))
        let pane = try XCTUnwrap(model.activePane)
        guard wait({ entered.snapshot.1 }) else { return XCTFail("authentication did not reach the actual shell boundary") }
        pane.stop(); release.signal()
        let finished = wait { stopped.snapshot.0 != 0 }
        // Drain the continuation back onto main; no timer-based cancellation is queued ahead of the blocked owner.
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        XCTAssertTrue(finished && stopped.snapshot.0 == 8 && stopped.snapshot.1 && !pane.isRunning && pane.surface == nil && pane.status.label.contains("已取消"),
            "stop must flip the shared token immediately and prevent late attachment")
    }
    @MainActor func testSFTPTaskCancellationReachesBusyOpeningOwner() async throws {
        let fixture = try fixture(), model = try model(fixture)
        defer { model.disconnectForTesting() }
        let entered = OpeningObservation(), stopped = OpeningObservation()
        let release = DispatchSemaphore(value: 0); defer { release.signal() }
        let production = SSHOpeningOperations()
        model.sshOpeningOperations.sftp = { ssh, ms, token in
            entered.record(1, true)
            _ = release.wait(timeout: .now() + 8)
            stopped.record(1, token.isCancelled)
            return try production.sftp(ssh, ms, token)
        }
        let task = Task {
            try await model.makeSFTPFileBackend(user: fixture.user, host: fixture.host, port: fixture.port,
                identityFiles: ["/fixture/memory-key"], knownHosts: model.knownHostsPath, cancelID: UUID(), alias: "fixture")
        }
        guard await waitAsync({ entered.snapshot.1 }) else { task.cancel(); return XCTFail("SFTP did not reach the actual subsystem boundary") }
        task.cancel(); release.signal()
        var code: Int32 = 0
        do { _ = try await task.value }
        catch GhostermError.status(let status, _) { code = status }
        catch { code = -1 }
        XCTAssertTrue(code == 8 && stopped.snapshot.1, "Swift task cancellation must reach controlled SFTP while its IO owner is blocked")
    }
    @MainActor func testTerminalLateSuccessCannotAttachAfterCancellation() throws {
        let model = try model(fixture()); defer { model.disconnectForTesting() }
        let opening = SSHConnectionOpening(), done = OpeningObservation(), production = SSHOpeningOperations()
        model.makeSSHConnectionOpening = { opening }
        model.sshOpeningOperations.shell = { ssh, ms, token in
            let transport = try production.shell(ssh, ms, token)
            opening.cancel(); done.record(1, true)
            return transport
        }
        model.connect(to: .host("fixture"))
        let pane = try XCTUnwrap(model.activePane)
        let finished = wait { done.snapshot.1 }
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        XCTAssertTrue(finished && !pane.isRunning && pane.surface == nil && pane.status.label.contains("已取消"),
            "cancellation after native success but before the main handoff must discard the transport")
    }
    @MainActor func testSFTPLateSuccessIsClosedBeforeCancellationReturns() async throws {
        let fixture = try fixture(), model = try model(fixture)
        defer { model.disconnectForTesting() }
        let opening = SSHConnectionOpening(), production = SSHOpeningOperations()
        model.makeSSHConnectionOpening = { opening }
        model.sshOpeningOperations.sftp = { ssh, ms, token in
            let sftp = try production.sftp(ssh, ms, token)
            opening.cancel()
            return sftp
        }
        var code: Int32 = 0
        do {
            _ = try await model.makeSFTPFileBackend(user: fixture.user, host: fixture.host, port: fixture.port,
                identityFiles: ["/fixture/memory-key"], knownHosts: model.knownHostsPath, cancelID: UUID(), alias: "fixture")
        } catch GhostermError.status(let status, _) { code = status }
        catch { code = -1 }
        XCTAssertEqual(code, 8, "a completed subsystem must be retired on its owner instead of returning a cancelled backend")
    }
    @MainActor func testCancellationInterruptsARealStalledSFTPSubsystem() async throws {
        let fixture = try fixture()
        guard let raw = ProcessInfo.processInfo.environment["GHOSTERM_SSH_SFTP_STALL_PORT"], let port = UInt16(raw) else {
            throw XCTSkip("requires the owned sshd with a deliberately stalled SFTP subsystem")
        }
        let model = try model(fixture); defer { model.disconnectForTesting() }
        let entered = OpeningObservation(), completed = OpeningObservation(), production = SSHOpeningOperations()
        model.sshOpeningOperations.sftp = { ssh, ms, token in
            entered.record(1, true)
            do { let sftp = try production.sftp(ssh, ms, token); completed.record(1, true); return sftp }
            catch { completed.record(1, true); throw error }
        }
        let id = UUID()
        let task = Task {
            try await model.makeSFTPFileBackend(user: fixture.user, host: fixture.host, port: port,
                identityFiles: ["/fixture/memory-key"], knownHosts: model.knownHostsPath, cancelID: id, alias: "fixture")
        }
        guard await waitAsync({ entered.snapshot.1 }) else { task.cancel(); return XCTFail("stalled subsystem did not reach controlled Core") }
        // No test semaphore wraps this operation: the real server accepts the
        // channel and starts a subsystem which never answers SFTP INIT.
        try await Task.sleep(nanoseconds: 150_000_000)
        let waitingInCore = !completed.snapshot.1
        let start = ProcessInfo.processInfo.systemUptime
        model.cancelConnection(id)
        var code: Int32 = 0
        do { _ = try await task.value }
        catch GhostermError.status(let status, _) { code = status }
        catch { code = -1 }
        XCTAssertTrue(waitingInCore && code == 8 && ProcessInfo.processInfo.systemUptime - start < 2,
            "ordinary cancellation must interrupt the actual SFTP protocol wait, not just reject a result afterwards")
    }
    @MainActor func testControlledOpenersRejectPreCancelledToken() async throws {
        let fixture = try fixture(), model = try model(fixture)
        defer { model.disconnectForTesting() }
        let owner = DispatchQueue(label: "opening.bindings"), token = SSHCancellation(); token.cancel()
        let ssh = try await model.authenticate(model.sshTarget(for: "fixture"), id: UUID(), owner: owner)
        let valid = try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Bool, Error>) in
            owner.async {
                defer { ssh.close() }
                let production = SSHOpeningOperations()
                var shellCode: Int32 = 0, sftpCode: Int32 = 0
                do { let sftp = try production.sftp(ssh, 5000, token); sftp.close() }
                catch GhostermError.status(let code, _) { sftpCode = code }
                catch { sftpCode = -1 }
                var usableAfterSFTPPreflight = false
                do {
                    let sftp = try ssh.sftp(timeoutMilliseconds: 5000); defer { sftp.close() }
                    usableAfterSFTPPreflight = try sftp.realpath(".").hasPrefix("/")
                } catch { continuation.resume(throwing: error); return }
                do { _ = try production.shell(ssh, 5000, token) }
                catch GhostermError.status(let code, _) { shellCode = code }
                catch { shellCode = -1 }
                continuation.resume(returning: shellCode == 8 && sftpCode == 8 && usableAfterSFTPPreflight)
            }
        }
        XCTAssertTrue(valid, "both production bindings must pass cancellation to Core; SFTP preflight rejection leaves SSH usable (shell cancellation may discard it)")
    }
}
