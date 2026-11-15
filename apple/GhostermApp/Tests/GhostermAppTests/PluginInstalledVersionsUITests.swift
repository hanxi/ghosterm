import AppKit
import Foundation
import GhostermCore
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class PluginInstalledVersionsUITests: XCTestCase {
    func testCurrentPolicySelectDisableAndRollbackControls() async throws {
        guard ExtensionRuntime.supportStatus == 0, let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] else { throw XCTSkip("requires explicit signed fixtures") }
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let input = URL(fileURLWithPath: path), root = input.appendingPathComponent("versions-ui-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try PluginReleaseInstallTests.thaw(root); try FileManager.default.removeItem(at: root) }
        let config = root.appendingPathComponent("config"); try Data().write(to: config)
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: root.appendingPathComponent("known").path,
            pluginResourceDirectory: root.appendingPathComponent("resources"))
        let controller = try XCTUnwrap(model.extensionController)
        addTeardownBlock { let done = await controller.close(); XCTAssertTrue(done) }
        var versions: [ExtensionController.InstalledRelease] = []
        for directory in [input, input.appendingPathComponent("v2")] {
            let prepared = try await controller.prepareSignedRelease(artifact: directory.appendingPathComponent("r/plugin.zip"),
                descriptor: directory.appendingPathComponent("r/release.json"), signature: directory.appendingPathComponent("r/release.sig"), trustedPolicy: directory.appendingPathComponent("policy.json"))
            versions.append(try await controller.installPreparedRelease(id: prepared.id))
        }
        let pane = NSHostingView(rootView: ScrollView { ExtensionsPane(model: model) })
        let window = PluginTableWindow(contentRect: NSRect(x: 0, y: 0, width: 1440, height: 1600), styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = pane; TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        func node(_ id: String) -> NSObject? {
            pane.layoutSubtreeIfNeeded()
            return PluginTableRendering.elements(pane).first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == id }
        }
        func press(_ id: String) throws { XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(node(id), id)), id) }
        func until(_ check: () -> Bool) async throws -> Bool {
            let deadline = ProcessInfo.processInfo.systemUptime + 10
            while !check(), ProcessInfo.processInfo.systemUptime < deadline { try await Task.sleep(for: .milliseconds(25)) }
            return check()
        }
        func waitNotice(_ expected: String) async throws {
            guard try await until({
                guard let control = node("plugin-installed-notice") else { return false }
                return ["accessibilityValue", "accessibilityLabel"].contains { PluginTableRendering.attribute(control, $0)?.contains(expected) == true }
            }) else { XCTFail("version operation did not report \(expected)"); return }
        }
        try press("plugin-installed-expand")
        guard try await until({ versions.allSatisfy { node("plugin-installed-row-" + $0.signature.descriptorSHA256) != nil } }) else { XCTFail("restart inventory controls missing"); return }
        XCTAssertNil(node("plugin-launch-org.ghosterm.preview"))
        let field = try XCTUnwrap(node("plugin-installed-policy"))
        field.perform(NSSelectorFromString("setAccessibilityValue:"), with: input.appendingPathComponent("policy.json").path)
        let native = try XCTUnwrap((field as? NSTextField) ?? (field as? NSCell)?.controlView as? NSTextField)
        NotificationCenter.default.post(name: NSControl.textDidChangeNotification, object: native)
        try await Task.sleep(for: .milliseconds(25))
        try press("plugin-installed-trust"); try await Task.sleep(for: .milliseconds(25))
        try press("plugin-installed-select-" + versions[1].signature.descriptorSHA256)
        try await waitNotice("活动版本已更新")
        guard try await until({ node("plugin-approve-org.ghosterm.preview") != nil }) else { XCTFail("selected version must enter source review"); return }
        XCTAssertNil(node("plugin-launch-org.ghosterm.preview"), "selection must not approve source")
        try press("plugin-installed-disable-" + versions[1].signature.descriptorSHA256)
        try await waitNotice("已禁用新建")
        try press("plugin-installed-select-" + versions[0].signature.descriptorSHA256)
        try await waitNotice("活动版本已更新")
        let catalog = try await controller.installedVersions()
        XCTAssertEqual(catalog.versions.first(where: { $0.selected })?.releaseID, versions[0].signature.descriptorSHA256)
        XCTAssertNil(node("plugin-launch-org.ghosterm.preview"))
        XCTAssertNil(model.extensionRun)
    }
}
