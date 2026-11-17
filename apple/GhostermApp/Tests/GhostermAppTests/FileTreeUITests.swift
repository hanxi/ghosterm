import AppKit
import GhostermCore
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
private final class FileTreeTestWindow: NSWindow {
    override var occlusionState: NSWindow.OcclusionState { [.visible] }
    override func constrainFrameRect(_ rect: NSRect, to screen: NSScreen?) -> NSRect {
        var result = rect; result.origin = TestWindows.offscreenOrigin; return result
    }
}

@MainActor
final class FileTreeUITests: XCTestCase {
    private func nodes(_ view: NSView) -> [NSObject] {
        var found: [NSObject] = [], seen: Set<ObjectIdentifier> = []
        func visit(_ node: NSObject) {
            guard found.count < 4096, seen.insert(ObjectIdentifier(node)).inserted else { return }
            found.append(node)
            let selector = NSSelectorFromString("accessibilityChildren")
            if node.responds(to: selector), let children = node.perform(selector)?.takeUnretainedValue() as? [NSObject] {
                for child in children { visit(child) }
            }
        }
        view.layoutSubtreeIfNeeded(); visit(view); return found
    }
    private func node(_ view: NSView, _ id: String) -> NSObject? {
        nodes(view).first {
            let selector = NSSelectorFromString("accessibilityIdentifier")
            return $0.responds(to: selector) && $0.perform(selector)?.takeUnretainedValue() as? String == id
        }
    }
    private func press(_ node: NSObject) -> Bool {
        let selector = NSSelectorFromString("accessibilityPerformPress")
        guard node.responds(to: selector), let implementation = node.method(for: selector) else { return false }
        typealias Action = @convention(c) (AnyObject, Selector) -> Bool
        return unsafeBitCast(implementation, to: Action.self)(node, selector)
    }
    func testActualTreeShowsFailureAndRetriesThroughItsButton() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let backend = NavigationBackend(); backend.failNext("/root")
        let model = FilesPanelModel(backend: backend); model.viewMode = .tree
        let manager = TransferManager(pump: CoreEventPump())
        let view = NSHostingView(rootView: FilesPanelView(panel: model, transfers: manager))
        let window = FileTreeTestWindow(contentRect: NSRect(x: 0, y: 0, width: 680, height: 460), styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = view; TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        for _ in 0..<150 {
            if node(view, "files-tree-retry-/root") != nil { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        let retry = node(view, "files-tree-retry-/root")
        let errorVisible = node(view, "files-tree-error-/root") != nil && model.treeChildren["/root"] == nil
        if let retry { _ = press(retry) }
        for _ in 0..<150 {
            if model.treeChildren["/root"]?.count == 1 && node(view, "files-tree-retry-/root") == nil { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        backend.failNext("/root/entry")
        model.toggleTree("/root/entry")
        for _ in 0..<150 {
            if node(view, "files-tree-retry-/root/entry") != nil { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        let childRetry = node(view, "files-tree-retry-/root/entry")
        let childErrorVisible = node(view, "files-tree-error-/root/entry") != nil
        if let childRetry { _ = press(childRetry) }
        for _ in 0..<150 {
            if model.treeChildren["/root/entry"]?.count == 1 { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertTrue(errorVisible && retry != nil && model.treeErrors["/root"] == nil &&
                      model.treeChildren["/root"]?.count == 1 && backend.calls("/root") == 2 &&
                      childErrorVisible && childRetry != nil && model.treeChildren["/root/entry"]?.count == 1 && backend.calls("/root/entry") == 2,
                      "actual tree retry: errorVisible=\(errorVisible), retry=\(retry != nil), errors=\(model.treeErrors.count), children=\(model.treeChildren["/root"]?.count ?? -1), reads=\(backend.calls("/root")), childError=\(childErrorVisible), childRetry=\(childRetry != nil), childReads=\(backend.calls("/root/entry")), AX=\(nodes(view).count)")
    }
}
