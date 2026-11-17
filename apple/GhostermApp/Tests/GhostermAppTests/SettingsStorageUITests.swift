import AppKit
import Foundation
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
private final class SettingsStorageWindow: NSWindow {
    override var occlusionState: NSWindow.OcclusionState { [.visible] }
    override func constrainFrameRect(_ rect: NSRect, to screen: NSScreen?) -> NSRect {
        var result = rect; result.origin = TestWindows.offscreenOrigin; return result
    }
}

@MainActor
final class SettingsStorageUITests: XCTestCase {
    private func node(_ view: NSView, _ id: String) -> NSObject? {
        var seen: Set<ObjectIdentifier> = []
        func visit(_ value: NSObject) -> NSObject? {
            guard seen.count < 4096, seen.insert(ObjectIdentifier(value)).inserted else { return nil }
            let selector = NSSelectorFromString("accessibilityIdentifier")
            if value.responds(to: selector), value.perform(selector)?.takeUnretainedValue() as? String == id { return value }
            let children = NSSelectorFromString("accessibilityChildren")
            if value.responds(to: children), let nodes = value.perform(children)?.takeUnretainedValue() as? [NSObject] {
                for child in nodes { if let found = visit(child) { return found } }
            }
            return nil
        }
        view.layoutSubtreeIfNeeded(); return visit(view)
    }
    private func press(_ node: NSObject?) -> Bool {
        let selector = NSSelectorFromString("accessibilityPerformPress")
        guard let node, node.responds(to: selector), let implementation = node.method(for: selector) else { return false }
        typealias Action = @convention(c) (AnyObject, Selector) -> Bool
        return unsafeBitCast(implementation, to: Action.self)(node, selector)
    }
    func testActualOldEditorSaveCannotOverwriteNewControlCommit() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let root = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("settings-stale-ui-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let path = root.appendingPathComponent("ghostty.conf")
        try Data("font-size = 8\n".utf8).write(to: path)
        let model = isolatedAppModel(configPath: root.appendingPathComponent("ssh_config").path,
            knownHostsPath: root.appendingPathComponent("known_hosts").path,
            settings: SettingsStore(terminalConfigPath: path.path))
        let view = NSHostingView(rootView: SettingsView(model: model, initialPane: .configuration))
        let window = SettingsStorageWindow(contentRect: NSRect(x: 0, y: 0, width: 760, height: 600), styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = view; TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        func editor(_ view: NSView) -> NSTextView? {
            if let text = view as? NSTextView, text.isEditable { return text }
            for child in view.subviews { if let found = editor(child) { return found } }
            return nil
        }
        for _ in 0..<150 {
            if editor(view)?.string == "font-size = 8\n" { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        let loadedOld = editor(view)?.string == "font-size = 8\n"
        try model.applyTerminalSettings([("font-size", "19")])
        let pressed = press(node(view, "settings-config-save"))
        for _ in 0..<150 {
            if node(view, "settings-config-error") != nil { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertTrue(loadedOld && pressed && node(view, "settings-config-error") != nil && editor(view)?.string == "font-size = 8\n" && model.terminalApp?.float("font-size") == 19 && (try? Data(contentsOf: path)) == Data("font-size = 19\n".utf8))
    }
    func testOpeningAppearanceReadsActualSizeWithoutWritingConfiguration() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let root = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("settings-open-ui-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let path = root.appendingPathComponent("ghostty.conf"), original = "font-size = 8\n"
        try Data(original.utf8).write(to: path)
        let model = isolatedAppModel(configPath: root.appendingPathComponent("ssh_config").path,
            knownHostsPath: root.appendingPathComponent("known_hosts").path,
            settings: SettingsStore(terminalConfigPath: path.path))
        let revision = model.terminalConfigRevision
        let view = NSHostingView(rootView: SettingsView(model: model, initialPane: .appearance))
        let window = SettingsStorageWindow(contentRect: NSRect(x: 0, y: 0, width: 760, height: 700), styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = view; TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        func displayed() -> String? {
            guard let text = node(view, "settings-terminal-font-size") else { return nil }
            for name in ["accessibilityValue", "accessibilityLabel"] {
                let selector = NSSelectorFromString(name)
                if text.responds(to: selector), let value = text.perform(selector)?.takeUnretainedValue() as? String { return value }
            }
            return nil
        }
        for _ in 0..<150 {
            if displayed() == "8 pt" { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        await withCheckedContinuation { continuation in DispatchQueue.main.async { continuation.resume() } }
        await withCheckedContinuation { continuation in DispatchQueue.main.async { continuation.resume() } }
        XCTAssertTrue(displayed() == "8 pt" && model.terminalConfigRevision == revision && (try? Data(contentsOf: path)) == Data(original.utf8) && !FileManager.default.fileExists(atPath: path.path + ".bak"), "actual size=\(displayed() ?? "missing"), revision=\(model.terminalConfigRevision)")
    }
    func testActualConfigurationReadErrorAndExplicitReloadProtectTheFile() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let root = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("settings-ui-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let path = root.appendingPathComponent("ghostty.conf")
        try Data([255]).write(to: path)
        let model = isolatedAppModel(configPath: root.appendingPathComponent("ssh_config").path,
                                     knownHostsPath: root.appendingPathComponent("known_hosts").path,
                                     settings: SettingsStore(terminalConfigPath: path.path))
        let oldFont = model.terminalApp?.float("font-size")
        let view = NSHostingView(rootView: SettingsView(model: model, initialPane: .configuration))
        let window = SettingsStorageWindow(contentRect: NSRect(x: 0, y: 0, width: 760, height: 600), styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = view; TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        for _ in 0..<150 {
            if node(view, "settings-config-error") != nil { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        let visibleError = node(view, "settings-config-error") != nil && model.settings.loadError != nil
        _ = press(node(view, "settings-config-save"))
        let protected = (try? Data(contentsOf: path)) == Data([255])
        let replacement = "font-size = 19\n"
        try Data(replacement.utf8).write(to: path)
        _ = press(node(view, "settings-config-save"))
        let notAdopted = model.settings.loadError != nil && (try? Data(contentsOf: path)) == Data(replacement.utf8)
        let reloaded = press(node(view, "settings-config-reload"))
        for _ in 0..<150 {
            if model.settings.loadError == nil && node(view, "settings-config-error") == nil { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        let readOnly = model.terminalApp?.float("font-size") == oldFont && model.terminalConfigText == replacement
        let saved = press(node(view, "settings-config-save"))
        for _ in 0..<150 {
            if model.terminalApp?.float("font-size") == 19 { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertTrue(visibleError && protected && notAdopted && reloaded && readOnly && saved && model.terminalApp?.float("font-size") == 19 && node(view, "settings-config-error") == nil && (try? Data(contentsOf: path)) == Data(replacement.utf8))
    }
}
