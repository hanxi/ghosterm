import AppKit
import Foundation
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

@MainActor
final class PluginTableFixture {
    var description: [String: Any] = ["viewId": "table-a", "revision": "18446744073709551615", "title": "Number powers",
        "columns": [["id": "n", "title": "Number"], ["id": "square", "title": "Square"], ["id": "cube", "title": "Cube"]],
        "total": 1_000_000, "pageSize": 16]
    var calls: [(String, Data)] = []
    var pauseMethod: String?
    var barrier: CheckedContinuation<Data, Never>?
    private var paused: Data?
    var unknown = false
    var transform: (([String: Any]) -> [String: Any])?

    func outcome(_ value: [String: Any]) throws -> Data {
        try JSONSerialization.data(withJSONObject: ["kind": "success", "value": value], options: [.sortedKeys, .withoutEscapingSlashes])
    }
    func page(_ offset: Int = 0) -> [String: Any] {
        let total = description["total"] as! Int, limit = description["pageSize"] as! Int
        let rows = (0..<min(limit, total - offset)).map { index -> [String: Any] in
            let n = offset + index + 1
            return ["id": String(n), "cells": [String(n), String(n * n), String(n * n * n)]]
        }
        return ["viewId": description["viewId"]!, "revision": description["revision"]!, "offset": offset, "total": total, "rows": rows]
    }
    func request(_ method: String, _ input: Data) async throws -> Data {
        calls.append((method, input))
        let value: [String: Any]
        if method == "view.table.describe" { value = description }
        else if method == PluginTableModel.searchMethod {
            let parameters = try JSONSerialization.jsonObject(with: input) as! [String: Any]
            let first = parameters["after"] as? Int == 0
            let rows = first ? [] : page()["rows"] as! [[String: Any]]
            value = ["viewId": description["viewId"]!, "revision": description["revision"]!,
                "cursor": first ? 128 : 256, "latest": 256, "truncated": first, "rows": Array(rows.prefix(1))]
        } else {
            let parameters = try JSONSerialization.jsonObject(with: input) as! [String: Any]
            value = page(parameters["offset"] as! Int)
        }
        let data = unknown ? Data(#"{"kind":"unknown","reason":"FIXTURE_UNCONFIRMED"}"#.utf8) : try outcome(transform?(value) ?? value)
        if pauseMethod == method {
            pauseMethod = nil; paused = data
            return await withCheckedContinuation { barrier = $0 }
        }
        return data
    }
    func release() { barrier?.resume(returning: paused!); barrier = nil; paused = nil }
    func model(isCurrent: @escaping @MainActor () -> Bool = { true }) -> PluginTableModel {
        PluginTableModel(rpc: { try await self.request($0, $1) }, isCurrent: isCurrent)
    }
}

@MainActor
final class PluginTableTests: XCTestCase {
    private func until(_ predicate: () -> Bool) async throws {
        let end = ProcessInfo.processInfo.systemUptime + 5
        while !predicate() && ProcessInfo.processInfo.systemUptime < end { try await Task.sleep(for: .milliseconds(5)) }
        XCTAssertTrue(predicate(), "Expected explicit table fixture barrier")
    }
    func testSearchContinuesFromAnEmptyPageAndDropsLateReply() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let f = PluginTableFixture()
        let model = PluginTableModel(rpc: { try await f.request($0, $1) }, isCurrent: { true }, supportsSearch: true)
        await model.load()
        let host = NSHostingView(rootView: PluginTableView(model: model).padding(16))
        host.frame = NSRect(x: 0, y: 0, width: 760, height: 640)
        let window = PluginTableWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        TestWindows.presentOffscreen(window)
        defer { window.close() }
        func status(_ identifier: String, _ message: String) -> Bool {
            host.layoutSubtreeIfNeeded()
            return PluginTableRendering.elements(host).contains { node in
                PluginTableRendering.attribute(node, "accessibilityIdentifier") == identifier &&
                    ["accessibilityLabel", "accessibilityValue"].contains {
                        PluginTableRendering.attribute(node, $0) == message
                    }
            }
        }
        f.pauseMethod = PluginTableModel.searchMethod
        let first = Task { await model.search("value") }
        defer { f.release() }
        try await until { f.barrier != nil && status("plugin-table-loading", "正在搜索表格…") }
        f.release()
        await first.value
        try await until { status("plugin-table-range", "本页没有匹配项，可继续搜索") }
        XCTAssertEqual(model.searchPage?.rows, [])
        XCTAssertEqual(model.searchPage?.cursor, 128)
        XCTAssertTrue(model.searchTruncated)
        await model.continueSearch()
        try await until { status("plugin-table-range", "本页有 1 个匹配项") }
        XCTAssertEqual(model.searchPage?.rows.map(\.id), ["1"])
        XCTAssertTrue(model.searchTruncated)
        let parameters = try XCTUnwrap(JSONSerialization.jsonObject(with: f.calls.last!.1) as? [String: Any])
        XCTAssertEqual(parameters["after"] as? Int, 128)
        XCTAssertEqual(parameters["through"] as? Int, 256)
        XCTAssertEqual(parameters["query"] as? String, "value")

        f.pauseMethod = PluginTableModel.searchMethod
        let pending = Task { await model.search("later") }
        try await until { f.barrier != nil }
        model.invalidate()
        f.release()
        await pending.value
        XCTAssertNil(model.searchPage)
        XCTAssertNil(model.searchQuery)
    }
    func testCanonicalRevisionAndExactDescriptor() throws {
        let f = PluginTableFixture()
        XCTAssertEqual(try PluginTableCodec.descriptor(f.outcome(f.description)).revision, "18446744073709551615")
        for revision in ["0", "01", "+1", "1\n", "1\u{2028}", "18446744073709551616"] {
            f.description["revision"] = revision
            XCTAssertThrowsError(try PluginTableCodec.descriptor(f.outcome(f.description)))
        }
    }
    func testDescriptorRejectsUnknownKeysBooleanAndFraction() throws {
        let f = PluginTableFixture()
        for (key, value) in [("extra", 1), ("total", true), ("pageSize", 1.5)] as [(String, Any)] {
            var d = f.description; d[key] = value
            XCTAssertThrowsError(try PluginTableCodec.descriptor(f.outcome(d)))
        }
    }
    func testColumnCountAndUniqueIDs() throws {
        let f = PluginTableFixture()
        let first = ["id": "n", "title": "Number"]
        for columns in [[], Array(repeating: first, count: 9), [first, first]] {
            f.description["columns"] = columns
            XCTAssertThrowsError(try PluginTableCodec.descriptor(f.outcome(f.description)))
        }
    }
    func testTitlesUseCodepointsAndRejectControlCharacters() throws {
        let f = PluginTableFixture()
        f.description["title"] = String(repeating: "😀", count: 128)
        XCTAssertEqual(try PluginTableCodec.descriptor(f.outcome(f.description)).title.unicodeScalars.count, 128)
        for title in ["", String(repeating: "e\u{301}", count: 65), "bad\0text", "bad\u{7f}text"] {
            f.description["title"] = title
            XCTAssertThrowsError(try PluginTableCodec.descriptor(f.outcome(f.description)))
        }
    }
    func testPageRejectsMismatchedViewRevisionOffsetAndTotal() throws {
        let f = PluginTableFixture(), d = try PluginTableCodec.descriptor(f.outcome(f.description))
        for (key, value) in [("viewId", "other"), ("revision", "1"), ("offset", 16), ("total", 2)] as [(String, Any)] {
            var p = f.page(); p[key] = value
            XCTAssertThrowsError(try PluginTableCodec.page(f.outcome(p), descriptor: d, offset: 0))
        }
    }
    func testPageRequiresExactRowCount() throws {
        let f = PluginTableFixture(), d = try PluginTableCodec.descriptor(f.outcome(f.description))
        var p = f.page(); p["rows"] = Array((p["rows"] as! [[String: Any]]).dropLast())
        XCTAssertThrowsError(try PluginTableCodec.page(f.outcome(p), descriptor: d, offset: 0))
    }
    func testPageRequiresColumnWidthAndUniqueRows() throws {
        let f = PluginTableFixture(); f.description["total"] = 2
        let d = try PluginTableCodec.descriptor(f.outcome(f.description))
        var p = f.page(), rows = p["rows"] as! [[String: Any]]
        rows[0]["cells"] = ["one"]; p["rows"] = rows
        XCTAssertThrowsError(try PluginTableCodec.page(f.outcome(p), descriptor: d, offset: 0))
        p = f.page(); rows = p["rows"] as! [[String: Any]]; rows[1]["id"] = rows[0]["id"]; p["rows"] = rows
        XCTAssertThrowsError(try PluginTableCodec.page(f.outcome(p), descriptor: d, offset: 0))
    }
    func testAggregateBytesAndStrictJSON() throws {
        let f = PluginTableFixture(); f.description["total"] = 1
        let d = try PluginTableCodec.descriptor(f.outcome(f.description))
        var p = f.page()
        p["rows"] = [["id": "1", "cells": Array(repeating: String(repeating: "😀", count: 180), count: 3)]]
        XCTAssertThrowsError(try PluginTableCodec.page(f.outcome(p), descriptor: d, offset: 0))
        let normal = String(decoding: try f.outcome(f.description), as: UTF8.self)
        for bad in [normal.replacingOccurrences(of: #""total":1"#, with: #""total":1,"total":1"#),
                    normal + "{}", normal.replacingOccurrences(of: "Number powers", with: #"\ud800"#)] {
            XCTAssertThrowsError(try PluginTableCodec.descriptor(Data(bad.utf8)))
        }
    }
    func testBoundedPagingAndExactLargeIntegers() async throws {
        let f = PluginTableFixture(), m = f.model()
        await m.load()
        XCTAssertEqual(m.page?.rows.count, 16); XCTAssertFalse(m.canPrevious); XCTAssertTrue(m.canNext)
        await m.showPage(offset: m.lastOffset)
        XCTAssertEqual(m.page?.rows.last?.cells, ["1000000", "1000000000000", "1000000000000000000"])
        XCTAssertEqual(m.selectedOffset, 999984); XCTAssertTrue(m.canPrevious); XCTAssertFalse(m.canNext)
        XCTAssertEqual(f.calls.count, 3)
        let input = try XCTUnwrap(JSONSerialization.jsonObject(with: f.calls.last!.1) as? [String: Any])
        XCTAssertEqual(input["revision"] as? String, f.description["revision"] as? String)
        XCTAssertEqual(input["limit"] as? Int, 16)
        await m.showPage(offset: -1); await m.showPage(offset: 1_000_000); await m.showPage(offset: 1)
        XCTAssertEqual(f.calls.count, 3)
    }
    func testEmptyDatasetIsConfirmed() async {
        let f = PluginTableFixture(); f.description["total"] = 0
        let m = f.model(); await m.load()
        XCTAssertEqual(m.page?.rows.count, 0); XCTAssertNil(m.issue)
        XCTAssertFalse(m.canPrevious); XCTAssertFalse(m.canNext)
    }
    func testUnknownIsNotAnEmptyTableOrAutomaticRetry() async {
        let f = PluginTableFixture(); f.unknown = true
        let m = f.model(); await m.load()
        XCTAssertNotNil(m.issue); XCTAssertNil(m.page); XCTAssertNil(m.descriptor)
        XCTAssertFalse(m.busy); XCTAssertEqual(f.calls.count, 1)
    }
    func testSelectingPageClearsOldRowsAndPreventsOverlappingReads() async throws {
        let f = PluginTableFixture(), m = f.model(); await m.load()
        f.pauseMethod = "view.table.page"
        let t = Task { await m.showPage(offset: 16) }
        try await until { f.barrier != nil }
        XCTAssertTrue(m.busy); XCTAssertEqual(m.selectedOffset, 16); XCTAssertNil(m.page)
        await m.showPage(offset: 32)
        XCTAssertEqual(f.calls.count, 3)
        f.release(); await t.value
        XCTAssertEqual(m.page?.offset, 16); XCTAssertEqual(m.page?.rows.first?.id, "17")
    }
    func testLateDescriptorAfterRuntimeReplacementIsIgnored() async throws {
        let f = PluginTableFixture(); var current = true
        let m = f.model { current }; f.pauseMethod = "view.table.describe"
        let t = Task { await m.load() }
        try await until { f.barrier != nil }
        current = false; f.release(); await t.value
        XCTAssertNil(m.descriptor); XCTAssertNil(m.page); XCTAssertEqual(f.calls.count, 1)
    }
    func testLatePageAfterInvalidationCannotReopenTheView() async throws {
        let f = PluginTableFixture(), m = f.model(); await m.load()
        f.pauseMethod = "view.table.page"
        let t = Task { await m.showPage(offset: 16) }
        try await until { f.barrier != nil }
        m.invalidate(); f.release(); await t.value
        XCTAssertTrue(m.page == nil && !m.valid && !m.busy && m.issue == "表格所属会话已结束")
        await m.load(); await m.showPage(offset: 0)
        XCTAssertEqual(f.calls.count, 3)
    }
    func testNativeTablePixelsAccessibilityThemeAndLargeText() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let f = PluginTableFixture(); f.description["total"] = 17
        f.transform = { reply in
            guard reply["offset"] as? Int == 0, var rows = reply["rows"] as? [[String: Any]] else { return reply }
            rows[0]["cells"] = ["1", "1", ""]
            var updated = reply
            updated["rows"] = rows
            return updated
        }
        let m = f.model(); await m.load()
        for appearance in [NSAppearance.Name.aqua, .darkAqua] {
            for points: CGFloat in [13, 28] {
                await m.showPage(offset: 0)
                let host = NSHostingView(rootView: PluginTableView(model: m).padding(16)
                    .environment(\.font, .system(size: points)).background(Color(nsColor: .windowBackgroundColor)))
                host.frame = NSRect(x: 0, y: 0, width: 760, height: 640)
                let window = PluginTableWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
                window.isReleasedWhenClosed = false; window.contentView = host; window.appearance = NSAppearance(named: appearance)
                TestWindows.presentOffscreen(window)
                defer { window.close() }
                try await Task.sleep(for: .milliseconds(100)); host.layoutSubtreeIfNeeded()
                XCTAssertEqual(window.alphaValue, 0)
                XCTAssertTrue(NSScreen.screens.allSatisfy { !$0.frame.intersects(window.frame) })
                let words = try PluginTableRendering.words(host).joined(separator: " ")
                XCTAssertTrue(words.contains("Number") && words.contains("Square") && words.contains("Cube"), words)
                let nodes = PluginTableRendering.elements(host)
                let cell = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-table-cell-1-n" },
                    nodes.map { node in ["accessibilityIdentifier", "accessibilityLabel", "accessibilityValue"].map { name in PluginTableRendering.attribute(node, name) ?? "" } }.description)
                XCTAssertTrue(PluginTableRendering.attribute(cell, "accessibilityLabel")?.contains("Number") == true)
                XCTAssertEqual(PluginTableRendering.attribute(cell, "accessibilityValue"), "1")
                let empty = try XCTUnwrap(nodes.first {
                    PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-table-cell-1-cube"
                })
                XCTAssertEqual(PluginTableRendering.attribute(empty, "accessibilityLabel"), "Cube，第 1 行")
                XCTAssertEqual(PluginTableRendering.attribute(empty, "accessibilityValue"), "空白")
                let next = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-table-next" })
                guard PluginTableRendering.press(next) else {
                    XCTFail("AX press unavailable: \(type(of: next)), role \(PluginTableRendering.attribute(next, "accessibilityRole") ?? "nil"), actions \(next.accessibilityActionNames())")
                    return
                }
                try await until { m.page?.offset == 16 }
                XCTAssertEqual(m.page?.rows.first?.id, "17")
            }
        }
    }
}
