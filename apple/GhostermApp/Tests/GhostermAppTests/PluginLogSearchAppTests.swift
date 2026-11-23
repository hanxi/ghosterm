import AppKit
import CryptoKit
import Foundation
import GhostermCore
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

@MainActor final class PluginLogSearchAppTests: XCTestCase {
    func testLogSearchControlsContinueAfterEmptyPage() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let binary = env["GHOSTERM_LOGS_GTERM"], let packagePath = env["GHOSTERM_LOGS_PACKAGE"],
              let scratch = env["GHOSTERM_LOGS_SCRATCH"] else {
            throw XCTSkip("requires the owned Lua logs fixture")
        }
        XCTAssertEqual(ExtensionRuntime.supportStatus, 0)
        let root = URL(fileURLWithPath: scratch), package = URL(fileURLWithPath: packagePath)
        let binaryDigest = SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: binary)))
            .map { String(format: "%02x", $0) }.joined()
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }

        let work = root.appendingPathComponent("logs")
        try FileManager.default.createDirectory(at: work, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        let stage = work.appendingPathComponent("stage")
        try FileManager.default.createDirectory(at: stage, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        let controller = try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1],
            runtimes: ["lua"], capabilities: ["session.rpc", "ui.table"]),
            localJournalDirectory: work.appendingPathComponent("journal"))
        let model = ExtensionInstanceModel(controller: controller)
        addTeardownBlock { await model.stopExtensionRun(); let closed = await controller.close(); XCTAssertTrue(closed) }
        let review = try await controller.review(directory: package)
        XCTAssertEqual(Set(review.requiredCapabilities), ["session.rpc", "ui.table"])
        let approved = try await controller.approve(id: review.id, digest: review.digest)
        XCTAssertEqual(approved, 0)
        let launch = ExtensionLocalLaunch(executableSHA256: binaryDigest,
            workingDirectory: work.path, temporaryDirectory: stage.path, authorized: true)
        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest, command: "logs",
            arguments: [], sshAlias: "", executable: binary, executorDirectory: package.path,
            executorDigest: review.digest, methods: [], hostMethods: [], localLaunch: launch)
        if let issue = model.extensionRunIssue { throw GhostermError.failed(issue) }
        XCTAssertTrue(model.extensionRun?.methods.contains(PluginTableModel.searchMethod) == true)
        await model.decideExtensionConsent(allow: true)
        try await until { model.extensionTable != nil || model.extensionRunIssue != nil }
        if let issue = model.extensionRunIssue { throw GhostermError.failed(issue) }
        let table = try XCTUnwrap(model.extensionTable)
        XCTAssertTrue(table.supportsSearch)
        let pane = NSHostingView(rootView: ExtensionRunView(model: model))
        pane.frame = NSRect(x: 0, y: 0, width: 1280, height: 1800)
        let window = PluginTableWindow(contentRect: pane.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = pane
        TestWindows.presentOffscreen(window)
        defer { window.close() }
        func editableField(in view: NSView) -> NSTextField? {
            if let field = view as? NSTextField, field.isEditable { return field }
            return view.subviews.lazy.compactMap { editableField(in: $0) }.first
        }
        func control(_ id: String) -> NSObject? {
            pane.layoutSubtreeIfNeeded()
            return PluginTableRendering.elements(pane).first {
                PluginTableRendering.attribute($0, "accessibilityIdentifier") == id
            }
        }
        func press(_ id: String) async throws {
            try await until { control(id) != nil }
            XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(control(id))), id)
        }
        try await until { table.page != nil && editableField(in: pane) != nil }
        XCTAssertEqual(table.page?.rows.first?.id, "905")
        let field = try XCTUnwrap(editableField(in: pane))
        XCTAssertTrue(window.makeFirstResponder(field))
        let editor = try XCTUnwrap(field.currentEditor() as? NSTextView)
        editor.insertText("record 1100", replacementRange: editor.selectedRange())
        try await Task.sleep(for: .milliseconds(80))
        try await press("plugin-table-search")
        try await until { table.searchPage != nil || table.issue != nil }
        XCTAssertNil(table.issue)
        XCTAssertEqual(table.searchPage?.rows, [])
        XCTAssertEqual(table.searchPage?.cursor, 1032)
        XCTAssertTrue(table.searchTruncated)
        try await press("plugin-table-search-more")
        try await until { table.searchPage?.cursor == 1160 || table.issue != nil }
        XCTAssertNil(table.issue)
        XCTAssertEqual(table.searchPage?.rows.first?.id, "1100")
        try await until { control("plugin-table-cell-1100-message") != nil }
        try await press("plugin-table-search-clear")
        try await until { table.searchQuery == nil && table.page != nil }
        XCTAssertEqual(table.page?.rows.first?.id, "905")
    }

    private func until(_ predicate: () -> Bool) async throws {
        let end = ProcessInfo.processInfo.systemUptime + 12
        while !predicate() && ProcessInfo.processInfo.systemUptime < end {
            try await Task.sleep(for: .milliseconds(20))
        }
        guard predicate() else { throw GhostermError.failed("Plugin log search observation timed out") }
    }
}
