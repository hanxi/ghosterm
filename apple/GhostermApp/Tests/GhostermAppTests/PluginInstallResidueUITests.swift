import AppKit
import Foundation
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class PluginInstallResidueUITests: XCTestCase {
    func testExplicitPreviewCancelAndCleanupButtons() async throws {
        TestWindows.enterAutomationMode()
        let root = sessionTestDirectory().appendingPathComponent("residue-ui-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try PluginReleaseInstallTests.thaw(root) }
        var stage: PluginReleaseStage? = try PluginReleaseStage(directory: root)
        try stage!.write(Data("retained".utf8), path: "payload/file")
        let directory = stage!.payloadURL.deletingLastPathComponent(), id = String(stage!.name.dropFirst(".prepare-".count))
        stage = nil
        let controller = try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: [], capabilities: []), installationDirectory: root)
        addTeardownBlock { let closed = await controller.close(); XCTAssertTrue(closed) }
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
        try press("plugin-installed-expand")
        guard try await until({ node("plugin-install-residue-preview-" + id) != nil }) else { XCTFail("known abandoned record controls missing"); return }
        try press("plugin-install-residue-preview-" + id)
        guard try await until({ node("plugin-installed-remove-cancel") != nil }) else { XCTFail("exact tree preview missing"); return }
        XCTAssertEqual(try? Data(contentsOf: directory.appendingPathComponent("payload/file")), Data("retained".utf8))
        try press("plugin-installed-remove-cancel")
        guard try await until({ node("plugin-installed-remove-cancel") == nil }) else { XCTFail("cancel did not release ticket"); return }
        try press("plugin-install-residue-preview-" + id)
        guard try await until({ node("plugin-installed-remove-confirm") != nil }) else { XCTFail("repeat preview missing"); return }
        try press("plugin-installed-remove-confirm")
        guard try await until({ node("plugin-install-residue-" + id) == nil }) else { XCTFail("completed residue record remained"); return }
        XCTAssertFalse(FileManager.default.fileExists(atPath: directory.path))
        let listing = try await controller.installationResidues(); XCTAssertTrue(listing.entries.isEmpty)
    }
}
