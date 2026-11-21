import AppKit
import Foundation
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

@MainActor final class PluginViewsFixture {
    let instance: String
    let nonce = String(repeating: "a", count: 32)
    let alpha = PluginTableFixture(), beta = PluginTableFixture(), diff = PluginDiffFixture(), task = PluginTaskFixture()
    var revision: UInt64 = 1
    var entries: [[String: Any]] = []
    var closes: [String] = []
    var closeFails = false
    var pauseClose = false
    var closeBarrier: CheckedContinuation<Void, Never>?
    func releaseClose() {
        pauseClose = false
        closeBarrier?.resume()
        closeBarrier = nil
    }
    var current = true
    init(instance: String = String(repeating: "b", count: 32)) {
        self.instance = instance
        beta.description["viewId"] = "table-b"
        entries = [entry(1, "table-a", "table", "alpha"), entry(2, "table-b", "table", "beta"),
                   entry(3, "diff-a", "diff", "changes"), entry(4, "task-a", "task", "work")]
    }
    func entry(_ number: Int, _ id: String, _ kind: String, _ prefix: String) -> [String: Any] {
        var methods = ["describe": prefix + ".describe", "read": prefix + ".read"]
        if kind == "task" { methods["cancel"] = prefix + ".cancel" }
        return ["handle": nonce + "." + String(number), "viewId": id, "kind": kind, "methods": methods]
    }
    func data(epoch: String = "9007199254740993", instance: String? = nil, revision: String? = nil) throws -> Data {
        try JSONSerialization.data(withJSONObject: ["instanceId": instance ?? self.instance, "epoch": epoch,
            "revision": revision ?? String(self.revision), "views": entries], options: [.sortedKeys])
    }
    var methods: Set<String> { Set(entries.flatMap { ($0["methods"] as! [String: String]).values }) }
    func model() throws -> PluginViewsModel {
        try .init(instance: instance, epoch: "9007199254740993", kinds: [.table, .diff, .task],
            capabilities: ["session.rpc", "ui.views", "ui.table", "ui.diff", "ui.task"], methods: methods,
            rpc: { method, input in
                let prefix = method.split(separator: ".")[0], suffix = method.split(separator: ".")[1]
                if prefix == "alpha" { return try await self.alpha.request(suffix == "describe" ? "view.table.describe" : "view.table.page", input) }
                if prefix == "beta" { return try await self.beta.request(suffix == "describe" ? "view.table.describe" : "view.table.page", input) }
                if prefix == "changes" { return try await self.diff.request(suffix == "describe" ? "view.diff.describe" : "view.diff.page", input) }
                return try await self.task.request("view.task." + suffix, input)
            }, isCurrent: { self.current }, close: { handle in
                self.closes.append(handle)
                if self.pauseClose {
                    await withCheckedContinuation { self.closeBarrier = $0 }
                }
                if self.closeFails { throw PluginViewValue.Failure.unconfirmedResult }
                self.entries.removeAll { $0["handle"] as? String == handle }; self.revision += 1
                return try self.data()
            })
    }
    func populated() throws -> PluginViewsModel { let model = try model(); try model.apply(data()); return model }
}

@MainActor final class PluginViewsTests: XCTestCase {
    func testFourIndependentModelsAndStableIdentity() async throws {
        let f = PluginViewsFixture(), m = try f.populated(), original = m.items
        try m.apply(f.data()); XCTAssertTrue(zip(original, m.items).allSatisfy { $0 === $1 })
        guard case .table(let alpha) = m.items[0].content, case .table(let beta) = m.items[1].content,
              case .diff(let diff) = m.items[2].content, case .task(let task) = m.items[3].content else { return XCTFail("component types") }
        await alpha.load(); await beta.load(); await diff.load(); await task.load()
        XCTAssertEqual(alpha.descriptor?.viewID, "table-a"); XCTAssertEqual(beta.descriptor?.viewID, "table-b")
        XCTAssertNotNil(diff.page); XCTAssertNotNil(task.snapshot)
        await alpha.showPage(offset: 16)
        XCTAssertEqual(alpha.page?.offset, 16); XCTAssertEqual(beta.page?.offset, 0)
    }
    func testWrongInstanceClearsViews() throws {
        let f = PluginViewsFixture(), m = try f.populated()
        f.revision += 1
        var rejected = false
        do { try m.apply(f.data(instance: String(repeating: "c", count: 32))) } catch { rejected = true }
        XCTAssertTrue(rejected && !m.valid && m.items.isEmpty)
    }
    func testWrongEpochClearsViews() throws {
        let f = PluginViewsFixture(), m = try f.populated()
        f.revision += 1
        var rejected = false
        do { try m.apply(f.data(epoch: "9007199254740992")) } catch { rejected = true }
        XCTAssertTrue(rejected && m.items.isEmpty)
    }
    func testSameRevisionCannotChangeBindings() throws {
        let f = PluginViewsFixture(), m = try f.populated(); f.entries.removeLast()
        var rejected = false
        do { try m.apply(f.data()) } catch { rejected = true }
        XCTAssertTrue(rejected && !m.valid)
    }
    func testOlderSnapshotCannotResurrectClosedView() async throws {
        let f = PluginViewsFixture(), m = try f.populated(), old = try f.data(), handle = m.items[0].id
        await m.closeView(handle: handle); try m.apply(old)
        XCTAssertEqual(m.items.count, 3); XCTAssertFalse(m.items.contains { $0.id == handle })
    }
    func testRemovedHandleCannotReturnAtNewRevision() throws {
        let f = PluginViewsFixture(), m = try f.populated(), old = f.entries[0]
        f.entries.removeFirst(); f.revision += 1; try m.apply(f.data())
        f.entries.append(old); f.revision += 1
        var rejected = false
        do { try m.apply(f.data()) } catch { rejected = true }
        XCTAssertTrue(rejected && !m.valid)
    }
    func testChangedMetadataCannotReuseHandle() throws {
        let f = PluginViewsFixture(), m = try f.populated()
        f.entries[0]["viewId"] = "different"; f.revision += 1
        XCTAssertThrowsError(try m.apply(f.data()))
    }
    func testReopenSameViewIDUsesFreshModel() async throws {
        let f = PluginViewsFixture(), m = try f.populated(), old = m.items[0]
        await m.closeView(handle: old.id)
        f.entries.append(f.entry(5, "table-a", "table", "alpha")); f.revision += 1; try m.apply(f.data())
        XCTAssertEqual(m.items.count, 4); XCTAssertFalse(m.items.last === old)
        if case .table(let model) = old.content { XCTAssertFalse(model.valid) }
    }
    func testUnknownMethodBindingRejected() throws {
        let f = PluginViewsFixture(), m = try f.populated()
        var entry = f.entry(5, "other", "table", "other")
        entry["methods"] = ["describe": "other.describe", "read": "other.read"]
        f.entries.append(entry); f.revision += 1
        XCTAssertThrowsError(try m.apply(f.data()))
    }
    func testUndeclaredKindRejected() throws {
        let f = PluginViewsFixture()
        let m = try PluginViewsModel(instance: f.instance, epoch: "9007199254740993", kinds: [.table],
            capabilities: ["session.rpc", "ui.views", "ui.table"], methods: f.methods, rpc: { _, _ in Data() }, isCurrent: { true }, close: { _ in Data() })
        XCTAssertThrowsError(try m.apply(f.data()))
    }
    func testRegistrationBoundsDuplicateAndCanonicalNumbers() throws {
        let f = PluginViewsFixture()
        for revision in ["0", "01", "18446744073709551616", "1\n"] { XCTAssertThrowsError(try PluginViewRegistrySnapshot.parse(f.data(revision: revision))) }
        f.entries.append(f.entries[0]); XCTAssertThrowsError(try PluginViewRegistrySnapshot.parse(f.data()))
        f.entries = (1...9).map { f.entry($0, "v\($0)", "table", "alpha") }
        XCTAssertThrowsError(try PluginViewRegistrySnapshot.parse(f.data()))
        XCTAssertThrowsError(try PluginViewRegistrySnapshot.parse(Data(repeating: 32, count: 8193)))
    }
    func testRawDuplicateKeysAreRejected() throws {
        let f = PluginViewsFixture(), valid = String(decoding: try f.data(), as: UTF8.self)
        let duplicate = "{\"revision\":\"1\"," + valid.dropFirst()
        XCTAssertThrowsError(try PluginViewRegistrySnapshot.parse(Data(duplicate.utf8)))
    }
    func testDescriptorMustMatchRegisteredViewID() async throws {
        let f = PluginViewsFixture(), m = try f.populated()
        f.alpha.description["viewId"] = "other"; f.diff.description["viewId"] = "other"; f.task.description["viewId"] = "other"
        var rejected = true
        if case .table(let model) = m.items[0].content { await model.load(); rejected = rejected && model.descriptor == nil && model.issue != nil }
        if case .diff(let model) = m.items[2].content { await model.load(); rejected = rejected && model.descriptor == nil && model.issue != nil }
        if case .task(let model) = m.items[3].content { await model.load(); rejected = rejected && model.descriptor == nil && model.issue != nil }
        XCTAssertTrue(rejected)
    }
    func testNativeCloseDoesNotCallTaskCancel() async throws {
        let f = PluginViewsFixture(), m = try f.populated()
        await m.closeView(handle: m.items[3].id)
        XCTAssertEqual(f.closes.count, 1); XCTAssertTrue(f.task.calls.isEmpty); XCTAssertEqual(m.items.count, 3)
    }
    func testFailedCloseKeepsViewAndAllowsExplicitRetry() async throws {
        let f = PluginViewsFixture(), m = try f.populated(), item = m.items[0]; f.closeFails = true
        await m.closeView(handle: item.id)
        XCTAssertEqual(m.items.count, 4); XCTAssertFalse(item.closing); XCTAssertNotNil(item.closeIssue)
        f.closeFails = false; await m.closeView(handle: item.id)
        XCTAssertEqual(m.items.count, 3); XCTAssertEqual(f.closes.count, 2)
    }
    func testOverlappingHandlesBelongToTheirOwnInstances() async throws {
        let first = PluginViewsFixture(), second = PluginViewsFixture(instance: String(repeating: "c", count: 32))
        let firstModel = try first.populated(), secondModel = try second.populated()
        let firstItem = firstModel.items[0], secondItem = secondModel.items[0]
        XCTAssertEqual(firstItem.id, secondItem.id)
        guard case .table(let firstTable) = firstItem.content, case .table(let secondTable) = secondItem.content else {
            return XCTFail("both instances must expose a table")
        }
        await firstTable.load()
        await secondTable.load()
        await firstModel.closeView(handle: firstItem.id)
        XCTAssertFalse(firstTable.valid)
        XCTAssertTrue(secondTable.valid)
        XCTAssertTrue(secondModel.items[0] === secondItem)
        XCTAssertEqual(first.closes, [firstItem.id])
        XCTAssertTrue(second.closes.isEmpty)
        await secondTable.showPage(offset: 16)
        XCTAssertEqual(secondTable.page?.rows.first?.id, "17")
        XCTAssertNil(firstTable.page)
    }

    func testLateCloseAfterRuntimeInvalidationDoesNotAffectAnotherInstance() async throws {
        let first = PluginViewsFixture(), second = PluginViewsFixture(instance: String(repeating: "c", count: 32))
        let firstModel = try first.populated(), secondModel = try second.populated()
        let firstItem = firstModel.items[0], secondItem = secondModel.items[0]
        XCTAssertEqual(firstItem.id, secondItem.id)
        guard case .table(let activeTable) = secondItem.content else { return XCTFail("expected table") }
        await activeTable.load()

        first.pauseClose = true
        defer { first.releaseClose() }
        let pending = Task { await firstModel.closeView(handle: firstItem.id) }
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while first.closeBarrier == nil && ProcessInfo.processInfo.systemUptime < deadline {
            try await Task.sleep(for: .milliseconds(5))
        }
        guard first.closeBarrier != nil else { return XCTFail("expected pending close receipt") }

        firstModel.invalidate()
        await activeTable.showPage(offset: 16)
        first.releaseClose()
        await pending.value

        XCTAssertFalse(firstModel.available)
        XCTAssertTrue(firstModel.items.isEmpty)
        XCTAssertNil(firstModel.issue)
        XCTAssertEqual(first.closes, [firstItem.id])
        XCTAssertTrue(second.closes.isEmpty)
        XCTAssertTrue(secondModel.items.first === secondItem)
        XCTAssertEqual(activeTable.page?.rows.first?.id, "17")
    }

    func testRuntimeInvalidationStopsLatePage() async throws {
        let f = PluginViewsFixture(), m = try f.populated()
        guard case .table(let table) = m.items[0].content else { return XCTFail("table") }
        f.alpha.pauseMethod = "view.table.page"
        let pending = Task { await table.load() }
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while f.alpha.barrier == nil && ProcessInfo.processInfo.systemUptime < deadline { try await Task.sleep(for: .milliseconds(5)) }
        guard f.alpha.barrier != nil else { pending.cancel(); return XCTFail("expected explicit page barrier") }
        m.invalidate(); f.alpha.release(); await pending.value
        XCTAssertNil(table.page); XCTAssertFalse(table.valid); XCTAssertTrue(m.items.isEmpty)
    }

    func testNativeCloseControlsRouteOverlappingHandlesToTheirOwnInstance() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let first = PluginViewsFixture(), second = PluginViewsFixture(instance: String(repeating: "c", count: 32))
        let firstModel = try first.populated(), secondModel = try second.populated()
        let overlap = firstModel.items[0].id
        XCTAssertEqual(secondModel.items[0].id, overlap)
        XCTAssertEqual(PluginViewsView.adjacentCloseTarget(in: secondModel.items, from: overlap), secondModel.items[1].id)
        XCTAssertEqual(PluginViewsView.adjacentCloseTarget(in: secondModel.items, from: secondModel.items[3].id), secondModel.items[2].id)
        XCTAssertNil(PluginViewsView.adjacentCloseTarget(in: [secondModel.items[0]], from: overlap))
        let firstHost = NSHostingView(rootView: PluginViewsView(model: firstModel))
        let secondHost = NSHostingView(rootView: PluginViewsView(model: secondModel))
        let firstProbe = ConversationUIProbe(root: firstHost), secondProbe = ConversationUIProbe(root: secondHost)
        defer { firstProbe.close(); secondProbe.close() }
        try await Task.sleep(for: .milliseconds(100))
        secondHost.layoutSubtreeIfNeeded()
        let close = try XCTUnwrap(PluginTableRendering.elements(secondHost).first {
            PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-view-close" &&
                PluginTableRendering.attribute($0, "accessibilityLabel") == "关闭视图 table-a"
        })
        XCTAssertTrue(PluginTableRendering.press(close))
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while secondModel.items.count == 4 && ProcessInfo.processInfo.systemUptime < deadline {
            try await Task.sleep(for: .milliseconds(10))
        }
        XCTAssertEqual(second.closes, [overlap])
        XCTAssertFalse(secondModel.items.contains { $0.id == overlap })
        XCTAssertTrue(first.closes.isEmpty)
        XCTAssertEqual(firstModel.items.map(\.id).first, overlap)
    }
    func testTaskReopenKeepsUnknownCancellationIdentity() async throws {
        let f = PluginViewsFixture(), m = try f.populated()
        guard case .task(let old) = m.items[3].content else { return XCTFail("task") }
        await old.load(); f.task.unknownMethod = "view.task.cancel"; await old.cancel()
        let id = old.cancelRequestID
        await m.closeView(handle: m.items[3].id)
        f.entries.append(f.entry(5, "task-a", "task", "work")); f.revision += 1; try m.apply(f.data())
        guard case .task(let new) = m.items.last?.content else { return XCTFail("reopened task") }
        await new.load(); await new.cancel()
        XCTAssertTrue(new.cancelRequestID == id && new.cancellation == .unconfirmed && f.task.calls.filter { $0.0 == "view.task.cancel" }.count == 1)
    }
    func testTaskReopenCannotRewriteConfirmedTerminal() async throws {
        let f = PluginViewsFixture(), m = try f.populated(); f.task.finish("succeeded")
        guard case .task(let old) = m.items[3].content else { return XCTFail("task") }
        await old.load(); await m.closeView(handle: m.items[3].id)
        f.entries.append(f.entry(5, "task-a", "task", "work")); f.revision += 1; try m.apply(f.data())
        f.task.state["sequence"] = "3"; f.task.state["phase"] = "running"; f.task.state["result"] = ["kind": "none"]
        guard case .task(let new) = m.items.last?.content else { return XCTFail("task") }
        await new.load(); XCTAssertEqual(new.snapshot?.phase, .succeeded); XCTAssertNotNil(new.issue)
    }
    func testTaskHistoryBudgetIsFiniteAndDoesNotEvict() async throws {
        let history = PluginTaskHistory(), f = PluginTaskFixture()
        for i in 0..<9 {
            f.description["viewId"] = "task-\(i)"; f.state["viewId"] = "task-\(i)"
            let model = PluginTaskModel(rpc: { try await f.request($0, $1) }, isCurrent: { true }, history: history)
            await model.load()
            if i == 8 { XCTAssertNil(model.descriptor); XCTAssertNotNil(model.issue) }
        }
        XCTAssertEqual(history.retained, 8)
    }
}
