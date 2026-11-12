import AppKit
import CoreGraphics
import GhostermAppModel
import ImageIO
import SwiftUI
import XCTest
import WebKit
@testable import GhostermApp

final class PluginResourceUITests: XCTestCase {
    func testCancelledPreviewDecodeDiscardsLateResult() async {
        let gate = DecodeGate()
        let decode = Task {
            await PluginResourceDecodeTask.run { await gate.decode() }
        }
        await gate.waitUntilStarted()
        decode.cancel()
        await gate.finish(with: "stale document")
        let result = await decode.value
        let workerWasCancelled = await gate.workerObservedCancellation()
        XCTAssertNil(result, "a late decode must not update a replaced preview")
        XCTAssertTrue(workerWasCancelled, "the detached decode must receive cancellation")
    }

    func testResourceSelectionReconcilesAppendRemovalAndRunStop() {
        let first = resource("first")
        let second = resource("second")
        var selection = PluginResourceSelection()

        selection.reconcile([first.resourceID, second.resourceID])
        XCTAssertEqual(selection.resourceID, first.resourceID)
        selection.select(second.resourceID, available: [first.resourceID, second.resourceID])
        XCTAssertEqual(selection.resourceID, second.resourceID)
        selection.reconcile([first.resourceID, second.resourceID, "third"])
        XCTAssertEqual(selection.resourceID, second.resourceID, "arrival should preserve the user's selection")
        selection.reconcile([first.resourceID, "third"])
        XCTAssertEqual(selection.resourceID, first.resourceID, "removing the selected resource should select a live item")
        selection.select("stale", available: [first.resourceID])
        XCTAssertEqual(selection.resourceID, first.resourceID, "a late stale selection must be ignored")
        selection.reconcile([])
        XCTAssertNil(selection.resourceID, "stopping a run must clear its resource selection")
    }

    @MainActor
    func testResourceBrowserRendersASelectedVerifiedResource() throws {
        let item = resource("first")
        let view = PluginResourceBrowser(resources: [item],
                                         selection: .constant(PluginResourceSelection(resourceID: item.resourceID)))
        let host = NSHostingView(rootView: view)
        host.frame = NSRect(x: 0, y: 0, width: 720, height: 420)
        host.layoutSubtreeIfNeeded()
        XCTAssertGreaterThan(host.fittingSize.width, 0)
        XCTAssertGreaterThan(host.fittingSize.height, 0)
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        var visible = 0
        for y in stride(from: 0, to: bitmap.pixelsHigh, by: 3) {
            for x in stride(from: 0, to: bitmap.pixelsWide, by: 3) {
                guard let color = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                if color.redComponent < 0.9 || color.greenComponent < 0.9 || color.blueComponent < 0.9 {
                    visible += 1
                }
            }
        }
        XCTAssertGreaterThan(visible, 20, "selected resource and preview should render nonempty content")
    }

    @MainActor
    func testMarkdownPreviewDrawsAnExplicitImageSnapshot() async throws {
        let markdownBytes = Data("Before ![red sample](images/sample.png) after".utf8)
        let imageBytes = try solidRedPNG()
        let markdown = resource("markdown", path: "site/index.md", mime: "text/markdown",
                                bytes: markdownBytes)
        let image = resource("sample", path: "site/images/sample.png", mime: "image/png",
                             bytes: imageBytes)
        let view = PluginResourceBrowser(resources: [markdown, image],
            selection: .constant(PluginResourceSelection(resourceID: markdown.resourceID)))
        let host = NSHostingView(rootView: view)
        host.frame = NSRect(x: 0, y: 0, width: 720, height: 420)

        for _ in 0..<40 {
            host.layoutSubtreeIfNeeded()
            if let bitmap = host.bitmapImageRepForCachingDisplay(in: host.bounds) {
                host.cacheDisplay(in: host.bounds, to: bitmap)
                if redPixelCount(in: bitmap) > 10 { return }
            }
            try await Task.sleep(for: .milliseconds(50))
        }
        XCTFail("the explicitly received Markdown image should render in the selected preview")
    }

    func testHTMLAssetResolverServesOnlyExplicitSameDirectoryBundleResources() throws {
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("<img src='images/logo.png'>".utf8))
        let stylesheet = resource("css", path: "site/styles/site.css", mime: "text/css", bytes: Data("body { color: red }".utf8))
        let image = resource("image", path: "site/images/logo.png", mime: "image/png", bytes: Data([1, 2, 3]))
        let siblingEscape = resource("escape", path: "site2/images/logo.png", mime: "image/png", bytes: Data([4]))
        let nextDocument = resource("next", path: "site/next.html", mime: "text/html", bytes: Data("next".utf8))
        let laterAsset = resource("later", path: "site/later.png", mime: "image/png", bytes: Data([5]))
        let resources = [main, stylesheet, image, siblingEscape, nextDocument, laterAsset]

        XCTAssertEqual(PluginResourcePreviewAssets.after(main, in: resources).map(\.resourceID),
                       [stylesheet.resourceID, image.resourceID])
        let resolver = PluginResourceHTMLAssetResolver(document: main, resources: resources)
        let cssURL = try XCTUnwrap(URL(string: "ghosterm-resource://preview/site/styles/site.css"))
        XCTAssertEqual(resolver.resolve(cssURL)?.mime, "text/css")
        XCTAssertEqual(resolver.resolve(cssURL)?.data, stylesheet.bytes)
        let imageURL = try XCTUnwrap(URL(string: "ghosterm-resource://preview/site/images/logo.png"))
        XCTAssertEqual(resolver.resolve(imageURL)?.data, image.bytes)
        XCTAssertNil(resolver.resolve(URL(string: "https://example.invalid/site/images/logo.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://other/site/images/logo.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site2/images/logo.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/later.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/../site2/images/logo.png")!))
        XCTAssertNil(PluginResourceHTMLAssetResolver.documentURL("site/../index.html"))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/blocked.svg")!))
    }

    func testMarkdownImageResolvesOnlyToAnExplicitScopedSnapshot() throws {
        let source = Data("![logo](images/logo.png)".utf8)
        let main = resource("markdown", path: "site/index.md", mime: "text/markdown", bytes: source)
        let imageData = Data([1, 2, 3])
        let image = resource("logo", path: "site/images/logo.png", mime: "image/png", bytes: imageData)
        let markdown = try XCTUnwrap(PluginResourceMarkdownDecoder.decode(
            source, mime: main.mime, displayName: main.displayName,
            baseURL: PluginResourceHTMLAssetResolver.documentURL(main.displayName)))
        let imageURL = try XCTUnwrap(markdown.runs.first(where: { $0.imageURL != nil })?.imageURL?.absoluteURL)
        let resolver = PluginResourceHTMLAssetResolver(document: main, resources: [main, image])

        XCTAssertEqual(resolver.resolve(imageURL)?.data, imageData)
        XCTAssertEqual(resolver.resolve(imageURL)?.mime, "image/png")
        XCTAssertNil(resolver.resolve(URL(string: "https://example.invalid/logo.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/other.png")!))
    }

    @MainActor
    func testHTMLSchemeHandlerReturnsTheVerifiedSnapshotBytes() throws {
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("<img src='logo.png'>".utf8))
        let image = resource("image", path: "site/logo.png", mime: "image/png", bytes: Data([9, 8, 7]))
        let handler = PluginResourceHTMLSchemeHandler(document: main, resources: [main, image])
        let request = URLRequest(url: try XCTUnwrap(URL(string: "ghosterm-resource://preview/site/logo.png")))
        let task = CapturedSchemeTask(request: request)

        handler.webView(WKWebView(frame: .zero), start: task)

        XCTAssertEqual(task.response?.mimeType, "image/png")
        XCTAssertEqual(task.data, image.bytes)
        XCTAssertTrue(task.finished)
        XCTAssertNil(task.error)
    }

    private func resource(_ id: String) -> ExtensionController.ReceivedResource {
        resource(id, path: "\(id).txt", mime: "text/plain", bytes: Data("preview \(id)".utf8))
    }

    private func resource(_ id: String, path: String, mime: String,
                          bytes: Data) -> ExtensionController.ReceivedResource {
        return ExtensionController.ReceivedResource(resourceID: id,
            sha256: String(repeating: "a", count: 64), mime: mime,
            displayName: path, size: UInt64(bytes.count), bytes: bytes)
    }

    private func solidRedPNG() throws -> Data {
        let pixels = Data((0..<16 * 16).flatMap { _ in [UInt8(0xff), 0, 0, 0xff] })
        let provider = try XCTUnwrap(CGDataProvider(data: pixels as CFData))
        let image = try XCTUnwrap(CGImage(width: 16, height: 16, bitsPerComponent: 8,
            bitsPerPixel: 32, bytesPerRow: 64, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue), provider: provider,
            decode: nil, shouldInterpolate: false, intent: .defaultIntent))
        let encoded = NSMutableData()
        let destination = try XCTUnwrap(CGImageDestinationCreateWithData(encoded, "public.png" as CFString, 1, nil))
        CGImageDestinationAddImage(destination, image, nil)
        XCTAssertTrue(CGImageDestinationFinalize(destination))
        return encoded as Data
    }

    private func redPixelCount(in bitmap: NSBitmapImageRep) -> Int {
        var count = 0
        for y in stride(from: 0, to: bitmap.pixelsHigh, by: 2) {
            for x in stride(from: 0, to: bitmap.pixelsWide, by: 2) {
                guard let color = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                if color.redComponent > 0.8 && color.greenComponent < 0.3 && color.blueComponent < 0.3 {
                    count += 1
                }
            }
        }
        return count
    }

    private final class CapturedSchemeTask: NSObject, WKURLSchemeTask {
        let request: URLRequest
        private(set) var response: URLResponse?
        private(set) var data = Data()
        private(set) var finished = false
        private(set) var error: Error?

        init(request: URLRequest) { self.request = request }
        func didReceive(_ response: URLResponse) { self.response = response }
        func didReceive(_ data: Data) { self.data.append(data) }
        func didFinish() { finished = true }
        func didFailWithError(_ error: Error) { self.error = error }
    }

    private actor DecodeGate {
        private var started = false
        private var startWaiter: CheckedContinuation<Void, Never>?
        private var resultWaiter: CheckedContinuation<String, Never>?
        private var cancelledAtReturn = false

        func decode() async -> String {
            started = true
            startWaiter?.resume()
            startWaiter = nil
            let value = await withCheckedContinuation { resultWaiter = $0 }
            cancelledAtReturn = Task.isCancelled
            return value
        }

        func waitUntilStarted() async {
            guard !started else { return }
            await withCheckedContinuation { startWaiter = $0 }
        }

        func finish(with value: String) {
            resultWaiter?.resume(returning: value)
            resultWaiter = nil
        }

        func workerObservedCancellation() -> Bool { cancelledAtReturn }
    }
}
