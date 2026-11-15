import AppKit
import GhostermCore
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class PluginSignatureUITests: XCTestCase {
    func testExplicitPolicyAndSignatureThroughActualPackageControls() async throws {
        guard ExtensionRuntime.supportStatus == 0,
              let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] else {
            throw XCTSkip("requires JS-enabled Core and explicit CLI public release fixture")
        }
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let root = URL(fileURLWithPath: path), config = root.appendingPathComponent("ui-config")
        try Data().write(to: config)
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: root.appendingPathComponent("ui-known").path,
                                     pluginResourceDirectory: root.appendingPathComponent("ui-resources"))
        let controller = try XCTUnwrap(model.extensionController)
        addTeardownBlock { let closed = await controller.close(); XCTAssertTrue(closed) }
        let pane = NSHostingView(rootView: ScrollView {
            ExtensionsPane(model: model, packagePicker: { $0(.success([root.appendingPathComponent("p")])) })
        })
        let window = PluginTableWindow(contentRect: NSRect(x: 0, y: 0, width: 1440, height: 1400),
            styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = pane
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        func nodes() -> [NSObject] { pane.layoutSubtreeIfNeeded(); return PluginTableRendering.elements(pane) }
        func node(_ id: String) -> NSObject? {
            nodes().first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == id }
        }
        func press(_ id: String) throws {
            let control = try XCTUnwrap(node(id), id)
            XCTAssertTrue(PluginTableRendering.press(control), id)
        }
        func set(_ id: String, _ value: String) throws {
            let control = try XCTUnwrap(node(id), id)
            let selector = NSSelectorFromString("setAccessibilityValue:")
            guard control.responds(to: selector) else { XCTFail("not editable: \(id)"); return }
            control.perform(selector, with: value)
            let field = try XCTUnwrap((control as? NSTextField) ?? (control as? NSCell)?.controlView as? NSTextField)
            NotificationCenter.default.post(name: NSControl.textDidChangeNotification, object: field)
        }
        func until(_ check: () -> Bool) async throws -> Bool {
            let deadline = ProcessInfo.processInfo.systemUptime + 10
            while !check(), ProcessInfo.processInfo.systemUptime < deadline { try await Task.sleep(for: .milliseconds(25)) }
            return check()
        }
        guard try await until({ node("plugin-package-import") != nil }) else { XCTFail("import missing"); return }
        try press("plugin-package-import")
        guard try await until({ node("plugin-signature-disclosure") != nil }) else { XCTFail("signature review missing"); return }
        try press("plugin-signature-disclosure")
        guard try await until({ node("plugin-signature-descriptor") != nil }) else { XCTFail("signature fields missing"); return }
        for (id, value) in [("descriptor", "r/release.json"), ("file", "r/release.sig"), ("policy", "policy.json")] {
            try set("plugin-signature-" + id, root.appendingPathComponent(value).path)
            try await Task.sleep(for: .milliseconds(25))
        }
        XCTAssertNil(node("plugin-signature-verified"))
        XCTAssertNil(model.extensionRun)
        try press("plugin-signature-policy-trust")
        try await Task.sleep(for: .milliseconds(25))
        try press("plugin-signature-verify")
        guard try await until({ node("plugin-signature-verified") != nil || node("plugin-signature-issue") != nil }) else {
            XCTFail("signature result missing"); return
        }
        XCTAssertNotNil(node("plugin-signature-verified"), "explicit public policy and CLI signature must verify through actual controls")
        XCTAssertNil(node("plugin-signature-issue"))
        XCTAssertNil(node("plugin-launch-org.ghosterm.preview"), "signature must not approve the source")
        XCTAssertNil(model.extensionRun, "signature must not activate a runtime")
        try set("plugin-signature-descriptor", root.appendingPathComponent("missing-release.json").path)
        try await Task.sleep(for: .milliseconds(25))
        XCTAssertNil(node("plugin-signature-verified"), "editing the descriptor must invalidate the old proof")
        try press("plugin-signature-verify")
        guard try await until({ node("plugin-signature-issue") != nil }) else { XCTFail("failed read must be visible"); return }
        XCTAssertNil(node("plugin-signature-verified"))
    }
}
