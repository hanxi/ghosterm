import AppKit
import CryptoKit
import Foundation
import GhostermCore
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class PluginReleaseInstallUITests: XCTestCase {
    func testInspectCancelAndInstallThroughActualControls() async throws {
        try await exercise(useChannel: false)
    }

    func testConfiguredChannelInstallAndEnableThroughActualControls() async throws {
        try await exercise(useChannel: true)
    }

    private func exercise(useChannel: Bool) async throws {
        guard ExtensionRuntime.supportStatus == 0,
              let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] else {
            throw XCTSkip("requires explicit JS-enabled Core and signed release fixtures")
        }
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let input = URL(fileURLWithPath: path), root = input.appendingPathComponent("install-ui-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try PluginReleaseInstallTests.thaw(root); try FileManager.default.removeItem(at: root) }
        let config = root.appendingPathComponent("config"); try Data().write(to: config)
        let channelTrust: ExtensionController.ChannelTrust?
        if useChannel {
            let policy = try Data(contentsOf: input.appendingPathComponent("policy.json"))
            let fields = try XCTUnwrap(JSONSerialization.jsonObject(with: policy) as? [String: Any])
            let channel = try XCTUnwrap(fields["channel"] as? String)
            let key = Curve25519.Signing.PrivateKey()
            let now = UInt64(Date().timeIntervalSince1970)
            let state = root.appendingPathComponent("channel")
            try FileManager.default.createDirectory(at: state, withIntermediateDirectories: false,
                                                    attributes: [.posixPermissions: 0o700])
            let digest = SHA256.hash(data: policy).map { String(format: "%02x", $0) }.joined()
            let statement = try JSONSerialization.data(withJSONObject: [
                "schemaVersion": 1, "channel": channel, "generation": "1",
                "issuedAt": String(now), "expiresAt": String(now + 600), "policySHA256": digest,
            ])
            let signature = try key.signature(for: Data("ghosterm.channel-policy.v1\0".utf8) + statement)
            _ = try PluginDirectorySignatureVerifier.refreshChannelPolicy(statement: statement, signature: signature,
                policy: policy, channel: channel, channelRoot: key.publicKey.rawRepresentation, now: now, stateDirectory: state)
            channelTrust = .init(channel: channel, root: key.publicKey.rawRepresentation, stateDirectory: state)
        } else { channelTrust = nil }
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: root.appendingPathComponent("known").path,
                                     pluginResourceDirectory: root.appendingPathComponent("resources"), pluginChannelTrust: channelTrust)
        let controller = try XCTUnwrap(model.extensionController)
        addTeardownBlock { let closed = await controller.close(); XCTAssertTrue(closed) }
        let pane = NSHostingView(rootView: ScrollView { ExtensionsPane(model: model) })
        let window = PluginTableWindow(contentRect: NSRect(x: 0, y: 0, width: 1440, height: useChannel ? 2000 : 1400),
            styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = pane; TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        func node(_ id: String) -> NSObject? {
            pane.layoutSubtreeIfNeeded()
            return PluginTableRendering.elements(pane).first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == id }
        }
        func press(_ id: String) throws { XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(node(id), id)), id) }
        func enabled(_ id: String) -> Bool {
            guard let button = node(id) else { return false }
            let selector = NSSelectorFromString("isAccessibilityEnabled")
            guard button.responds(to: selector), let implementation = button.method(for: selector) else { return false }
            typealias Enabled = @convention(c) (AnyObject, Selector) -> Bool
            return unsafeBitCast(implementation, to: Enabled.self)(button, selector)
        }
        func set(_ id: String, _ value: String) throws {
            let control = try XCTUnwrap(node(id)), selector = NSSelectorFromString("setAccessibilityValue:")
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
        try press("plugin-release-expand")
        guard try await until({ node("plugin-release-artifact") != nil }) else { XCTFail("install fields missing"); return }
        var files = [("artifact", "r/plugin.zip"), ("descriptor", "r/release.json"), ("signature", "r/release.sig")]
        if useChannel {
            try press("plugin-release-channel")
            guard try await until({ node("plugin-release-policy") == nil }) else { XCTFail("channel mode still asks for offline policy"); return }
        } else { files.append(("policy", "policy.json")) }
        for (id, value) in files {
            try set("plugin-release-" + id, input.appendingPathComponent(value).path)
            try await Task.sleep(for: .milliseconds(25))
        }
        XCTAssertNil(node("plugin-release-install"), "install must require an inspected release")
        if !useChannel { try press("plugin-release-trust"); try await Task.sleep(for: .milliseconds(25)) }
        try press("plugin-release-inspect")
        guard try await until({ node("plugin-release-install") != nil }) else { XCTFail("signed release inspection missing"); return }
        XCTAssertNil(node("plugin-approve-org.ghosterm.preview"), "inspection must not register a package")
        try press("plugin-release-cancel")
        guard try await until({ node("plugin-release-install") == nil && enabled("plugin-release-inspect") })
        else { XCTFail("cancel must discard preparation and finish cleanup"); return }
        try press("plugin-release-inspect")
        guard try await until({ node("plugin-release-install") != nil }) else { XCTFail("cancelled stage must release the store lock"); return }
        try press("plugin-release-install")
        guard try await until({ node("plugin-release-installed") != nil }) else { XCTFail("explicit install must publish a signed release"); return }
        XCTAssertNotNil(node("plugin-approve-org.ghosterm.preview"), "installed package must enter normal source review")
        XCTAssertNil(node("plugin-launch-org.ghosterm.preview"), "installation must not approve the source")
        XCTAssertNil(model.extensionRun, "installation must not activate a runtime")
        if useChannel {
            let catalog = try await controller.installedVersions()
            let release = try XCTUnwrap(catalog.versions.first)
            XCTAssertTrue(release.requiresChannelPolicy)
            try press("plugin-release-expand")
            try press("plugin-installed-expand")
            let select = "plugin-installed-select-" + release.releaseID
            guard try await until({ enabled(select) }) else { XCTFail("installed channel version missing"); return }
            try press(select)
            guard try await until({ node("plugin-selection-review") != nil }) else { XCTFail("channel version review missing"); return }
            XCTAssertFalse(enabled("plugin-selection-confirm"))
            try press("plugin-selection-acknowledge")
            guard try await until({ enabled("plugin-selection-confirm") }) else { XCTFail("channel review was not acknowledged"); return }
            try press("plugin-selection-confirm")
            guard try await until({ node("plugin-selection-review") == nil }) else { XCTFail("channel review did not commit"); return }
            let selected = try await controller.installedVersions()
            XCTAssertTrue(try XCTUnwrap(selected.versions.first).enabled)
            XCTAssertNil(model.extensionRun)
        }
    }
}
