import AppKit
import Foundation
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class PluginUnclassifiedDirectoryUITests: XCTestCase {
    func testManualAcknowledgementResetsOnCancelAndControlsDeletion() async throws {
        TestWindows.enterAutomationMode()
        let root = sessionTestDirectory().appendingPathComponent("residue-ui-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try PluginReleaseInstallTests.thaw(root) }
        let directory = root.appendingPathComponent("ordinary folder")
        try FileManager.default.createDirectory(at: directory.appendingPathComponent("payload"), withIntermediateDirectories: true)
        try Data("retained".utf8).write(to: directory.appendingPathComponent("payload/file"))
        let controller = try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: [], capabilities: []), installationDirectory: root)
        addTeardownBlock { let closed = await controller.close(); XCTAssertTrue(closed) }
        let entries = try await controller.unclassifiedEntries()
        let id = try XCTUnwrap(entries.first { $0.name == directory.lastPathComponent }).id
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let pane = NSHostingView(rootView: ScrollView { PluginInstalledVersionsView(controller: controller) { _, _ in } })
        let window = PluginTableWindow(contentRect: NSRect(x: 0, y: 0, width: 1440, height: 1400), styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = pane; TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        func node(_ id: String) -> NSObject? {
            pane.layoutSubtreeIfNeeded()
            return PluginTableRendering.elements(pane).first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == id }
        }
        func press(_ id: String) throws { XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(node(id), id)), id) }
        func until(_ body: () -> Bool) async throws -> Bool {
            let deadline = ProcessInfo.processInfo.systemUptime + 10
            while !body(), ProcessInfo.processInfo.systemUptime < deadline { try await Task.sleep(for: .milliseconds(25)) }
            return body()
        }
        func enabled(_ id: String) throws -> Bool {
            let value = try XCTUnwrap(node(id), id), selector = NSSelectorFromString("isAccessibilityEnabled")
            guard value.responds(to: selector), let implementation = value.method(for: selector) else { XCTFail("missing enabled state"); return true }
            typealias Read = @convention(c) (AnyObject, Selector) -> Bool
            return unsafeBitCast(implementation, to: Read.self)(value, selector)
        }
        try press("plugin-installed-expand")
        guard try await until({ node("plugin-unknown-preview-" + id) != nil }) else { XCTFail("unknown directory controls missing"); return }
        try press("plugin-unknown-preview-" + id)
        guard try await until({ node("plugin-installed-remove-cancel") != nil }) else { XCTFail("exact tree preview missing"); return }
        XCTAssertEqual(try? Data(contentsOf: directory.appendingPathComponent("payload/file")), Data("retained".utf8))
        XCTAssertFalse(try enabled("plugin-installed-remove-confirm"))
        try press("plugin-unknown-acknowledge")
        guard try await until({ (try? enabled("plugin-installed-remove-confirm")) == true }) else { XCTFail("acknowledgement did not enable confirmation"); return }
        try press("plugin-installed-remove-cancel")
        guard try await until({ node("plugin-installed-remove-cancel") == nil }) else { XCTFail("cancel did not release ticket"); return }
        try press("plugin-unknown-preview-" + id)
        guard try await until({ node("plugin-installed-remove-confirm") != nil }) else { XCTFail("repeat preview missing"); return }
        XCTAssertFalse(try enabled("plugin-installed-remove-confirm"), "new preview must require fresh acknowledgement")
        try press("plugin-unknown-acknowledge")
        guard try await until({ (try? enabled("plugin-installed-remove-confirm")) == true }) else { XCTFail("repeat acknowledgement did not enable confirmation"); return }
        try press("plugin-installed-remove-confirm")
        guard try await until({ node("plugin-unknown-row-" + id) == nil }) else { XCTFail("deleted unknown directory remained listed"); return }
        XCTAssertFalse(FileManager.default.fileExists(atPath: directory.path))
        let listing = try await controller.installationResidues(); XCTAssertTrue(listing.entries.isEmpty)
    }
}
