import AppKit
import CoreGraphics
import Foundation
import ImageIO
import WebKit
import GhostermAppModel

@MainActor
final class CaptureDelegate: NSObject, WKNavigationDelegate {
    let coordinator: PluginResourceHTMLCoordinator
    var results: [(red: Int, blue: Int, green: Int)] = []
    var failure: String?
    var done: Bool { results.count == 2 || failure != nil }

    init(coordinator: PluginResourceHTMLCoordinator) { self.coordinator = coordinator }

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        coordinator.webView(webView, decidePolicyFor: action, decisionHandler: decisionHandler)
    }

    func webView(_ webView: WKWebView, decidePolicyFor response: WKNavigationResponse,
                 decisionHandler: @escaping (WKNavigationResponsePolicy) -> Void) {
        coordinator.webView(webView, decidePolicyFor: response, decisionHandler: decisionHandler)
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        webView.takeSnapshot(with: nil) { [weak self] image, error in
            guard let self else { return }
            if let error { failure = "snapshot: \(error)"; return }
            guard let cg = image?.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
                failure = "snapshot returned no CGImage"; return
            }
            results.append(countPixels(cg))
        }
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        failure = "navigation failed: \(error)"
    }

}

@MainActor
@main
struct Probe {
    static func main() throws {
        let app = NSApplication.shared
        _ = app.setActivationPolicy(.prohibited)
        let htmlBytes = Data("<html><head><link rel='stylesheet' href='style.css'></head><body><img width='64' height='64' src='image.png'></body></html>".utf8)
        let html = ExtensionController.ReceivedResource(resourceID: "html", sha256: String(repeating: "a", count: 64), mime: "text/html", displayName: "site/index.html", size: UInt64(htmlBytes.count), bytes: htmlBytes)
        let cssBytes = Data("body { margin: 0; background: rgb(0, 0, 255); }".utf8)
        let css = ExtensionController.ReceivedResource(resourceID: "css", sha256: String(repeating: "b", count: 64), mime: "text/css", displayName: "site/style.css", size: UInt64(cssBytes.count), bytes: cssBytes)
        let pngBytes = try solidRedPNG()
        let image = ExtensionController.ReceivedResource(resourceID: "image", sha256: String(repeating: "c", count: 64), mime: "image/png", displayName: "site/image.png", size: UInt64(pngBytes.count), bytes: pngBytes)
        let resources = [html, css, image]
        let document = try PluginResourceHTMLDecoder.document(htmlBytes, mime: html.mime).unwrap()
        let coordinator = PluginResourceHTMLCoordinator(resource: html, resources: resources)
        let configuration = PluginResourceHTMLWebViewConfiguration.make(schemeHandler: coordinator.schemeHandler)
        let webView = WKWebView(frame: NSRect(x: 0, y: 0, width: 240, height: 180), configuration: configuration)
        webView.setValue(false, forKey: "drawsBackground")
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 240, height: 180), styleMask: .borderless, backing: .buffered, defer: false)
        let delegate = CaptureDelegate(coordinator: coordinator)
        webView.navigationDelegate = delegate
        webView.uiDelegate = coordinator
        window.orderOut(nil)
        window.contentView = webView
        coordinator.update(document: document, resource: html, resources: resources, in: webView)
        let deadline = Date().addingTimeInterval(10)
        while delegate.results.isEmpty && delegate.failure == nil && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        guard delegate.failure == nil, delegate.results.count == 1 else {
            throw ProbeError.failed(delegate.failure ?? "timed out waiting for initial snapshot")
        }
        let greenCSSBytes = Data("body { margin: 0; background: rgb(0, 255, 0); }".utf8)
        let greenCSS = ExtensionController.ReceivedResource(resourceID: "css",
            sha256: String(repeating: "d", count: 64), mime: "text/css",
            displayName: "site/style.css", size: UInt64(greenCSSBytes.count), bytes: greenCSSBytes)
        coordinator.update(document: document, resource: html,
                           resources: [html, greenCSS, image], in: webView)
        while !delegate.done && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        webView.stopLoading()
        webView.navigationDelegate = nil
        webView.uiDelegate = nil
        window.contentView = nil
        window.close()
        if let failure = delegate.failure { throw ProbeError.failed(failure) }
        guard delegate.results.count == 2 else { throw ProbeError.failed("timed out waiting for refreshed snapshot") }
        let initial = delegate.results[0]
        let refreshed = delegate.results[1]
        print("WebKit rendered custom-scheme CSS and PNG before/after resource refresh: initial red=\(initial.red), blue=\(initial.blue); refreshed red=\(refreshed.red), green=\(refreshed.green)")
        guard initial.red > 20, initial.blue > 100,
              refreshed.red > 20, refreshed.green > 100 else {
            throw ProbeError.failed("expected explicit red image and blue-to-green CSS refresh")
        }
    }
}

enum ProbeError: Error { case failed(String) }

func solidRedPNG() throws -> Data {
    let pixels = Data((0..<64 * 64).flatMap { _ in [UInt8(0xff), 0, 0, 0xff] })
    let provider = try CGDataProvider(data: pixels as CFData).unwrap()
    let image = try CGImage(width: 64, height: 64, bitsPerComponent: 8, bitsPerPixel: 32,
        bytesPerRow: 256, space: CGColorSpaceCreateDeviceRGB(),
        bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue), provider: provider,
        decode: nil, shouldInterpolate: false, intent: .defaultIntent).unwrap()
    let encoded = NSMutableData()
    let destination = try CGImageDestinationCreateWithData(encoded, "public.png" as CFString, 1, nil).unwrap()
    CGImageDestinationAddImage(destination, image, nil)
    guard CGImageDestinationFinalize(destination) else { throw ProbeError.failed("encode PNG") }
    return encoded as Data
}

func countPixels(_ image: CGImage) -> (red: Int, blue: Int, green: Int) {
    let data = NSMutableData(length: image.width * image.height * 4)!
    let context = CGContext(data: data.mutableBytes, width: image.width, height: image.height,
        bitsPerComponent: 8, bytesPerRow: image.width * 4, space: CGColorSpaceCreateDeviceRGB(),
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    context.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
    let bytes = data.bytes.assumingMemoryBound(to: UInt8.self)
    var red = 0; var blue = 0; var green = 0
    for index in 0..<(image.width * image.height) {
        let offset = index * 4
        if bytes[offset] > 180 && bytes[offset + 1] < 80 && bytes[offset + 2] < 80 { red += 1 }
        if bytes[offset + 2] > 180 && bytes[offset] < 80 && bytes[offset + 1] < 80 { blue += 1 }
        if bytes[offset + 1] > 180 && bytes[offset] < 80 && bytes[offset + 2] < 80 { green += 1 }
    }
    return (red, blue, green)
}

extension Optional {
    func unwrap(file: StaticString = #filePath, line: UInt = #line) throws -> Wrapped {
        guard let value = self else { throw ProbeError.failed("unexpected nil at \(file):\(line)") }
        return value
    }
}
