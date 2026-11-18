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
        func waitAndPress(_ id: String) async throws {
            let selector = NSSelectorFromString("isAccessibilityEnabled")
            let ready = try await until {
                guard let value = node(id), value.responds(to: selector),
                      let implementation = value.method(for: selector) else { return false }
                typealias ReadEnabled = @convention(c) (AnyObject, Selector) -> Bool
                return unsafeBitCast(implementation, to: ReadEnabled.self)(value, selector)
            }
            guard ready else { throw NSError(domain: "PluginInstalledVersionsUITests: control did not become enabled: " + id, code: 1) }
            try press(id)
        }
        func waitNotice(_ expected: String) async throws {
            guard try await until({
                guard let control = node("plugin-installed-notice") else { return false }
                return ["accessibilityValue", "accessibilityLabel"].contains { PluginTableRendering.attribute(control, $0)?.contains(expected) == true }
            }) else { XCTFail("version operation did not report \(expected)"); return }
        }
        func confirmSelection() async throws {
            guard try await until({ node("plugin-selection-confirm") != nil }) else { XCTFail("selection must show its complete review before changing versions"); return }
            try press("plugin-selection-acknowledge")
            try await waitAndPress("plugin-selection-confirm")
        }
        try press("plugin-installed-expand")
        guard try await until({ versions.allSatisfy { node("plugin-installed-row-" + $0.signature.descriptorSHA256) != nil } }) else { XCTFail("restart inventory controls missing"); return }
        XCTAssertNil(node("plugin-launch-org.ghosterm.preview"))
        let field = try XCTUnwrap(node("plugin-installed-policy"))
        field.perform(NSSelectorFromString("setAccessibilityValue:"), with: input.appendingPathComponent("policy.json").path)
        let native = try XCTUnwrap((field as? NSTextField) ?? (field as? NSCell)?.controlView as? NSTextField)
        NotificationCenter.default.post(name: NSControl.textDidChangeNotification, object: native)
        try await Task.sleep(for: .milliseconds(25))
        try await waitAndPress("plugin-installed-trust")
        try await waitAndPress("plugin-installed-select-" + versions[1].signature.descriptorSHA256)
        try await confirmSelection()
        try await waitNotice("活动版本已更新")
        guard try await until({ node("plugin-approve-org.ghosterm.preview") != nil }) else { XCTFail("selected version must enter source review"); return }
        XCTAssertNil(node("plugin-launch-org.ghosterm.preview"), "selection must not approve source")
        try press("plugin-installed-disable-" + versions[1].signature.descriptorSHA256)
        try await waitNotice("已禁用新建")
        try await waitAndPress("plugin-installed-select-" + versions[0].signature.descriptorSHA256)
        try await confirmSelection()
        try await waitNotice("活动版本已更新")
        let catalog = try await controller.installedVersions()
        XCTAssertEqual(catalog.versions.first(where: { $0.selected })?.releaseID, versions[0].signature.descriptorSHA256)
        XCTAssertNil(node("plugin-launch-org.ghosterm.preview"))
        XCTAssertNil(model.extensionRun)
        let retired = versions[1].signature.descriptorSHA256
        try press("plugin-installed-remove-preview-" + retired)
        guard try await until({ node("plugin-installed-remove-cancel") != nil }) else { XCTFail("removal preview missing"); return }
        XCTAssertTrue(FileManager.default.fileExists(atPath: versions[1].directory.path), "preview must not delete")
        try press("plugin-installed-remove-cancel")
        guard try await until({ node("plugin-installed-remove-cancel") == nil }) else { XCTFail("cancel did not release preview"); return }
        XCTAssertTrue(FileManager.default.fileExists(atPath: versions[1].directory.path))
        try press("plugin-installed-remove-preview-" + retired)
        guard try await until({ node("plugin-installed-remove-confirm") != nil }) else { XCTFail("second preview missing"); return }
        try press("plugin-installed-remove-confirm")
        try await waitNotice("回收已完成")
        guard try await until({ node("plugin-installed-row-" + retired) == nil }) else { XCTFail("deleted version row retained"); return }
        XCTAssertFalse(FileManager.default.fileExists(atPath: versions[1].directory.path))
        let remaining = try await controller.installedVersions()
        XCTAssertEqual(remaining.versions.map(\.releaseID), [versions[0].signature.descriptorSHA256])
        XCTAssertTrue(remaining.versions[0].selected && remaining.versions[0].enabled)
        try press("plugin-installed-disable-" + versions[0].signature.descriptorSHA256)
        try await waitNotice("已禁用新建")
        let disabled = try await controller.installedVersions()
        enum Stop: Error { case intentional }
        do {
            let root = versions[0].directory.deletingLastPathComponent().deletingLastPathComponent()
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
            let preview = try PluginReleaseRemoval(owner: owner, releaseID: versions[0].signature.descriptorSHA256,
                expectedRevision: disabled.revision, checkpoint: { if $0 == "after-intent" { throw Stop.intentional } })
            XCTAssertThrowsError(try preview.confirm(owner: owner))
        }
        try press("plugin-installed-refresh")
        guard try await until({ node("plugin-installed-remove-pending-cancel") != nil }) else { XCTFail("pending recovery controls missing"); return }
        try press("plugin-installed-remove-pending-cancel")
        try await waitNotice("已撤销尚未开始的删除")
        XCTAssertTrue(FileManager.default.fileExists(atPath: versions[0].directory.path))
        let final = try await controller.installedVersions(); XCTAssertFalse(final.removalPending)
    }
}
