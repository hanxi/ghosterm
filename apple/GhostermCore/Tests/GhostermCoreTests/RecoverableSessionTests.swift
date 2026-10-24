import Dispatch
import Foundation
import XCTest
@testable import GhostermCore

final class RecoverableSessionTests: XCTestCase {
    private func sink() -> RecoverableSession.Sink {
        .init(snapshot: { _, _, _, _, _ in .failed(7) }, event: { _, _, _ in .failed(7) },
              applied: { nil }, invalidated: { _, _ in XCTFail("no attachment was created") }, receipt: { _ in XCTFail("no input was admitted") })
    }
    private func options() -> RecoverableSession.Options {
        .init(kind: .local, proxy: "/ghosterm-fixture-no-such-proxy", daemon: "/fixture/daemon", runtime: "/fixture/runtime",
              compatibility: Array(repeating: 1, count: 32), format: 7,
              create: .init(nonce: Array(repeating: 1, count: 16), executable: "/bin/sh", cwd: "/", size: .init(cols: 80, rows: 24)))
    }
    private func onOwner(_ body: @escaping (DispatchQueue) throws -> Void) {
        let done = expectation(description: "session IO owner")
        let owner = DispatchQueue(label: "dev.ghosterm.test.recoverable")
        owner.async { do { try body(owner) } catch { XCTFail("\(error)") }; done.fulfill() }
        wait(for: [done], timeout: 10)
    }
    func testLifecycleAndInputLock() {
        let config = options(), callbacks = sink()
        onOwner { owner in
            let s = try RecoverableSession(options: config, sink: callbacks, owner: owner)
            XCTAssertEqual(s.info?.state, .starting); XCTAssertTrue(s.info?.readonly == true)
            let payload: [UInt8] = [0, 0xff, 1]
            payload.withUnsafeBytes { XCTAssertEqual(s.input(generation: 1, sequence: 0, bytes: $0).0, .locked) }
            XCTAssertEqual(s.resize(generation: 1, sequence: 0, size: .init(cols: 90, rows: 30)).0, .locked)
            XCTAssertEqual(s.color(generation: 1, sequence: 0, dark: true).0, .locked)
            s.cancel(); s.cancel(); XCTAssertEqual(s.step(), .closed)
            XCTAssertEqual(s.info?.error, 8); XCTAssertFalse(s.info?.hasExitCode ?? true)
            XCTAssertTrue(s.close()); XCTAssertTrue(s.close()); XCTAssertNil(s.info)
        }
    }
    func testMissingHelperPreservesFailure() {
        let config = options(), callbacks = sink()
        onOwner { owner in
            let s = try RecoverableSession(options: config, sink: callbacks, owner: owner)
            for _ in 0..<10 { if s.step() == .failed { break } }
            XCTAssertEqual(s.info?.state, .failed); XCTAssertEqual(s.info?.error, 2)
            XCTAssertFalse(s.info?.hasExitCode ?? true); XCTAssertTrue(s.close())
        }
    }
    func testRejectsInvalidMetadata() {
        let config = options(), callbacks = sink()
        onOwner { owner in
            var bad = config; bad.runtime = "/bad\0path"
            XCTAssertThrowsError(try RecoverableSession(options: bad, sink: callbacks, owner: owner))
            bad = config; bad.compatibility.removeLast()
            XCTAssertThrowsError(try RecoverableSession(options: bad, sink: callbacks, owner: owner))
            bad = config; bad.create?.nonce.removeLast()
            XCTAssertThrowsError(try RecoverableSession(options: bad, sink: callbacks, owner: owner))
            bad = config; bad.create?.arguments = Array(repeating: "a", count: 33)
            XCTAssertThrowsError(try RecoverableSession(options: bad, sink: callbacks, owner: owner))
            bad = config; bad.session = 1
            XCTAssertThrowsError(try RecoverableSession(options: bad, sink: callbacks, owner: owner))
        }
    }
    func testAsyncAuthenticationAdmissionAndReentry() {
        var config = options(); config.kind = .ssh
        let callbacks = sink()
        onOwner { owner in
            var session: RecoverableSession?
            var request: UInt64 = 0, requests = 0, cancellations = 0
            let auth = RecoverableSession.Authentication(requested: { id, attempt in
                request = id; requests += 1; XCTAssertEqual(attempt, 1)
                XCTAssertFalse(session!.close(), "callback must not free active core")
            }, cancelled: { id in XCTAssertEqual(id, request); cancellations += 1 },
               hostkey: { _, _, _ in XCTFail("no address was supplied") })
            session = try RecoverableSession(options: config, sink: callbacks, authentication: auth, owner: owner)
            XCTAssertEqual(session!.step(), .starting); XCTAssertGreaterThan(request, 0)
            XCTAssertEqual(session!.decide(request: request, decision: .accept), 1)
            XCTAssertThrowsError(try session!.supply(request: request + 1, options: .init(host: "localhost", username: "fixture"),
                                                     address: .init(family: 4, bytes: [127,0,0,1] + Array(repeating: 0, count: 12)), credential: .password("fixture-not-a-secret")))
            XCTAssertEqual(session!.reject(request: request, error: 4), 0)
            XCTAssertEqual(session!.step(), .failed); XCTAssertEqual(session!.info?.error, 4)
            XCTAssertEqual(requests, 1); XCTAssertEqual(cancellations, 1)
            XCTAssertTrue(session!.close()); session = nil
        }
    }
    func testDeinitSchedulesContextReleaseOnOwner() {
        let config = options()
        let released = expectation(description: "callback context released after close")
        final class Lifetime { let callback: () -> Void; init(_ callback: @escaping () -> Void) { self.callback = callback }; deinit { callback() } }
        onOwner { owner in
            let token = Lifetime { released.fulfill() }
            let callbacks = RecoverableSession.Sink(snapshot: { [token] _, _, _, _, _ in _ = token; return .busy },
                event: { _, _, _ in .busy }, applied: { nil }, invalidated: { _, _ in }, receipt: { _ in })
            var session: RecoverableSession? = try RecoverableSession(options: config, sink: callbacks, owner: owner)
            XCTAssertEqual(session!.step(), .failed) // missing proxy cannot start a daemon
            session = nil
        }
        wait(for: [released], timeout: 5)
    }
}
