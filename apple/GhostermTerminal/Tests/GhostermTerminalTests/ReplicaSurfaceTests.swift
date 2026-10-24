import AppKit
import Foundation
import XCTest
@testable import GhostermTerminal

private final class ReplicaFixture {
    let queue = DispatchQueue(label: "dev.ghosterm.test.replica.io")
    let directory: URL
    private var initial = Data(), delta = Data()
    init(_ root: String, _ mode: String) { directory = URL(fileURLWithPath: root).appendingPathComponent(mode) }
    func io<T>(_ body: @escaping () throws -> T) async throws -> T {
        try await withCheckedThrowingContinuation { completion in queue.async {
            do { completion.resume(returning: try body()) } catch { completion.resume(throwing: error) }
        } }
    }
    func policy() async throws -> TerminalSurface.ReplicaPolicy {
        try await io {
            self.initial = try Data(contentsOf: self.directory.appendingPathComponent("initial.gtvc"))
            self.delta = try Data(contentsOf: self.directory.appendingPathComponent("delta.bin"))
            return try self.initial.withUnsafeBytes { try TerminalSurface.ReplicaPolicy.decode($0) }
        }
    }
    func restore(_ surface: TerminalSurface, generation: UInt64 = 7, sequence: UInt64 = 9) async throws -> Bool {
        try await io { self.initial.withUnsafeBytes { surface.importReplica(generation: generation, sequence: sequence, bytes: $0) } }
    }
    func corrupt(_ surface: TerminalSurface) async throws -> Bool {
        try await io { var bad = self.initial; bad[0] ^= 1; return bad.withUnsafeBytes { surface.importReplica(generation: 7, sequence: 9, bytes: $0) } }
    }
    func advance(_ surface: TerminalSurface) async throws -> Bool {
        try await io { self.delta.withUnsafeBytes { surface.applyReplicaOutput(generation: 7, sequence: 10, bytes: $0) } }
    }
    func applied(_ surface: TerminalSurface, generation: UInt64 = 7) async throws -> UInt64? {
        try await io { try surface.replicaApplied(generation: generation) }
    }
    func input(_ surface: TerminalSurface, generation: UInt64 = 7, sequence: UInt64 = 10, enabled: Bool) async throws -> Bool {
        try await io { surface.setReplicaInput(generation: generation, sequence: sequence, enabled: enabled) }
    }
}
private final class ReplicaInputs {
    struct Event { let generation, sequence: UInt64; let bytes: Data }
    let lock = NSLock(); private var events: [Event] = []; private var raw = 0
    func append(_ g: UInt64, _ s: UInt64, _ bytes: Data) { lock.lock(); events.append(Event(generation: g, sequence: s, bytes: bytes)); lock.unlock() }
    func legacy() { lock.lock(); raw += 1; lock.unlock() }
    var snapshot: ([Event], Int) { lock.lock(); defer { lock.unlock() }; return (events, raw) }
}
@MainActor final class ReplicaSurfaceTests: XCTestCase {
    private func expectTrue(_ value: Bool, file: StaticString = #filePath, line: UInt = #line) { XCTAssertTrue(value, file: file, line: line) }
    private func expectFalse(_ value: Bool, file: StaticString = #filePath, line: UInt = #line) { XCTAssertFalse(value, file: file, line: line) }
    private func expectEqual<T: Equatable>(_ a: T, _ b: T, file: StaticString = #filePath, line: UInt = #line) { XCTAssertEqual(a, b, file: file, line: line) }
    private func expectNil<T>(_ value: T?, file: StaticString = #filePath, line: UInt = #line) { XCTAssertNil(value, file: file, line: line) }
    private func fixture(_ mode: String = "plain") throws -> ReplicaFixture {
        guard let root = ProcessInfo.processInfo.environment["GHOSTERM_REPLICA_FIXTURES"] else { throw XCTSkip("run scripts/probe-replica-surface.py for real synthetic VT containers") }
        return ReplicaFixture(root, mode)
    }
    private func settle(_ app: TerminalApp) async throws {
        for _ in 0..<10 { app.tick(); try await Task.sleep(nanoseconds: 10_000_000) }
    }
    func testRealImportInputBarrierAndRelease() async throws {
        _ = NSApplication.shared
        let fixture = try fixture(), app = try TerminalApp.shared()
        let policy = try await fixture.policy(); let before = TerminalSurface.replicaBudgetUsage
        var surface: TerminalSurface? = try TerminalSurface(replica: app, generation: 7, policy: policy)
        let inputs = ReplicaInputs()
        surface!.onReplicaInput = { inputs.append($0, $1, $2) }; surface!.onInput = { _ in inputs.legacy() }
        let window = NSWindow(contentRect: surface!.view.frame, styleMask: [.titled], backing: .buffered, defer: false)
        // Stay off every screen: layout/display still run, but nothing pops up.
        window.setFrameOrigin(NSPoint(x: -30_000, y: -30_000))
        window.contentView = surface!.view; window.orderFrontRegardless()
        surface!.sendCommittedText("x"); try await settle(app)
        expectNil(try await fixture.applied(surface!)); expectEqual(inputs.snapshot.0.count, 0)
        expectTrue(try await fixture.restore(surface!)); expectEqual(try await fixture.applied(surface!), 9)
        expectFalse(try await fixture.restore(surface!)); expectTrue(surface!.readText()?.contains("ALPHA α") == true)
        surface!.sendCommittedText("x"); try await settle(app); expectEqual(inputs.snapshot.0.count, 0)
        expectTrue(try await fixture.advance(surface!)); expectEqual(try await fixture.applied(surface!), 10)
        expectTrue(surface!.readText()?.contains("中文") == true)
        expectFalse(try await fixture.input(surface!, generation: 8, enabled: true))
        expectFalse(try await fixture.input(surface!, sequence: 11, enabled: true))
        expectTrue(try await fixture.input(surface!, enabled: true))
        surface!.sendCommittedText("x"); try await settle(app)
        let received = inputs.snapshot
        expectEqual(received.0.count, 1); expectEqual(received.0.first?.generation, 7)
        expectEqual(received.0.first?.sequence, 10); expectEqual(received.0.first?.bytes, Data("x".utf8)); expectEqual(received.1, 0)
        expectTrue(try await fixture.input(surface!, enabled: false)); surface!.sendCommittedText("x"); try await settle(app)
        expectEqual(inputs.snapshot.0.count, 1)
        window.orderOut(nil); window.contentView = nil; surface = nil; try await settle(app)
        let after = TerminalSurface.replicaBudgetUsage
        expectEqual(after.live, before.live); expectEqual(after.committed, before.committed); expectEqual(after.retired, before.retired)
    }
    func testCorruptContainerCannotReusePlaceholder() async throws {
        let fixture = try fixture(), policy = try await fixture.policy(), app = try TerminalApp.shared()
        var surface: TerminalSurface? = try TerminalSurface(replica: app, generation: 7, policy: policy)
        expectFalse(try await fixture.corrupt(surface!)); expectFalse(try await fixture.restore(surface!))
        expectNil(try await fixture.applied(surface!)); surface = nil; try await settle(app)
    }
    func testKnownZeroAndBudgetAdmission() async throws {
        let fixture = try fixture(), policy = try await fixture.policy(), app = try TerminalApp.shared()
        XCTAssertThrowsError(try TerminalSurface(replica: app, generation: 0, policy: policy))
        XCTAssertThrowsError(try TerminalSurface(replica: app, generation: 7, policy: policy.withMemoryLimit(1)))
        var surface: TerminalSurface? = try TerminalSurface(replica: app, generation: 7, policy: policy)
        expectTrue(try await fixture.restore(surface!, sequence: 0)); expectEqual(try await fixture.applied(surface!), 0)
        expectNil(try await fixture.applied(surface!, generation: 8)); surface = nil; try await settle(app)
    }
    func testHalfUTF8AndImageResources() async throws {
        let app = try TerminalApp.shared()
        for mode in ["utf8", "image"] {
            let fixture = try fixture(mode), policy = try await fixture.policy()
            var surface: TerminalSurface? = try TerminalSurface(replica: app, generation: 7, policy: policy)
            expectTrue(try await fixture.restore(surface!)); expectTrue(try await fixture.advance(surface!))
            expectEqual(try await fixture.applied(surface!), 10)
            expectTrue(surface!.readText()?.contains(mode == "utf8" ? "🌍" : "IMAGE α") == true)
            surface = nil; try await settle(app)
        }
    }
    func testOrderedResizeAndColorBarrier() async throws {
        let fixture = try fixture(), policy = try await fixture.policy(), app = try TerminalApp.shared()
        var surface: TerminalSurface? = try TerminalSurface(replica: app, generation: 7, policy: policy)
        expectTrue(try await fixture.restore(surface!))
        let current = surface!
        expectTrue(try await fixture.io { current.applyReplicaResize(generation: 7, sequence: 10, cols: 70, rows: 20) })
        expectEqual(try await fixture.applied(current), 10)
        expectTrue(try await fixture.io { current.beginReplicaScheme(generation: 7, sequence: 11, dark: !policy.dark) })
        expectEqual(try await fixture.applied(current), 10)
        expectFalse(current.applyReplicaScheme(generation: 8, sequence: 11, dark: !policy.dark))
        expectTrue(current.applyReplicaScheme(generation: 7, sequence: 11, dark: !policy.dark))
        expectEqual(try await fixture.applied(current), 11)
        surface = nil; try await settle(app)
    }

}
