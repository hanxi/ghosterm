import AppKit
import Foundation
import GhostermCore
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class PluginSelectionReviewUITests: XCTestCase {
    func testActualReviewCancelPathChangeAcknowledgementAndCommit() async throws {
        guard ExtensionRuntime.supportStatus == 0, let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] else { throw XCTSkip("requires explicit JS-enabled signed selection fixtures") }
        TestWindows.enterAutomationMode()
        let input = URL(fileURLWithPath: path), root = sessionTestDirectory().appendingPathComponent("selection-ui-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try PluginReleaseInstallTests.thaw(root) }
        let controller = try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: ["lua"], capabilities: ["session.rpc", "resource.receive"]), resourceDirectory: root, installationDirectory: root.appendingPathComponent("installed"))
        addTeardownBlock { let done = await controller.close(); XCTAssertTrue(done) }
        var versions: [ExtensionController.InstalledRelease] = []
        for url in [input, input.appendingPathComponent("upgrade")] {
            let pending = try await controller.prepareSignedRelease(artifact: url.appendingPathComponent("r/plugin.zip"), descriptor: url.appendingPathComponent("r/release.json"),
                signature: url.appendingPathComponent("r/release.sig"), trustedPolicy: url.appendingPathComponent("policy.json"))
            versions.append(try await controller.installPreparedRelease(id: pending.id))
        }
        _ = try await controller.selectInstalledVersion(releaseID: versions[0].signature.descriptorSHA256, expectedRevision: nil, trustedPolicy: input.appendingPathComponent("policy.json"))
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        var changed: [String] = []
        let pane = NSHostingView(rootView: ScrollView { PluginInstalledVersionsView(controller: controller) { id, _ in changed.append(id) } })
        let window = PluginTableWindow(contentRect: NSRect(x: 0, y: 0, width: 1440, height: 2000), styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = pane; TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        func node(_ id: String) -> NSObject? {
            pane.layoutSubtreeIfNeeded()
            return PluginTableRendering.elements(pane).first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == id }
        }
        func press(_ id: String) throws { XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(node(id), id)), id) }
        func enabled(_ id: String) throws -> Bool {
            let value = try XCTUnwrap(node(id), id), selector = NSSelectorFromString("isAccessibilityEnabled")
            guard value.responds(to: selector), let implementation = value.method(for: selector) else { XCTFail("missing enabled state"); return true }
            typealias Read = @convention(c) (AnyObject, Selector) -> Bool
            return unsafeBitCast(implementation, to: Read.self)(value, selector)
        }
        func until(_ body: () -> Bool) async throws -> Bool {
            let deadline = ProcessInfo.processInfo.systemUptime + 10
            while !body(), ProcessInfo.processInfo.systemUptime < deadline { try await Task.sleep(for: .milliseconds(25)) }
            return body()
        }
        func setPolicy(_ path: String) throws {
            let field = try XCTUnwrap(node("plugin-installed-policy"))
            field.perform(NSSelectorFromString("setAccessibilityValue:"), with: path)
            let native = try XCTUnwrap((field as? NSTextField) ?? (field as? NSCell)?.controlView as? NSTextField)
            NotificationCenter.default.post(name: NSControl.textDidChangeNotification, object: native)
        }
        let button = "plugin-installed-select-" + versions[1].signature.descriptorSHA256
        try press("plugin-installed-expand")
        guard try await until({ versions.allSatisfy { node("plugin-installed-row-" + $0.signature.descriptorSHA256) != nil } && (try? enabled("plugin-installed-trust")) == true }) else { XCTFail("inventory form not ready"); return }
        try setPolicy(input.appendingPathComponent("policy.json").path)
        try await Task.sleep(for: .milliseconds(25)); try press("plugin-installed-trust")
        guard try await until({ (try? enabled(button)) == true }) else { XCTFail("trusted policy did not enable preview"); return }
        try press(button)
        guard try await until({ node("plugin-selection-confirm") != nil }) else { XCTFail("version comparison missing"); return }
        XCTAssertFalse(try enabled("plugin-selection-confirm"))
        let before = try await controller.installedVersions(); XCTAssertEqual(before.selections.first?.releaseID, versions[0].signature.descriptorSHA256)
        let words = PluginTableRendering.elements(pane).flatMap { node in ["accessibilityValue", "accessibilityLabel"].compactMap { PluginTableRendering.attribute(node, $0) } }.joined(separator: "\n")
        XCTAssertTrue(words.contains("process.spawn") && words.contains("preview-review"))
        XCTAssertTrue(words.contains("file.readScoped"))
        try press("plugin-selection-acknowledge"); try press("plugin-selection-cancel")
        guard try await until({ node("plugin-selection-confirm") == nil && (try? enabled(button)) == true }) else { XCTFail("cancel did not finish releasing comparison"); return }
        XCTAssertTrue(changed.isEmpty)
        try press(button)
        guard try await until({ node("plugin-selection-confirm") != nil }) else { XCTFail("repeat comparison missing"); return }
        XCTAssertFalse(try enabled("plugin-selection-confirm"))
        let alternate = root.appendingPathComponent("alternate-policy.json")
        try Data(contentsOf: input.appendingPathComponent("policy.json")).write(to: alternate)
        try setPolicy(alternate.path)
        guard try await until({ node("plugin-selection-confirm") == nil && (try? enabled("plugin-installed-trust")) == true }) else { XCTFail("policy editing did not finish candidate cancellation"); return }
        do {
            let removal = try await controller.previewVersionRemoval(releaseID: versions[1].signature.descriptorSHA256, expectedRevision: before.revision)
            await controller.cancelVersionRemoval(id: removal.id)
        } catch {
            XCTFail("policy editing did not release candidate reference: \(error)"); return
        }
        try press("plugin-installed-trust")
        guard try await until({ (try? enabled(button)) == true }) else { XCTFail("new policy trust missing"); return }
        try press(button)
        guard try await until({ node("plugin-selection-confirm") != nil }) else { XCTFail("third comparison missing"); return }
        XCTAssertFalse(try enabled("plugin-selection-confirm"))
        try press("plugin-selection-acknowledge")
        guard try await until({ (try? enabled("plugin-selection-confirm")) == true }) else { XCTFail("explicit acknowledgement did not enable commit"); return }
        try press("plugin-selection-confirm")
        guard try await until({ changed == [versions[1].review.id] }) else { XCTFail("confirmed comparison did not change choice"); return }
        let after = try await controller.installedVersions(); XCTAssertEqual(after.selections.first?.releaseID, versions[1].signature.descriptorSHA256)
        let status = try await controller.resolve(command: "preview", selectedID: versions[1].review.id, selectedDigest: versions[1].review.digest).status; XCTAssertNotEqual(status, 0)
    }
}
