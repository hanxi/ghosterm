import AppKit
import CryptoKit
import GhostermCore
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

/// Actual production controls from package selection callback through owned SSH
/// Preview and stop. The system chooser is a fixed test-only return value.
@MainActor
final class PluginPaneFlowTests: XCTestCase {
    private func until(_ check: () -> Bool) async throws -> Bool {
        let deadline = ProcessInfo.processInfo.systemUptime + 10
        while !check(), ProcessInfo.processInfo.systemUptime < deadline {
            try await Task.sleep(for: .milliseconds(25))
        }
        return check()
    }

    func testPreviewThroughPackageApprovalLaunchConsentAndStopControls() async throws {
        let env = ProcessInfo.processInfo.environment
        guard ExtensionRuntime.supportStatus == 0, let gterm = env["GHOSTERM_TEST_GTERM"],
              let host = env["GHOSTERM_SSH_HOST"], let port = env["GHOSTERM_SSH_PORT"],
              let key = env["GHOSTERM_SSH_KEY"], let user = env["GHOSTERM_SSH_USER"] else {
            throw XCTSkip("requires JS-enabled Core and the explicitly owned gate SSH fixture")
        }
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let repository = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let package = repository.appendingPathComponent("plugins/official/preview")
        let id = "org.ghosterm.preview"
        let root = repository.appendingPathComponent("tmp/pane-" + UUID().uuidString.prefix(8))
        let workspace = root.appendingPathComponent("workspace")
        try FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: root) }
        let document = Data("# Preview controls\n\nOwned SSH pane flow token\n".utf8)
        try document.write(to: workspace.appendingPathComponent("index.md"))
        let config = root.appendingPathComponent("config"), known = root.appendingPathComponent("known")
        try Data("Host pane-target\n HostName \(host)\n Port \(port)\n User \(user)\n IdentityFile \(key)\n".utf8).write(to: config)
        try Data().write(to: known)
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: known.path,
                                     pluginResourceDirectory: root.appendingPathComponent("received"))
        let controller = try XCTUnwrap(model.extensionController)
        addTeardownBlock { await model.stopExtensionRun(); let closed = await controller.close(); XCTAssertTrue(closed) }
        model.interaction = AcceptAllInteraction()
        var selections = 0
        let pane = NSHostingView(rootView: ScrollView { ExtensionsPane(model: model, packagePicker: { completion in
            selections += 1
            completion(.success([package]))
        }) })
        let window = PluginTableWindow(contentRect: NSRect(x: 0, y: 0, width: 1440, height: 1400),
                                       styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = pane
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        func nodes() -> [NSObject] { pane.layoutSubtreeIfNeeded(); return PluginTableRendering.elements(pane) }
        func node(_ identifier: String) -> NSObject? {
            nodes().first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == identifier }
        }
        func press(_ identifier: String) throws {
            let control = try XCTUnwrap(node(identifier), identifier)
            XCTAssertTrue(PluginTableRendering.press(control), identifier)
        }
        func set(_ identifier: String, _ value: String) throws {
            let control = try XCTUnwrap(node(identifier), identifier)
            let selector = NSSelectorFromString("setAccessibilityValue:")
            guard control.responds(to: selector) else { throw GhostermError.failed("not editable: \(identifier)") }
            control.perform(selector, with: value)
            let field = try XCTUnwrap((control as? NSTextField) ?? (control as? NSCell)?.controlView as? NSTextField, "Native editable field: \(type(of: control))")
            NotificationCenter.default.post(name: NSControl.textDidChangeNotification, object: field)
        }
        func scrollBottom() throws {
            func scrollViews(_ view: NSView) -> [NSScrollView] {
                (view as? NSScrollView).map { [$0] } ?? view.subviews.flatMap(scrollViews)
            }
            pane.layoutSubtreeIfNeeded()
            let scroll = try XCTUnwrap(scrollViews(pane).first)
            let clip = scroll.contentView
            let documentView = try XCTUnwrap(scroll.documentView)
            clip.scroll(to: NSPoint(x: 0, y: documentView.isFlipped ? max(0, documentView.bounds.height - clip.bounds.height) : 0))
            scroll.reflectScrolledClipView(clip)
        }
        func hasText(_ value: String) -> Bool {
            nodes().contains { node in
                ["accessibilityValue", "accessibilityLabel"].contains {
                    PluginTableRendering.attribute(node, $0)?.contains(value) == true
                }
            }
        }
        guard try await until({ node("plugin-package-import") != nil }) else { XCTFail("import control missing"); return }
        XCTAssertNil(model.extensionRun)
        try press("plugin-package-import")
        guard try await until({ node("plugin-approve-" + id) != nil }) else { XCTFail("directory selection callback did not review selected package"); return }
        XCTAssertEqual(selections, 1)
        let digest = SHA256.hash(data: try Data(contentsOf: package.appendingPathComponent("integrity.json")))
            .map { String(format: "%02x", $0) }.joined()
        XCTAssertTrue(hasText(digest), "review must display the exact immutable package digest")
        XCTAssertTrue(hasText("未认证"))
        XCTAssertNil(model.extensionRun, "review must not activate the plugin")
        XCTAssertNil(node("plugin-launch-" + id), "source decision must precede the launch form")
        try press("plugin-approve-" + id)
        guard try await until({ node("plugin-launch-" + id) != nil }) else { XCTFail("approval did not reveal launch form"); return }
        for (name, value) in [
            ("ssh-alias", "pane-target"), ("executable", gterm),
            ("executor-directory", package.path), ("executor-digest", digest),
            ("arguments", "[\"index.md\"]"), ("resource-root", workspace.path),
        ] {
            try set("plugin-" + name + "-" + id, value)
            try await Task.sleep(for: .milliseconds(25))
        }
        XCTAssertNil(model.extensionRun, "editing the launch form must not connect")
        try scrollBottom()
        try await Task.sleep(for: .milliseconds(50))
        try press("plugin-launch-" + id)
        guard try await until({ node("plugin-consent-allow") != nil && !model.extensionRunBusy }) else {
            XCTFail("launch control did not reach consent: \(model.extensionRunIssue ?? "none")"); return
        }
        XCTAssertEqual(model.extensionRun?.session?.phase, ExtensionSession.Phase.consent.rawValue)
        XCTAssertNil(model.extensionRun?.runtime, "source approval must not grant runtime consent")
        XCTAssertEqual(model.extensionRun?.arguments, ["index.md"])
        XCTAssertTrue(model.extensionRun?.remoteCommand.contains("'--root' 'workspace' '\(workspace.path)'") == true)
        try scrollBottom()
        try await Task.sleep(for: .milliseconds(50))
        try press("plugin-consent-allow")
        guard try await until({ model.extensionRun?.receivedResources.first?.bytes == document }) else {
            XCTFail("Preview did not receive exact owned SSH resource: \(model.extensionRunIssue ?? "none")"); return
        }
        XCTAssertNil(model.extensionRunIssue)
        XCTAssertEqual(model.extensionRun?.receivedResources.count, 1)
        XCTAssertEqual(model.extensionRun?.runtime?.phase, ExtensionRuntime.Phase.active.rawValue)
        try scrollBottom()
        try await Task.sleep(for: .milliseconds(100))
        guard try await until({
            (try? PluginTableRendering.words(pane).joined(separator: " ").lowercased().contains("owned ssh pane flow token")) == true
        }) else { XCTFail("production pane did not draw received Markdown"); return }
        XCTAssertEqual(window.alphaValue, 0)
        XCTAssertFalse(NSScreen.screens.contains { $0.frame.intersects(window.frame) })
        let receivedID = try XCTUnwrap(model.extensionRun?.receivedResources.first?.resourceID)
        try press("plugin-stop")
        guard try await until({ model.extensionRun == nil && !model.extensionRunBusy }) else { XCTFail("stop control did not release run"); return }
        XCTAssertNil(model.extensionRunIssue)
        XCTAssertNil(node("plugin-resource-select-" + receivedID))
        XCTAssertNil(node("plugin-consent-allow"))
    }
}
