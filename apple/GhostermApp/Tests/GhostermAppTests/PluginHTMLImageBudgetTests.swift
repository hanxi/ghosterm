import AppKit
import CryptoKit
import GhostermAppModel
import ImageIO
import WebKit
import XCTest
@testable import GhostermApp

@MainActor
final class PluginHTMLImageBudgetTests: XCTestCase {
    private func image(_ width: Int, _ height: Int, green: Bool = false) throws -> CGImage {
        let pixels = Data((0..<width * height).flatMap { _ in
            green ? [UInt8(0), 255, 0, 255] : [UInt8(255), 0, 0, 255]
        })
        let provider = try XCTUnwrap(CGDataProvider(data: pixels as CFData))
        return try XCTUnwrap(CGImage(width: width, height: height, bitsPerComponent: 8,
            bitsPerPixel: 32, bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue), provider: provider,
            decode: nil, shouldInterpolate: false, intent: .defaultIntent))
    }

    private func encode(_ images: [CGImage], type: String = "public.png") throws -> Data {
        let data = NSMutableData()
        let destination = try XCTUnwrap(CGImageDestinationCreateWithData(data, type as CFString, images.count, nil))
        for image in images { CGImageDestinationAddImage(destination, image, nil) }
        XCTAssertTrue(CGImageDestinationFinalize(destination))
        return data as Data
    }

    private func resource(_ path: String, _ mime: String, _ bytes: Data) -> ExtensionController.ReceivedResource {
        ExtensionController.ReceivedResource(resourceID: path,
            sha256: SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined(),
            mime: mime, displayName: path, size: UInt64(bytes.count), bytes: bytes)
    }

    private func read(_ path: String, resources: [ExtensionController.ReceivedResource]) async throws -> Read {
        TestWindows.enterAutomationMode()
        let main = resource("site/index.html", "text/html", Data("<img src='first.png'>".utf8))
        let captured = await PluginResourceHTMLSnapshot.prepare(document: main, resources: [main] + resources)
        let handler = PluginResourceHTMLSchemeHandler(snapshot: try XCTUnwrap(captured))
        let view = WKWebView(frame: .zero, configuration: PluginResourceHTMLWebViewConfiguration.make(schemeHandler: handler))
        let task = Read(request: URLRequest(url: try XCTUnwrap(PluginResourceHTMLAssetResolver.documentURL(path))))
        handler.webView(view, start: task)
        return task
    }

    func testHTMLRejectsOversizedRasterBeforeServingItToWebKit() async throws {
        let png = try encode([image(2049, 1)])
        XCTAssertLessThan(png.count, 1_048_576)
        let result = try await read("site/first.png", resources: [resource("site/first.png", "image/png", png)])
        XCTAssertTrue(result.error != nil && result.data.isEmpty,
                      "HTML must reject source dimensions beyond the raster preview limit")
    }

    func testHTMLAppliesOneDecodedPixelBudgetToTheWholeBundle() async throws {
        let first = resource("site/first.png", "image/png", try encode([image(2048, 2048)]))
        let second = resource("site/second.png", "image/png", try encode([image(1, 1)]))
        let allowed = try await read(first.displayName, resources: [first, second])
        let rejected = try await read(second.displayName, resources: [first, second])
        XCTAssertTrue(allowed.finished && rejected.error != nil && rejected.data.isEmpty,
                      "HTML attachments must share one decoded pixel budget")
    }

    func testHTMLServesOnlyTheFirstStaticFrameOfAnAnimatedRaster() async throws {
        let gif = try encode([image(32, 32), image(32, 32, green: true)], type: "com.compuserve.gif")
        let source = try XCTUnwrap(CGImageSourceCreateWithData(gif as CFData, nil))
        XCTAssertEqual(CGImageSourceGetCount(source), 2)
        let result = try await read("site/animation.gif", resources: [resource("site/animation.gif", "image/gif", gif)])
        let served = result.data.isEmpty ? nil : CGImageSourceCreateWithData(result.data as CFData, nil)
        XCTAssertTrue(result.mime == "image/png" && served.map { CGImageSourceGetCount($0) == 1 } == true,
                      "HTML must receive a bounded static PNG instead of the original animation")
    }

    func testHTMLRejectsOversizedRasterHeightBeforeServingItToWebKit() async throws {
        let png = try encode([image(1, 2049)])
        let result = try await read("site/first.png", resources: [resource("site/first.png", "image/png", png)])
        XCTAssertTrue(result.error != nil && result.data.isEmpty,
                      "HTML must reject raster height beyond the preview limit")
    }

    func testHTMLRejectsImageBytesWhoseTypeDoesNotMatchTheDeclaredMime() async throws {
        let tiff = try encode([image(16, 16)], type: "public.tiff")
        let result = try await read("site/first.png", resources: [resource("site/first.png", "image/png", tiff)])
        XCTAssertTrue(result.error != nil && result.data.isEmpty,
                      "HTML must reject an undeclared raster container")
    }

    func testHTMLKeepsLegitimateJpegAndItsOriginalReceiptWhenDerivedPngIsLarger() async throws {
        let side = 896
        var state: UInt32 = 0x1937a5d1
        var pixels = Data(count: side * side * 4)
        pixels.withUnsafeMutableBytes { raw in
            let bytes = raw.bindMemory(to: UInt8.self)
            for index in stride(from: 0, to: bytes.count, by: 4) {
                for channel in 0..<3 {
                    state ^= state << 13; state ^= state >> 17; state ^= state << 5
                    bytes[index + channel] = UInt8(truncatingIfNeeded: state)
                }
                bytes[index + 3] = 255
            }
        }
        let provider = try XCTUnwrap(CGDataProvider(data: pixels as CFData))
        let bitmap = try XCTUnwrap(CGImage(width: side, height: side, bitsPerComponent: 8, bitsPerPixel: 32,
            bytesPerRow: side * 4, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue), provider: provider,
            decode: nil, shouldInterpolate: false, intent: .defaultIntent))
        let jpeg = try encode([bitmap], type: "public.jpeg")
        XCTAssertLessThanOrEqual(jpeg.count, 1_048_576)
        let receipt = resource("site/noise.jpg", "image/jpeg", jpeg)
        let result = try await read(receipt.displayName, resources: [receipt])
        XCTAssertTrue(result.finished && result.mime == "image/png" && result.data.count > 1_048_576 &&
                      result.data.count <= PluginResourceHTMLSnapshot.maximumEncodedBytes && receipt.bytes == jpeg,
                      "source and derived byte budgets must stay separate without dropping a valid JPEG")
    }

    func testPNGConsumerRefusesGrowthBeforeTheLimitAndKeepsFailureSticky() {
        let output = PluginHTMLPNGOutput(limit: 4)
        let bytes = Data([1, 2, 3, 4, 5])
        bytes.withUnsafeBytes { buffer in
            XCTAssertEqual(output.write(buffer.baseAddress!, count: 3), 3)
            let denied = output.write(buffer.baseAddress!, count: 2)
            let late = output.write(buffer.baseAddress!, count: 1)
            XCTAssertTrue(denied == 0 && late == 0 && output.exceeded && output.data.count == 3,
                          "PNG consumer must stop before growing beyond its budget and stay failed")
        }
    }

    func testHTMLDuplicatePathStaysUnavailableWhenTheFirstRasterWasRejected() async throws {
        let invalid = resource("site/first.png", "image/png", try encode([image(2049, 1)]))
        let valid = resource("site/first.png", "image/png", try encode([image(1, 1)]))
        let result = try await read("site/first.png", resources: [invalid, valid])
        XCTAssertTrue(result.error != nil && result.data.isEmpty,
                      "a rejected raster must not release its duplicate path for another attachment")
    }

    private final class Read: NSObject, WKURLSchemeTask {
        let request: URLRequest
        var mime: String?
        var data = Data()
        var error: Error?
        var finished = false
        init(request: URLRequest) { self.request = request }
        func didReceive(_ response: URLResponse) { mime = response.mimeType }
        func didReceive(_ bytes: Data) { data.append(bytes) }
        func didFinish() { finished = true }
        func didFailWithError(_ error: Error) { self.error = error }
    }
}
