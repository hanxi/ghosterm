import AppKit
import Foundation
import Vision

@MainActor
final class PluginTableWindow: NSWindow {
    override var occlusionState: NSWindow.OcclusionState { [.visible] }
    override func constrainFrameRect(_ frameRect: NSRect, to screen: NSScreen?) -> NSRect {
        var result = frameRect; result.origin = TestWindows.offscreenOrigin; return result
    }
    override func setFrame(_ frameRect: NSRect, display flag: Bool) {
        var result = frameRect; result.origin = TestWindows.offscreenOrigin
        super.setFrame(result, display: flag)
    }
    override func setFrame(_ frameRect: NSRect, display flag: Bool, animate: Bool) {
        var result = frameRect; result.origin = TestWindows.offscreenOrigin
        super.setFrame(result, display: flag, animate: animate)
    }
}

@MainActor
enum PluginTableRendering {
    static func elements(_ view: NSView) -> [NSObject] {
        var found: [NSObject] = [], seen = Set<ObjectIdentifier>()
        func visit(_ node: NSObject) {
            guard found.count < 4096, seen.insert(ObjectIdentifier(node)).inserted else { return }
            found.append(node)
            let selector = NSSelectorFromString("accessibilityChildren")
            if node.responds(to: selector), let children = node.perform(selector)?.takeUnretainedValue() as? [NSObject] {
                for child in children { visit(child) }
            }
        }
        visit(view); return found
    }
    static func attribute(_ node: NSObject, _ name: String) -> String? {
        let selector = NSSelectorFromString(name)
        guard node.responds(to: selector) else { return nil }
        return node.perform(selector)?.takeUnretainedValue() as? String
    }
    static func press(_ node: NSObject) -> Bool {
        // SwiftUI's AX nodes implement the public ObjC action selector without
        // declaring NSAccessibilityProtocol conformance.
        let selector = NSSelectorFromString("accessibilityPerformPress")
        guard node.responds(to: selector), let implementation = node.method(for: selector) else { return false }
        typealias Press = @convention(c) (AnyObject, Selector) -> Bool
        return unsafeBitCast(implementation, to: Press.self)(node, selector)
    }
    static func words(_ view: NSView) throws -> [String] {
        view.layoutSubtreeIfNeeded()
        let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(view.bounds.width * 2),
            pixelsHigh: Int(view.bounds.height * 2), bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
            isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        bitmap.size = view.bounds.size; view.cacheDisplay(in: view.bounds, to: bitmap)
        let request = VNRecognizeTextRequest(); request.recognitionLevel = .accurate
        request.usesLanguageCorrection = false; request.recognitionLanguages = ["en-US"]
        try VNImageRequestHandler(cgImage: bitmap.cgImage!).perform([request])
        return (request.results ?? []).compactMap { $0.topCandidates(1).first?.string }
    }
}
