import Dispatch
import Foundation
import XCTest
@testable import GhostermCore

final class SessionManagementTests: XCTestCase {
    private func onOwner(_ body: @escaping (DispatchQueue) throws -> Void) {
        let done = expectation(description: "management owner")
        let owner = DispatchQueue(label: "dev.ghosterm.test.management")
        owner.async { do { try body(owner) } catch { XCTFail("\(error)") }; done.fulfill() }
        wait(for: [done], timeout: 25)
    }
    private func options() -> RecoverableSession.Options {
        .init(kind: .local, proxy: "/fixture/no-proxy", daemon: "/fixture/no-daemon", runtime: "/fixture/no-runtime",
              compatibility: Array(repeating: 1, count: 32), format: 0)
    }
    private var unavailable: RecoverableSession.Sink {
        .init(snapshot: { _, _, _, _, _ in .failed(7) }, event: { _, _, _ in .failed(7) }, applied: { nil }, invalidated: { _, _ in }, receipt: { _ in })
    }
    func testCatalogNeedsNoSurfaceAndPreservesUnknownList() {
        var config = options(); config.catalogOnly = true
        onOwner { owner in
            let session = try RecoverableSession(options: config, owner: owner)
            XCTAssertNil(try session.catalog()); XCTAssertEqual(session.info?.stop.state, RecoverableSession.StopState.none)
            XCTAssertFalse(session.info?.hasAdmittedCreate ?? true)
            XCTAssertEqual(session.refreshCatalog(), .locked); XCTAssertEqual(session.terminate(session: 5), .locked)
            XCTAssertEqual(session.terminate(session: 0), .invalid)
            session.cancel(); XCTAssertTrue(session.close())
        }
    }
    func testPendingLookupRequiresOriginalInstanceAndRejectsMixedModes() {
        let callback = unavailable
        onOwner { owner in
            var config = self.options(); config.format = 0x47545643; config.pendingNonce[0] = 9
            XCTAssertThrowsError(try RecoverableSession(options: config, sink: callback, owner: owner))
            config.instance[0] = 1
            let session = try RecoverableSession(options: config, sink: callback, owner: owner)
            XCTAssertTrue(session.info?.hasAdmittedCreate == true); XCTAssertEqual(session.info?.session, 0)
            XCTAssertThrowsError(try session.catalog()); session.cancel(); XCTAssertTrue(session.close())
            config.catalogOnly = true
            XCTAssertThrowsError(try RecoverableSession(options: config, owner: owner))
            config.catalogOnly = false; config.pendingNonce.removeLast()
            XCTAssertThrowsError(try RecoverableSession(options: config, sink: callback, owner: owner))
            config = self.options(); config.session = 5; config.instance[0] = 1; config.format = 0x47545643
            XCTAssertThrowsError(try RecoverableSession(options: config, owner: owner), "terminal mode must declare an actual engine sink")
        }
    }
    func testRealLocalDaemonManagement() throws {
        let env = ProcessInfo.processInfo.environment
        guard let runtime = env["GHOSTERM_REPLICA_RUNTIME"], let helpers = env["GHOSTERM_REPLICA_HELPERS"],
              let hex = env["GHOSTERM_REPLICA_CONTRACT"], hex.count == 64 else { throw XCTSkip("run scripts/probe-recoverable-transport.py --management") }
        let bytes = stride(from: 0, to: 64, by: 2).compactMap { n -> UInt8? in
            let a = hex.index(hex.startIndex, offsetBy: n); return UInt8(hex[a..<hex.index(a, offsetBy: 2)], radix: 16)
        }
        let callback = unavailable
        onOwner { owner in
            var options = RecoverableSession.Options(kind: .local, proxy: helpers + "/ghosterm-session-proxy", daemon: helpers + "/ghosterm-sessiond", runtime: runtime, compatibility: bytes, format: 0x47545643,
                create: .init(nonce: Array(UUID().uuidString.utf8.prefix(16)), executable: "/bin/sh", cwd: "/", arguments: ["-c", "IFS= read -r line"], size: .init(cols: 80, rows: 24), direct: true))
            let source = try RecoverableSession(options: options, sink: callback, owner: owner)
            var deadline = ProcessInfo.processInfo.systemUptime + 10
            while source.step() != .failed { guard ProcessInfo.processInfo.systemUptime < deadline else { throw GhostermError.failed("fixture renderer rejection deadline") }; Thread.sleep(forTimeInterval: 0.001) }
            guard let identity = source.info else { throw GhostermError.failed("missing created source identity") }
            XCTAssertEqual(identity.error, 7); XCTAssertGreaterThan(identity.session, 0); XCTAssertTrue(identity.hasAdmittedCreate)
            while !source.close() { guard ProcessInfo.processInfo.systemUptime < deadline else { throw GhostermError.failed("source proxy reap deadline") }; Thread.sleep(forTimeInterval: 0.001) }
            options.create = nil; options.catalogOnly = true; options.format = 0; options.instance = identity.instance
            var catalog = try RecoverableSession(options: options, owner: owner)
            defer {
                catalog.cancel(); let closeDeadline = ProcessInfo.processInfo.systemUptime + 3
                while !catalog.close(), ProcessInfo.processInfo.systemUptime < closeDeadline { Thread.sleep(forTimeInterval: 0.001) }
                XCTAssertNil(catalog.info, "catalog proxy reap deadline")
            }
            XCTAssertNil(try catalog.catalog())
            deadline = ProcessInfo.processInfo.systemUptime + 10
            while catalog.step() != .catalog { guard catalog.info?.state != .failed, ProcessInfo.processInfo.systemUptime < deadline else { throw GhostermError.failed("catalog deadline") }; Thread.sleep(forTimeInterval: 0.001) }
            let active = try XCTUnwrap(catalog.catalog()?.first { $0.session == identity.session })
            XCTAssertFalse(active.hasResult); XCTAssertFalse(active.hasExitCode); XCTAssertEqual(active.exitCode, 0)
            XCTAssertEqual(active.session, identity.session); XCTAssertEqual(active.phase, 0); XCTAssertFalse(active.terminating)
            XCTAssertFalse(active.hasSequence); XCTAssertEqual(active.sequence, 0)
            XCTAssertEqual(catalog.terminate(session: identity.session), .queued)
            catalog.cancel()
            XCTAssertEqual(catalog.info?.stop.state, .unknown); XCTAssertEqual(catalog.info?.stop.error, 8)
            XCTAssertEqual(catalog.info?.stop.session, identity.session)
            while !catalog.close() { guard ProcessInfo.processInfo.systemUptime < deadline else { throw GhostermError.failed("cancelled catalog reap deadline") }; Thread.sleep(forTimeInterval: 0.001) }
            catalog = try RecoverableSession(options: options, owner: owner)
            while catalog.step() != .catalog { guard catalog.info?.state != .failed, ProcessInfo.processInfo.systemUptime < deadline else { throw GhostermError.failed("fresh catalog deadline") }; Thread.sleep(forTimeInterval: 0.001) }
            XCTAssertEqual(try catalog.catalog()?.first { $0.session == identity.session }?.hasResult, false)
            XCTAssertEqual(catalog.info?.stop.state, RecoverableSession.StopState.none)
            XCTAssertEqual(catalog.terminate(session: identity.session), .queued)
            while catalog.info?.stop.state == .pending { guard ProcessInfo.processInfo.systemUptime < deadline else { throw GhostermError.failed("termination deadline") }; _ = catalog.step(); Thread.sleep(forTimeInterval: 0.001) }
            XCTAssertEqual(catalog.info?.stop.state, .known); XCTAssertEqual(catalog.info?.stop.status, 0)
            XCTAssertEqual(catalog.info?.stop.session, identity.session); XCTAssertGreaterThan(catalog.info?.stop.request ?? 0, 0)
            var final: RecoverableSession.Row?
            while final?.hasResult != true {
                guard ProcessInfo.processInfo.systemUptime < deadline else { throw GhostermError.failed("observed result deadline") }
                XCTAssertEqual(catalog.refreshCatalog(), .queued); XCTAssertNil(try catalog.catalog())
                while catalog.step() != .catalog { guard catalog.info?.state != .failed, ProcessInfo.processInfo.systemUptime < deadline else { throw GhostermError.failed("refresh deadline") }; Thread.sleep(forTimeInterval: 0.001) }
                final = try catalog.catalog()?.first { $0.session == identity.session }
            }
            XCTAssertTrue(final?.terminating == true); if final?.hasExitCode == false { XCTAssertEqual(final?.exitCode, 0) }
            print("PASS native session management: renderer failure keeps source, engine-free catalog, UNKNOWN list, explicit stop admission and observed result")
        }
    }
}
