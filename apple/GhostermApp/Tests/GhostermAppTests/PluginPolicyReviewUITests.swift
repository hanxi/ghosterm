import AppKit
import Foundation
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class PluginPolicyReviewUITests: XCTestCase {
    func testReviewCancelAndExplicitSubsetDisableControls() async throws {
        TestWindows.enterAutomationMode()
        let f = try PluginPolicyFixture(); addTeardownBlock { try PluginReleaseInstallTests.thaw(f.directory) }
        try f.add(); try f.add("org.ghosterm.two"); try f.writePolicy(revoked: true)
        let controller = try f.controller(); addTeardownBlock { let closed = await controller.close(); XCTAssertTrue(closed) }
        var disabled: [String] = []
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let pane = NSHostingView(rootView: ScrollView { PluginInstalledVersionsView(controller: controller) { id, _ in disabled.append(id) } })
        let window = PluginTableWindow(contentRect: NSRect(x: 0, y: 0, width: 1440, height: 1800), styleMask: [.borderless], backing: .buffered, defer: false)
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
        try press("plugin-installed-expand")
        guard try await until({ f.choices.values.allSatisfy { node("plugin-installed-row-" + $0.releaseID) != nil } && (try? enabled("plugin-installed-trust")) == true }) else { XCTFail("policy form did not finish initial refresh"); return }
        let field = try XCTUnwrap(node("plugin-installed-policy"))
        field.perform(NSSelectorFromString("setAccessibilityValue:"), with: f.policy.path)
        let native = try XCTUnwrap((field as? NSTextField) ?? (field as? NSCell)?.controlView as? NSTextField)
        NotificationCenter.default.post(name: NSControl.textDidChangeNotification, object: native)
        try await Task.sleep(for: .milliseconds(25))
        XCTAssertFalse(try enabled("plugin-policy-review-open"))
        try press("plugin-installed-trust")
        guard try await until({ (try? enabled("plugin-policy-review-open")) == true }) else { XCTFail("explicit policy trust did not enable review"); return }
        try press("plugin-policy-review-open")
        guard try await until({ node("plugin-policy-select-org.ghosterm.one") != nil }) else { XCTFail("review rows missing"); return }
        XCTAssertFalse(try enabled("plugin-policy-review-apply"), "no automatic subset selection")
        XCTAssertTrue(try f.selection().choices["org.ghosterm.one"]!.enabled)
        try press("plugin-policy-select-org.ghosterm.one")
        try press("plugin-policy-review-cancel")
        guard try await until({ node("plugin-policy-review-cancel") == nil }) else { XCTFail("cancel did not dismiss review"); return }
        XCTAssertTrue(try f.selection().choices["org.ghosterm.one"]!.enabled)
        XCTAssertTrue(disabled.isEmpty)
        try press("plugin-policy-review-open")
        guard try await until({ node("plugin-policy-select-org.ghosterm.one") != nil }) else { XCTFail("repeat review missing"); return }
        XCTAssertFalse(try enabled("plugin-policy-review-apply"), "repeated review must clear old subset")
        try press("plugin-policy-select-org.ghosterm.one")
        guard try await until({ (try? enabled("plugin-policy-review-apply")) == true }) else { XCTFail("explicit subset did not enable confirm"); return }
        try press("plugin-policy-review-apply")
        guard try await until({ disabled == ["org.ghosterm.one"] }) else { XCTFail("explicit disable did not report changed choice"); return }
        XCTAssertFalse(try f.selection().choices["org.ghosterm.one"]!.enabled)
        XCTAssertTrue(try f.selection().choices["org.ghosterm.two"]!.enabled)
    }
    func testPolicyPathOrTrustChangeClearsReviewedSubset() async throws {
        let f = try PluginPolicyFixture(); addTeardownBlock { try PluginReleaseInstallTests.thaw(f.directory) }
        try f.add(); try f.writePolicy(revoked: true)
        let controller = try f.controller(); addTeardownBlock { let done = await controller.close(); XCTAssertTrue(done) }
        let draft = PluginInstalledVersionsDraft(); await draft.refresh(controller)
        draft.policyPath = f.policy.path; draft.trustedPolicy = true
        await draft.reviewPolicy(controller); XCTAssertNotNil(draft.policyReview)
        draft.policyDisableIDs = ["org.ghosterm.one"]
        draft.policyPath = f.policy.path
        XCTAssertNil(draft.policyReview); XCTAssertTrue(draft.policyDisableIDs.isEmpty); XCTAssertFalse(draft.trustedPolicy)
        draft.trustedPolicy = true; await draft.reviewPolicy(controller); XCTAssertNotNil(draft.policyReview)
        draft.policyDisableIDs = ["org.ghosterm.one"]; draft.trustedPolicy = false
        XCTAssertNil(draft.policyReview); XCTAssertTrue(draft.policyDisableIDs.isEmpty)
        let result = await draft.applyPolicy(controller); XCTAssertNil(result)
        XCTAssertTrue(try f.selection().choices["org.ghosterm.one"]!.enabled)
    }
}
