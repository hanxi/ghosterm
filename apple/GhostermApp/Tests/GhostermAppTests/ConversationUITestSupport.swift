import AppKit
import SwiftUI
import Vision
import XCTest

/// Local bitmap/OCR clicks on transparent offscreen production controls.
@MainActor
final class ConversationUIProbe {
    private final class FixtureWindow: NSWindow {
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

    struct FixtureFailure: Error { let message: String }
    let root: NSView
    let window: NSWindow
    init<Content: View>(root: NSHostingView<Content>, width: CGFloat = 1000, height: CGFloat = 2000) {
        TestWindows.enterAutomationMode()
        root.sizingOptions = []
        self.root = root
        root.frame = NSRect(x: 0, y: 0, width: width, height: height)
        window = FixtureWindow(contentRect: root.frame, styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = root
        TestWindows.presentOffscreenAsKey(window)
    }
    func close() { window.orderOut(nil); window.close() }
    private func labelBox(_ observation: VNRecognizedTextObservation, label: String) -> CGRect? {
        guard let candidate = observation.topCandidates(1).first else { return nil }
        let text = candidate.string
        let positions = text.indices.filter { !text[$0].isWhitespace }
        let compact = String(positions.map { text[$0] })
        guard let range = compact.range(of: label) else { return nil }
        let lower = compact.distance(from: compact.startIndex, to: range.lowerBound)
        let upper = compact.distance(from: compact.startIndex, to: range.upperBound)
        guard lower < upper else { return nil }
        let original = positions[lower]..<text.index(after: positions[upper - 1])
        // Adjacent Native buttons may share one OCR observation. The character
        // box selects the actual label rather than the centre between buttons.
        return (try? candidate.boundingBox(for: original))?.boundingBox
    }

    private func buttonPoint(_ root: NSView, label: String, occurrence: Int?) throws -> NSPoint? {
        let window = try XCTUnwrap(root.window)
        window.alphaValue = 0
        guard NSScreen.screens.allSatisfy({ !$0.frame.intersects(window.frame) }) else {
            throw FixtureFailure(message: "Automation window must stay outside every screen: \(window.frame)")
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        root.layoutSubtreeIfNeeded()
        guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(root.bounds.width * 2),
            pixelsHigh: Int(root.bounds.height * 2), bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
            isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { return nil }
        bitmap.size = root.bounds.size
        root.cacheDisplay(in: root.bounds, to: bitmap)
        guard let image = bitmap.cgImage else { return nil }
        let request = VNRecognizeTextRequest()
        request.recognitionLevel = .accurate
        request.minimumTextHeight = 0.002
        request.usesLanguageCorrection = false
        request.recognitionLanguages = ["zh-Hans", "en-US"]
        request.customWords = [label]
        try VNImageRequestHandler(cgImage: image).perform([request])
        let matches = (request.results ?? []).compactMap { labelBox($0, label: label) }
            .sorted { $0.midY > $1.midY }
        guard occurrence == nil, let match = matches.first else {
            var points: [NSPoint] = []
            // Vision downsizes very tall full-window captures. Search bounded
            // overlapping image tiles before declaring a small control absent.
            for top in stride(from: 0, to: image.height, by: 768) {
                let crop = CGRect(x: 0, y: top, width: image.width, height: min(1024, image.height - top))
                guard let tile = image.cropping(to: crop) else { continue }
                let local = VNRecognizeTextRequest()
                local.recognitionLevel = .accurate; local.minimumTextHeight = 0.002
                local.usesLanguageCorrection = false; local.recognitionLanguages = ["zh-Hans", "en-US"]; local.customWords = [label]
                try VNImageRequestHandler(cgImage: tile).perform([local])
                for box in (local.results ?? []).compactMap({ labelBox($0, label: label) }) {
                    let x = box.midX * root.bounds.width
                    let fromTop = crop.minY + (1 - box.midY) * crop.height
                    let y = fromTop / CGFloat(image.height) * root.bounds.height
                    let point = NSPoint(x: x, y: root.isFlipped ? y : root.bounds.height - y)
                    if occurrence == nil { return point }
                    if !points.contains(where: { abs($0.x - point.x) < 20 && abs($0.y - point.y) < 6 }) { points.append(point) }
                }
            }
            if let occurrence {
                points.sort { root.isFlipped ? $0.y < $1.y : $0.y > $1.y }
                if points.indices.contains(occurrence) { return points[occurrence] }
            }
            var repository = URL(fileURLWithPath: #filePath)
            for _ in 0..<5 { repository.deleteLastPathComponent() }
            repository = repository.appendingPathComponent("build").resolvingSymlinksInPath().deletingLastPathComponent()
            if let data = bitmap.representation(using: .png, properties: [:]) {
                try? data.write(to: repository.appendingPathComponent("tmp/claude-chat-ui.png"))
            }
            let labels = (request.results ?? []).compactMap { $0.topCandidates(1).first?.string }.joined(separator: "\n")
            try? Data(labels.utf8).write(to: repository.appendingPathComponent("tmp/claude-chat-ui-ocr.txt"))
            return nil
        }
        let box = match
        return NSPoint(x: box.midX * root.bounds.width,
                       y: (root.isFlipped ? 1 - box.midY : box.midY) * root.bounds.height)
    }


    func press(_ label: String, occurrence: Int? = nil) async throws {
        var point: NSPoint?
        let deadline = ProcessInfo.processInfo.systemUptime + 8
        while point == nil && ProcessInfo.processInfo.systemUptime < deadline {
            point = try buttonPoint(root, label: label.replacingOccurrences(of: " ", with: ""), occurrence: occurrence)
            if point == nil { try await Task.sleep(nanoseconds: 50_000_000) }
        }
        let location = root.convert(try XCTUnwrap(point, "Native control missing: \(label)"), to: nil)
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            let event = try XCTUnwrap(NSEvent.mouseEvent(with: type, location: location, modifierFlags: [],
                timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: window.windowNumber,
                context: nil, eventNumber: 1, clickCount: 1, pressure: type == .leftMouseDown ? 1 : 0))
            window.sendEvent(event)
        }
    }
    func textViews() -> [NSTextView] {
        func walk(_ view: NSView) -> [NSTextView] { (view as? NSTextView).map { [$0] } ?? view.subviews.flatMap(walk) }
        root.layoutSubtreeIfNeeded()
        return walk(root)
    }
}
