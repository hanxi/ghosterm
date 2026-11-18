import AppKit
import Foundation
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

@MainActor final class PluginTaskFixture {
    var description: [String: Any] = ["viewId": "task-a", "revision": "18446744073709551615", "title": "Prime task", "unit": "candidates", "cancellable": true]
    var state: [String: Any] = ["viewId": "task-a", "revision": "18446744073709551615", "sequence": "1", "phase": "running", "completed": "4",
        "total": ["kind": "known", "units": "100"], "message": "Counting primes", "result": ["kind": "none"]]
    var calls: [(String, Data)] = []
    var unknownMethod: String?
    var failureMethod: String?
    var cancelledRPC = false
    var badAck = false
    var pauseMethod: String?
    var barrier: CheckedContinuation<Data, Never>?
    private var paused: Data?
    func outcome(_ value: [String: Any]) throws -> Data { try JSONSerialization.data(withJSONObject: ["kind": "success", "value": value], options: [.sortedKeys]) }
    func request(_ method: String, _ input: Data) async throws -> Data {
        calls.append((method, input))
        let value: [String: Any]
        switch method {
        case "view.task.describe": value = description
        case "view.task.read": value = state
        case "view.task.cancel":
            let request = try JSONSerialization.jsonObject(with: input) as! [String: Any]
            value = ["viewId": description["viewId"]!, "revision": description["revision"]!, "requestId": badAck ? "other" : request["requestId"]!, "kind": "requested"]
        default: XCTFail("undeclared task method"); value = [:]
        }
        let data: Data
        if unknownMethod == method { data = Data(#"{"kind":"unknown","reason":"UNCONFIRMED"}"#.utf8) }
        else if failureMethod == method {
            data = cancelledRPC ? Data(#"{"kind":"cancelled","effect":"possible"}"#.utf8) :
                Data(#"{"kind":"failed","error":{"code":"DENIED","message":"Explicit refusal","effect":"none"}}"#.utf8)
        } else { data = try outcome(value) }
        if pauseMethod == method { pauseMethod = nil; paused = data; return await withCheckedContinuation { barrier = $0 } }
        return data
    }
    func release() { barrier?.resume(returning: paused!); barrier = nil; paused = nil }
    func model(isCurrent: @escaping @MainActor () -> Bool = { true }) -> PluginTaskModel {
        .init(rpc: { try await self.request($0, $1) }, isCurrent: isCurrent)
    }
    func descriptor() throws -> PluginTaskDescriptor { try PluginTaskCodec.descriptor(outcome(description)) }
    func snapshot() throws -> PluginTaskSnapshot { try PluginTaskCodec.snapshot(outcome(state), descriptor: descriptor()) }
    func finish(_ phase: String = "cancelled") {
        state["sequence"] = "2"; state["phase"] = phase
        state["result"] = phase == "cancelled" ? ["kind": "cancelled", "effect": "none"] : ["kind": "success", "summary": "25 primes"]
        if phase == "succeeded" { state["completed"] = "100" }
    }
}

@MainActor final class PluginTaskTests: XCTestCase {
    private func until(_ predicate: () -> Bool) async throws {
        let end = ProcessInfo.processInfo.systemUptime + 5
        while !predicate() && ProcessInfo.processInfo.systemUptime < end { try await Task.sleep(for: .milliseconds(5)) }
        XCTAssertTrue(predicate(), "Expected task fixture barrier")
    }
    func testDescriptorExactIdentityBooleanAndUnicode() throws {
        let f = PluginTaskFixture(); XCTAssertEqual(try f.descriptor().revision, "18446744073709551615")
        f.description["title"] = String(repeating: "😀", count: 128)
        XCTAssertEqual(try f.descriptor().title.unicodeScalars.count, 128)
        for (key, value) in [("viewId", "bad\n"), ("cancellable", 1), ("unit", ""), ("extra", 0), ("title", String(repeating: "e\u{301}", count: 65))] as [(String, Any)] {
            let old = f.description; f.description[key] = value; XCTAssertThrowsError(try f.descriptor()); f.description = old
        }
    }
    func testZeroRevisionRejected() throws {
        let f = PluginTaskFixture(); f.description["revision"] = "0"
        XCTAssertThrowsError(try f.descriptor())
    }
    func testSnapshotViewScope() throws {
        let f = PluginTaskFixture(); f.state["viewId"] = "other"
        XCTAssertThrowsError(try f.snapshot())
    }
    func testSnapshotRevisionScope() throws {
        let f = PluginTaskFixture(); f.state["revision"] = "1"
        XCTAssertThrowsError(try f.snapshot())
    }
    func testExactCountersAndKnownZeroVersusUnknown() throws {
        let f = PluginTaskFixture(); f.state["completed"] = "18446744073709551615"; f.state["total"] = ["kind": "unknown"]
        XCTAssertEqual(try f.snapshot().completed, "18446744073709551615"); XCTAssertNil(try f.snapshot().total)
        for bad: Any in [0, "00", "-1", "1.0", "18446744073709551616"] {
            f.state["completed"] = bad; XCTAssertThrowsError(try f.snapshot())
        }
        f.state["completed"] = "0"; f.state["total"] = ["kind": "known", "units": "0"]
        XCTAssertEqual(try f.snapshot().total, "0")
        f.state["total"] = ["kind": "unknown", "units": "0"]; XCTAssertThrowsError(try f.snapshot())
    }
    func testResultPhaseAndCompleteProgress() throws {
        let f = PluginTaskFixture(); f.state["phase"] = "succeeded"; f.state["result"] = ["kind": "success", "summary": "done"]
        XCTAssertThrowsError(try f.snapshot())
        f.state["completed"] = "100"; XCTAssertEqual(try f.snapshot().phase, .succeeded)
        f.state["phase"] = "running"; XCTAssertThrowsError(try f.snapshot())
        f.description["cancellable"] = false; f.finish(); XCTAssertThrowsError(try f.snapshot())
    }
    func testSequenceCannotGoBack() throws {
        let f = PluginTaskFixture(); f.state["sequence"] = "2"; let old = try f.snapshot(); f.state["sequence"] = "1"
        XCTAssertThrowsError(try PluginTaskCodec.snapshot(f.outcome(f.state), descriptor: f.descriptor(), previous: old))
    }
    func testSameSequenceCannotChange() throws {
        let f = PluginTaskFixture(), old = try f.snapshot(); f.state["message"] = "changed"
        XCTAssertThrowsError(try PluginTaskCodec.snapshot(f.outcome(f.state), descriptor: f.descriptor(), previous: old))
    }
    func testProgressCannotGoBack() throws {
        let f = PluginTaskFixture(), old = try f.snapshot(); f.state["sequence"] = "2"; f.state["completed"] = "3"
        XCTAssertThrowsError(try PluginTaskCodec.snapshot(f.outcome(f.state), descriptor: f.descriptor(), previous: old))
    }
    func testKnownTotalCannotChange() throws {
        let f = PluginTaskFixture(), old = try f.snapshot(); f.state["sequence"] = "2"; f.state["total"] = ["kind": "unknown"]
        XCTAssertThrowsError(try PluginTaskCodec.snapshot(f.outcome(f.state), descriptor: f.descriptor(), previous: old))
    }
    func testTerminalCannotBeRewritten() throws {
        let f = PluginTaskFixture(); f.finish(); let old = try f.snapshot(); f.state["sequence"] = "3"; f.state["phase"] = "running"; f.state["result"] = ["kind": "none"]
        XCTAssertThrowsError(try PluginTaskCodec.snapshot(f.outcome(f.state), descriptor: f.descriptor(), previous: old))
    }
    func testStrictRawJSONAndAggregateBytes() throws {
        let f = PluginTaskFixture(), normal = String(decoding: try f.outcome(f.description), as: UTF8.self)
        for bad in [normal + "{}", normal.replacingOccurrences(of: #""unit":"candidates""#, with: #""unit":"candidates","unit":"candidates""#), normal.replacingOccurrences(of: "Prime task", with: #"\ud800"#)] {
            XCTAssertThrowsError(try PluginTaskCodec.descriptor(Data(bad.utf8)))
        }
        f.state["viewId"] = String(repeating: "a", count: 64); f.description["viewId"] = f.state["viewId"]
        f.state["message"] = String(repeating: "😀", count: 128); f.state["phase"] = "failed"
        f.state["result"] = ["kind": "failed", "code": String(repeating: "😀", count: 64), "message": String(repeating: "😀", count: 256)]
        XCTAssertThrowsError(try f.snapshot())
    }
    func testCancelAckIdentityDoesNotAllowReplay() async {
        let f = PluginTaskFixture(), m = f.model(); await m.load(); f.badAck = true; await m.cancel()
        XCTAssertTrue(m.cancellation == .unconfirmed && !m.canCancel && !m.canAutoRefresh && m.cancelRequestID != nil)
    }
    func testUnknownCancellationOnlyAllowsExplicitReads() async {
        let f = PluginTaskFixture(), m = f.model(); await m.load(); f.unknownMethod = "view.task.cancel"; await m.cancel()
        let id = m.cancelRequestID; let calls = f.calls.count
        await m.cancel()
        XCTAssertTrue(m.cancellation == .unconfirmed && !m.canAutoRefresh && f.calls.count == calls)
        f.finish(); await m.load(); await m.cancel()
        XCTAssertTrue(m.snapshot?.phase == .cancelled && m.cancellation == .unconfirmed && m.cancelRequestID == id && f.calls.filter { $0.0 == "view.task.cancel" }.count == 1)
    }
    func testAcknowledgementIsNotProofOfStop() async {
        let f = PluginTaskFixture(), m = f.model(); await m.load(); await m.cancel()
        XCTAssertTrue(m.cancellation == .requested && m.snapshot?.phase == .running && !m.canCancel && m.canAutoRefresh)
        f.finish(); await m.load(); XCTAssertEqual(m.snapshot?.phase, .cancelled)
    }
    func testCancelNeverRepeatsAfterUnknown() async {
        let f = PluginTaskFixture(), m = f.model(); await m.load(); f.unknownMethod = "view.task.cancel"
        await m.cancel(); await m.cancel()
        XCTAssertEqual(f.calls.filter { $0.0 == "view.task.cancel" }.count, 1)
    }
    func testDeclaredFailureStaysDistinctFromUnknown() async {
        let f = PluginTaskFixture(), m = f.model(); await m.load(); let old = m.snapshot
        f.failureMethod = "view.task.cancel"; await m.cancel(); await m.cancel()
        XCTAssertTrue(m.cancellation == .failed && m.issue?.contains("DENIED") == true && m.snapshot == old && !m.canAutoRefresh && f.calls.filter { $0.0 == "view.task.cancel" }.count == 1)
        f.failureMethod = "view.task.read"; await m.load()
        XCTAssertTrue(m.issue?.contains("DENIED") == true && m.issue?.contains("结果不明") == false && m.snapshot == old)
    }
    func testCancelledRequestDoesNotMeanCancelledTask() async {
        let f = PluginTaskFixture(), m = f.model(); await m.load(); f.failureMethod = "view.task.cancel"; f.cancelledRPC = true; await m.cancel()
        XCTAssertTrue(m.cancellation == .cancelled && m.snapshot?.phase == .running && m.issue?.contains("可能已有执行效果") == true && !m.canCancel)
    }
    func testWireFailureUnicodeAndControlsKeepDeclaredOutcome() throws {
        for message in [String(repeating: "😀", count: 1024), "bad\0message"] {
            let data = try JSONSerialization.data(withJSONObject: ["kind": "failed", "error": ["code": "DENIED", "message": message, "effect": "possible"]])
            var preserved = false
            do { _ = try PluginTaskCodec.success(data) }
            catch let error as PluginTaskCodec.RPCFailure {
                if case .failed(let code, let visible, let effect) = error {
                    preserved = code == "DENIED" && effect == "possible" && !visible.utf8.contains(0) &&
                        (message.utf8.contains(0) ? visible == "bad\\u0000message" : visible == message)
                }
            }
            XCTAssertTrue(preserved)
        }
    }
    func testUnknownReadPreservesLastConfirmedSnapshot() async {
        let f = PluginTaskFixture(), m = f.model(); await m.load(); let old = m.snapshot
        f.unknownMethod = "view.task.read"; await m.load()
        XCTAssertTrue(m.snapshot == old && m.issue != nil && !m.canAutoRefresh)
        f.unknownMethod = nil; f.finish("succeeded"); await m.load()
        XCTAssertTrue(m.snapshot?.phase == .succeeded && !m.canCancel && !m.canAutoRefresh)
    }
    func testLateReplyCannotChangeInvalidatedTask() async throws {
        let f = PluginTaskFixture(), m = f.model(); await m.load(); let old = m.snapshot; f.finish(); f.pauseMethod = "view.task.read"
        let read = Task { await m.load() }; try await until { f.barrier != nil }; m.invalidate(); f.release(); await read.value
        XCTAssertTrue(!m.valid && !m.busy && m.snapshot == old && m.issue != nil)
    }
    func testCancelledCallerKeepsActionUnconfirmed() async throws {
        let f = PluginTaskFixture(), m = f.model(); await m.load(); f.pauseMethod = "view.task.cancel"
        let action = Task { await m.cancel() }; try await until { f.barrier != nil }; action.cancel(); f.release(); await action.value
        XCTAssertTrue(m.cancellation == .unconfirmed && m.issue != nil && !m.canCancel && !m.canAutoRefresh)
    }
    func testCancellationSupersedesInFlightStatusRead() async throws {
        let f = PluginTaskFixture(), m = f.model()
        await m.load()
        f.pauseMethod = "view.task.read"
        let read = Task { await m.load() }
        defer { f.release() }
        try await until { f.barrier != nil }
        XCTAssertTrue(m.canCancel)
        f.finish()
        await m.cancel()
        XCTAssertEqual(m.snapshot?.phase, .cancelled)
        XCTAssertEqual(f.calls.filter { $0.0 == "view.task.cancel" }.count, 1)
        f.release()
        await read.value
        XCTAssertEqual(m.snapshot?.phase, .cancelled)
        XCTAssertFalse(m.busy)
    }
    func testCurrentInstanceAndBusyPreventNewWork() async throws {
        let f = PluginTaskFixture(); var current = true; let m = f.model { current }; f.pauseMethod = "view.task.describe"
        let read = Task { await m.load() }; try await until { f.barrier != nil }; await m.load(); await m.cancel(); current = false; f.release(); await read.value
        XCTAssertTrue(m.descriptor == nil && m.snapshot == nil && f.calls.count == 1 && !m.available)
    }
    func testBootstrapAllowsOnlyUniqueImplementedTaskService() throws {
        let hash = String(repeating: "a", count: 64)
        func request(_ caps: [String]) -> ExtensionRelayBootstrap.Request {
            .init(executable: "/gterm", executorDirectory: "/package", executorDigest: hash, clientDigest: hash,
                methods: PluginTaskModel.methods, clientCapabilities: caps, command: "task primes")
        }
        XCTAssertTrue(try ExtensionRelayBootstrap.build(request(["ui.task"])).contains("'--client-capability' 'ui.task'"))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(["ui.task", "ui.task"])))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(["ui.logs"])))
    }
    func testVisibleViewRefreshesThenStopsOnUncertainty() async throws {
        TestWindows.enterAutomationMode()
        let f = PluginTaskFixture(), m = f.model(); await m.load()
        let host = NSHostingView(rootView: PluginTaskView(model: m))
        host.frame = NSRect(x: 0, y: 0, width: 760, height: 540)
        let window = PluginTableWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = host; TestWindows.presentOffscreen(window)
        defer { window.close() }
        f.state["completed"] = "5"; f.state["sequence"] = "2"
        try await until { m.snapshot?.completed == "5" }
        guard m.snapshot?.completed == "5" else { return }
        f.unknownMethod = "view.task.read"
        try await until { m.issue != nil }
        guard m.issue != nil else { return }
        let count = f.calls.count
        try await Task.sleep(for: .milliseconds(650))
        XCTAssertTrue(f.calls.count == count && !m.canAutoRefresh && m.snapshot?.completed == "5")
        f.unknownMethod = nil; f.state["completed"] = "6"; f.state["sequence"] = "3"
        await m.load()
        f.state["completed"] = "7"; f.state["sequence"] = "4"
        try await until { m.snapshot?.completed == "7" }
    }
    func testNativeThemeLargeTextAndActualCancelButton() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApp.accessibilitySetValue(true, forAttribute: enhanced); defer { NSApp.accessibilitySetValue(false, forAttribute: enhanced) }
        for appearance in [NSAppearance.Name.aqua, .darkAqua] { for points: CGFloat in [13, 28] {
            let f = PluginTaskFixture(), m = f.model(); await m.load()
            let host = NSHostingView(rootView: PluginTaskView(model: m).padding(16).environment(\.font, .system(size: points)).background(Color(nsColor: .windowBackgroundColor)))
            host.frame = NSRect(x: 0, y: 0, width: 760, height: 540)
            let window = PluginTableWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false; window.contentView = host; window.appearance = NSAppearance(named: appearance); TestWindows.presentOffscreen(window)
            defer { window.close() }; try await Task.sleep(for: .milliseconds(100)); host.layoutSubtreeIfNeeded()
            XCTAssertTrue(window.alphaValue == 0 && NSScreen.screens.allSatisfy { !$0.frame.intersects(window.frame) })
            let words = try PluginTableRendering.words(host).joined(separator: " ")
            XCTAssertTrue(words.contains("Counting primes") && words.contains("100"), words)
            let nodes = PluginTableRendering.elements(host)
            let count = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-task-count" })
            XCTAssertEqual(PluginTableRendering.attribute(count, "accessibilityValue"), "4 / 100 · candidates")
            let cancel = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-task-cancel" })
            XCTAssertTrue(PluginTableRendering.press(cancel)); try await until { m.cancellation == .requested }
            XCTAssertEqual(f.calls.filter { $0.0 == "view.task.cancel" }.count, 1)
            let view = PluginTaskView(model: m)
            XCTAssertEqual(view.statusAnnouncement, "取消请求已受理，仍需任务确认停止")
            f.finish()
            await m.load()
            let title = try XCTUnwrap(m.descriptor?.title)
            XCTAssertEqual(view.statusAnnouncement, "\(title)，已确认取消")
        } }
    }
}
