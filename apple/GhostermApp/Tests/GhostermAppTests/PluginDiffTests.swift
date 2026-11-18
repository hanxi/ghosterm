import AppKit
import Foundation
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

@MainActor final class PluginDiffFixture {
    var description: [String: Any] = ["viewId": "diff-a", "revision": "18446744073709551615", "title": "Text comparison",
        "beforeLabel": "Before", "afterLabel": "After", "beforeLines": 3, "afterLines": 3, "added": 2, "removed": 2, "total": 5, "pageSize": 3]
    var rows: [[String: Any]] = [
        ["kind": "equal", "beforeLine": 1, "afterLine": 1, "text": "alpha", "ending": "crlf"],
        ["kind": "delete", "beforeLine": 2, "afterLine": 0, "text": "beta", "ending": "lf"],
        ["kind": "delete", "beforeLine": 3, "afterLine": 0, "text": "last", "ending": "none"],
        ["kind": "insert", "beforeLine": 0, "afterLine": 2, "text": "gamma", "ending": "lf"],
        ["kind": "insert", "beforeLine": 0, "afterLine": 3, "text": "last", "ending": "lf"]]
    var calls: [(String, Data)] = []
    var unknown = false
    var transform: (([String: Any]) -> [String: Any])?
    var pauseMethod: String?
    var barrier: CheckedContinuation<Data, Never>?
    private var paused: Data?
    func outcome(_ value: [String: Any]) throws -> Data { try JSONSerialization.data(withJSONObject: ["kind": "success", "value": value], options: [.sortedKeys]) }
    func page(_ offset: Int = 0) -> [String: Any] {
        let prefix = rows.prefix(offset)
        return ["viewId": description["viewId"]!, "revision": description["revision"]!, "offset": offset, "total": description["total"]!,
            "beforeOffset": prefix.filter { $0["kind"] as? String != "insert" }.count,
            "afterOffset": prefix.filter { $0["kind"] as? String != "delete" }.count,
            "rows": Array(rows.dropFirst(offset).prefix(description["pageSize"] as! Int))]
    }
    func request(_ method: String, _ input: Data) async throws -> Data {
        calls.append((method, input))
        let value: [String: Any]
        if method == "view.diff.describe" { value = description }
        else { value = page((try JSONSerialization.jsonObject(with: input) as! [String: Any])["offset"] as! Int) }
        let data = unknown ? Data(#"{"kind":"unknown","reason":"UNCONFIRMED"}"#.utf8) : try outcome(transform?(value) ?? value)
        if pauseMethod == method { pauseMethod = nil; paused = data; return await withCheckedContinuation { barrier = $0 } }
        return data
    }
    func release() { barrier?.resume(returning: paused!); barrier = nil; paused = nil }
    func model(isCurrent: @escaping @MainActor () -> Bool = { true }) -> PluginDiffModel {
        .init(rpc: { try await self.request($0, $1) }, isCurrent: isCurrent)
    }
}

@MainActor final class PluginDiffTests: XCTestCase {
    private func until(_ predicate: () -> Bool) async throws {
        let end = ProcessInfo.processInfo.systemUptime + 5
        while !predicate() && ProcessInfo.processInfo.systemUptime < end { try await Task.sleep(for: .milliseconds(5)) }
        XCTAssertTrue(predicate(), "Expected diff fixture barrier")
    }
    func testDescriptorCountsFieldsAndCanonicalRevision() throws {
        let f = PluginDiffFixture(); XCTAssertEqual(try PluginDiffCodec.descriptor(f.outcome(f.description)).revision, "18446744073709551615")
        for (key, value) in [("beforeLines", 4), ("afterLines", 4), ("added", 4), ("removed", -1), ("total", 4),
            ("pageSize", 4), ("total", true), ("added", 1.5), ("extra", 0)] as [(String, Any)] {
            var d = f.description; d[key] = value; XCTAssertThrowsError(try PluginDiffCodec.descriptor(f.outcome(d)))
        }
        for revision in ["0", "01", "+1", "1\n", "18446744073709551616"] {
            f.description["revision"] = revision; XCTAssertThrowsError(try PluginDiffCodec.descriptor(f.outcome(f.description)))
        }
    }
    func testViewLabelsAndCodepointLimits() throws {
        let f = PluginDiffFixture(); f.description["title"] = String(repeating: "😀", count: 128)
        XCTAssertEqual(try PluginDiffCodec.descriptor(f.outcome(f.description)).title.unicodeScalars.count, 128)
        for (key, value) in [("viewId", "x\n"), ("beforeLabel", ""), ("afterLabel", "bad\0"), ("title", String(repeating: "e\u{301}", count: 65))] {
            var d = f.description; d[key] = value; XCTAssertThrowsError(try PluginDiffCodec.descriptor(f.outcome(d)))
        }
    }
    func testPageBindsViewRevisionOffsetAndTotal() throws {
        let f = PluginDiffFixture(), d = try PluginDiffCodec.descriptor(f.outcome(f.description))
        for (key, value) in [("viewId", "other"), ("revision", "1"), ("offset", 3), ("total", 6), ("extra", 0)] as [(String, Any)] {
            var p = f.page(); p[key] = value; XCTAssertThrowsError(try PluginDiffCodec.page(f.outcome(p), descriptor: d, offset: 0, prefix: .zero))
        }
    }
    func testPageRequiresCompleteRowsAndExactCursor() throws {
        let f = PluginDiffFixture(), d = try PluginDiffCodec.descriptor(f.outcome(f.description))
        var p = f.page(); p["rows"] = Array(f.rows.prefix(2))
        XCTAssertThrowsError(try PluginDiffCodec.page(f.outcome(p), descriptor: d, offset: 0, prefix: .zero))
        p = f.page(3); p["beforeOffset"] = 2
        XCTAssertThrowsError(try PluginDiffCodec.page(f.outcome(p), descriptor: d, offset: 3, prefix: .init(before: 3, after: 1)))
        p = f.page(3); p["afterOffset"] = 0
        XCTAssertThrowsError(try PluginDiffCodec.page(f.outcome(p), descriptor: d, offset: 3, prefix: .init(before: 3, after: 1)))
    }
    func testRowsRequireExactCoordinatesKindsAndEndings() throws {
        let f = PluginDiffFixture(), d = try PluginDiffCodec.descriptor(f.outcome(f.description))
        for (key, value) in [("kind", "update"), ("beforeLine", 0), ("afterLine", 2), ("ending", "unknown"), ("text", "a\r\nb"), ("text", "bad\0")] as [(String, Any)] {
            var p = f.page(), rows = f.rows; rows[0][key] = value; p["rows"] = Array(rows.prefix(3))
            XCTAssertThrowsError(try PluginDiffCodec.page(f.outcome(p), descriptor: d, offset: 0, prefix: .zero))
        }
    }
    func testNoFinalNewlineOnlyOnLastSourceLine() throws {
        let f = PluginDiffFixture(), d = try PluginDiffCodec.descriptor(f.outcome(f.description))
        var p = f.page(), rows = f.rows; rows[0]["ending"] = "none"; p["rows"] = Array(rows.prefix(3))
        XCTAssertThrowsError(try PluginDiffCodec.page(f.outcome(p), descriptor: d, offset: 0, prefix: .zero))
        XCTAssertEqual(try PluginDiffCodec.page(f.outcome(f.page()), descriptor: d, offset: 0, prefix: .zero).rows.last?.ending, PluginDiffPage.Row.Ending.none)
    }
    func testWideUnicodeAndStrictRawJSON() throws {
        let f = PluginDiffFixture(); f.description.merge(["beforeLines": 3, "afterLines": 3, "added": 0, "removed": 0, "total": 3]) { _, v in v }
        f.rows = (1...3).map { ["kind": "equal", "beforeLine": $0, "afterLine": $0, "text": String(repeating: "😀", count: 128), "ending": "lf"] }
        let d = try PluginDiffCodec.descriptor(f.outcome(f.description)); XCTAssertEqual(try PluginDiffCodec.page(f.outcome(f.page()), descriptor: d, offset: 0, prefix: .zero).rows.count, 3)
        let normal = String(decoding: try f.outcome(f.description), as: UTF8.self)
        for bad in [normal + "{}", normal.replacingOccurrences(of: #""total":3"#, with: #""total":3,"total":3"#), normal.replacingOccurrences(of: "Text comparison", with: #"\ud800"#)] {
            XCTAssertThrowsError(try PluginDiffCodec.descriptor(Data(bad.utf8)))
        }
        XCTAssertThrowsError(try PluginDiffCodec.descriptor(Data([0xc0, 0x80])))
    }
    func testNativePagingUsesOnlyVerifiedReachableCursors() async throws {
        let f = PluginDiffFixture(), m = f.model(); await m.load()
        XCTAssertNil(m.issue); XCTAssertEqual(m.page?.rows.map(\.text), ["alpha", "beta", "last"])
        XCTAssertTrue(m.canNext); XCTAssertFalse(m.canPrevious)
        await m.showPage(offset: 3); XCTAssertEqual(m.page?.start, .init(before: 3, after: 1)); XCTAssertEqual(m.page?.rows.map(\.text), ["gamma", "last"])
        XCTAssertFalse(m.canNext); XCTAssertTrue(m.canPrevious)
        await m.showPage(offset: 0); XCTAssertEqual(m.page?.rows.last?.ending, PluginDiffPage.Row.Ending.none)
        let count = f.calls.count
        await m.showPage(offset: 1); await m.showPage(offset: 5); await m.showPage(offset: -1)
        XCTAssertEqual(f.calls.count, count)
    }
    func testUnvisitedPageIsRejectedWithoutRPC() async {
        let f = PluginDiffFixture(); f.description.merge(["beforeLines": 9, "afterLines": 9, "added": 0, "removed": 0, "total": 9]) { _, v in v }
        f.rows = (1...9).map { ["kind": "equal", "beforeLine": $0, "afterLine": $0, "text": "line", "ending": "lf"] }
        let m = f.model(); await m.load(); await m.showPage(offset: 6)
        XCTAssertEqual(f.calls.count, 2); XCTAssertEqual(m.page?.offset, 0)
    }
    func testEmptyAndUnknownStayDistinct() async {
        let f = PluginDiffFixture(); f.rows = []; f.description.merge(["beforeLines": 0, "afterLines": 0, "added": 0, "removed": 0, "total": 0]) { _, v in v }
        let m = f.model(); await m.load(); XCTAssertEqual(m.page?.rows.count, 0); XCTAssertNil(m.issue); XCTAssertFalse(m.canNext)
        let unknown = PluginDiffFixture(); unknown.unknown = true; let unconfirmed = unknown.model(); await unconfirmed.load()
        XCTAssertNotNil(unconfirmed.issue); XCTAssertNil(unconfirmed.page); XCTAssertEqual(unknown.calls.count, 1)
    }
    func testNewSelectionClearsRowsAndPreventsOverlappingReads() async throws {
        let f = PluginDiffFixture(), m = f.model(); await m.load(); f.pauseMethod = "view.diff.page"
        let task = Task { await m.showPage(offset: 3) }; try await until { f.barrier != nil }
        XCTAssertTrue(m.busy); XCTAssertNil(m.page); XCTAssertEqual(m.selectedOffset, 3)
        await m.showPage(offset: 0); XCTAssertEqual(f.calls.count, 3)
        f.release(); await task.value; XCTAssertEqual(m.page?.offset, 3)
    }
    func testLateDescriptorCannotStartAnotherRead() async throws {
        let f = PluginDiffFixture(); var current = true; let m = f.model { current }; f.pauseMethod = "view.diff.describe"
        let task = Task { await m.load() }; try await until { f.barrier != nil }; current = false; f.release(); await task.value
        XCTAssertNil(m.descriptor); XCTAssertNil(m.page); XCTAssertEqual(f.calls.count, 1)
    }
    func testLatePageCannotReopenInvalidatedView() async throws {
        let f = PluginDiffFixture(), m = f.model(); await m.load(); f.pauseMethod = "view.diff.page"
        let task = Task { await m.showPage(offset: 3) }; try await until { f.barrier != nil }
        m.invalidate(); f.release(); await task.value
        XCTAssertTrue(m.page == nil && !m.valid && !m.busy && m.issue == "差异所属会话已结束")
        await m.load(); XCTAssertEqual(f.calls.count, 3)
    }
    func testBootstrapAllowsOnlyUniqueImplementedServices() throws {
        let hash = String(repeating: "a", count: 64)
        func request(_ caps: [String]) -> ExtensionRelayBootstrap.Request {
            .init(executable: "/gterm", executorDirectory: "/package", executorDigest: hash,
                clientDigest: hash, methods: PluginDiffModel.methods, clientCapabilities: caps, command: "diff")
        }
        XCTAssertTrue(try ExtensionRelayBootstrap.build(request(["ui.diff"])).contains("'--client-capability' 'ui.diff'"))
        XCTAssertTrue(try ExtensionRelayBootstrap.build(request(["ui.table", "ui.diff"])).contains("'--client-capability' 'ui.table'"))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(["ui.diff", "ui.diff"])))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(["ui.chat"])))
    }
    func testNativeDiffThemeLargeTextAccessibilityAndRealPaging() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApp.accessibilitySetValue(true, forAttribute: enhanced); defer { NSApp.accessibilitySetValue(false, forAttribute: enhanced) }
        let f = PluginDiffFixture(), m = f.model(); await m.load()
        for appearance in [NSAppearance.Name.aqua, .darkAqua] { for points: CGFloat in [13, 28] {
            await m.showPage(offset: 0)
            let host = NSHostingView(rootView: PluginDiffView(model: m).padding(16).environment(\.font, .system(size: points)).background(Color(nsColor: .windowBackgroundColor)))
            host.frame = NSRect(x: 0, y: 0, width: 760, height: 680)
            let window = PluginTableWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false; window.contentView = host; window.appearance = NSAppearance(named: appearance); TestWindows.presentOffscreen(window)
            defer { window.close() }; try await Task.sleep(for: .milliseconds(100)); host.layoutSubtreeIfNeeded()
            XCTAssertEqual(window.alphaValue, 0); XCTAssertTrue(NSScreen.screens.allSatisfy { !$0.frame.intersects(window.frame) })
            let words = try PluginTableRendering.words(host).joined(separator: " ")
            XCTAssertTrue(words.contains("alpha") && words.contains("beta"), words)
            let nodes = PluginTableRendering.elements(host)
            let text = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-diff-text-0" })
            XCTAssertEqual(PluginTableRendering.attribute(text, "accessibilityValue"), "alpha")
            let changed = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-diff-text-1" })
            XCTAssertEqual(PluginTableRendering.attribute(changed, "accessibilityLabel"), "删除，原第 2 行，新文无此行")
            let ending = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-diff-ending-0" })
            // SwiftUI exposes the label of a macOS static text as its AX value.
            XCTAssertEqual(PluginTableRendering.attribute(ending, "accessibilityValue"), "换行：CRLF")
            let next = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-diff-next" })
            XCTAssertTrue(PluginTableRendering.press(next)); try await until { m.page?.offset == 3 }
            XCTAssertEqual(m.page?.rows.first?.text, "gamma")
        } }
    }
}
