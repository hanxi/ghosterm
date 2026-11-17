import AppKit
import GhostermCore
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
private final class SearchTestWindow: NSWindow {
    override var occlusionState: NSWindow.OcclusionState { [.visible] }
    override func constrainFrameRect(_ rect: NSRect, to screen: NSScreen?) -> NSRect {
        var result = rect; result.origin = TestWindows.offscreenOrigin; return result
    }
}

@MainActor
final class FileSearchUITests: XCTestCase {
    private func node(_ view: NSView, _ identifier: String) -> NSObject? {
        var seen: Set<ObjectIdentifier> = []
        func visit(_ node: NSObject) -> NSObject? {
            guard seen.count < 4096, seen.insert(ObjectIdentifier(node)).inserted else { return nil }
            let id = NSSelectorFromString("accessibilityIdentifier")
            if node.responds(to: id), node.perform(id)?.takeUnretainedValue() as? String == identifier { return node }
            let selector = NSSelectorFromString("accessibilityChildren")
            if node.responds(to: selector), let children = node.perform(selector)?.takeUnretainedValue() as? [NSObject] {
                for child in children { if let found = visit(child) { return found } }
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
    func testActualSearchCancelAndFailureRetryButtons() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let backend = SearchBackend(), entered = expectation(description: "owned active search")
        let release = DispatchSemaphore(value: 0); defer { release.signal() }
        backend.action = { _, _, _, _, _ in entered.fulfill(); _ = release.wait(timeout: .now() + 5); return true }
        let model = FilesPanelModel(backend: backend)
        let view = NSHostingView(rootView: FilesPanelView(panel: model, transfers: TransferManager(pump: CoreEventPump())))
        let window = SearchTestWindow(contentRect: NSRect(x: 0, y: 0, width: 680, height: 460), styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = view; TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        model.search("owned")
        await fulfillment(of: [entered], timeout: 3)
        for _ in 0..<150 {
            if node(view, "files-search-cancel") != nil { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        let pressedCancel = press(node(view, "files-search-cancel"))
        let cancelled = model.searchMode == .none
        release.signal()
        backend.action = { _, _, _, hit, _ in
            _ = hit(FileEntry(path: "/root/owned", name: "owned", kind: .file))
            throw FileSearchError.incomplete("owned UI failure")
        }
        model.search("owned", maxDepth: 2)
        for _ in 0..<150 {
            if node(view, "files-search-retry") != nil { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        let visibleFailure = node(view, "files-search-incomplete") != nil && model.searchResults.count == 1
        backend.action = { pattern, depth, _, _, _ in
            guard pattern == "owned", depth == 2 else { throw FileSearchError.incomplete("retry changed scope") }
            return false
        }
        let pressedRetry = press(node(view, "files-search-retry"))
        for _ in 0..<150 {
            if model.searchMode == .done(usedExec: false, hits: 0) { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertTrue(pressedCancel && cancelled && visibleFailure && pressedRetry && model.searchMode == .done(usedExec: false, hits: 0) && model.errorText == nil && node(view, "files-search-incomplete") == nil)
    }
}
