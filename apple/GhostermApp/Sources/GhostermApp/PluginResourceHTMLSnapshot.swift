import CoreGraphics
import CryptoKit
import Foundation
import GhostermAppModel
import ImageIO
import UniformTypeIdentifiers

/// WebKit receives only a prepared, immutable document and bounded static
/// raster frames. Original ResourceStore receipts remain unchanged.
struct PluginResourceHTMLSnapshot: Sendable {
    struct Input: Hashable, Sendable {
        let id: Data, path: Data, mime: Data, sha256: Data
        let size: UInt64
        init(_ value: ExtensionController.ReceivedResource) {
            id = Data(value.resourceID.utf8); path = Data(value.displayName.utf8)
            mime = Data(value.mime.utf8); sha256 = Data(value.sha256.utf8); size = value.size
        }
    }
    static let maximumPixels: UInt64 = 4_194_304
    static let maximumDimension = 2048
    // Four 8-bit channels plus bounded PNG/zlib framing. This is a local derived
    // view buffer; the received resource limit remains one MiB.
    static let maximumEncodedBytes = 16_842_752
    let resource: ExtensionController.ReceivedResource
    let document: String
    let resolver: PluginResourceHTMLAssetResolver
    let inputs: [Input]
    let fingerprint: Data

    private init(resource: ExtensionController.ReceivedResource, document: String,
                 resolver: PluginResourceHTMLAssetResolver, inputs: [Input], fingerprint: Data) {
        self.resource = resource; self.document = document; self.resolver = resolver
        self.inputs = inputs; self.fingerprint = fingerprint
    }

    static func identity(document: ExtensionController.ReceivedResource,
                         resources: [ExtensionController.ReceivedResource]) -> [Input] {
        [Input(document)] + resources.map(Input.init)
    }

    static func prepare(document resource: ExtensionController.ReceivedResource,
                        resources: [ExtensionController.ReceivedResource]) async -> Self? {
        await PluginResourceDecodeTask.run {
            capture(document: resource, resources: resources)
        } ?? nil
    }

    private static func capture(document resource: ExtensionController.ReceivedResource,
                                resources: [ExtensionController.ReceivedResource]) -> Self? {
        guard !Task.isCancelled,
              PluginResourceHTMLAssetResolver.documentURL(resource.displayName) != nil,
              resources.filter({ $0.resourceID.utf8.elementsEqual(resource.resourceID.utf8) }).count == 1,
              let document = PluginResourceHTMLDecoder.document(resource.bytes, mime: resource.mime) else { return nil }
        var remaining = maximumPixels
        let resolver = PluginResourceHTMLAssetResolver(document: resource, resources: resources) { item in
            guard !Task.isCancelled else { return nil }
            let mime = item.mime.split(separator: ";", maxSplits: 1).first?
                .trimmingCharacters(in: .whitespacesAndNewlines).lowercased() ?? ""
            if mime == "text/css" { return .init(data: item.bytes, mime: mime) }
            guard let source = CGImageSourceCreateWithData(item.bytes as CFData,
                    [kCGImageSourceShouldCache: false] as CFDictionary),
                  let actual = CGImageSourceGetType(source).flatMap({ UTType($0 as String) }),
                  let expected = ["image/png": UTType.png, "image/jpeg": .jpeg,
                                  "image/gif": .gif, "image/webp": .webP][mime], actual.conforms(to: expected),
                  let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
                  let width = properties[kCGImagePropertyPixelWidth] as? UInt64,
                  let height = properties[kCGImagePropertyPixelHeight] as? UInt64,
                  width > 0, height > 0, width <= UInt64(maximumDimension), height <= UInt64(maximumDimension),
                  width <= remaining / height else { return nil }
            remaining -= width * height
            guard let image = PluginResourceImageDecoder.decode(item.bytes, mime: mime,
                    maxPixels: width * height, maxThumbnailDimension: maximumDimension),
                  !Task.isCancelled, let bytes = staticPNG(image) else { return nil }
            return .init(data: bytes, mime: "image/png")
        }
        guard !Task.isCancelled else { return nil }
        var digest = SHA256()
        for bytes in [Data(resource.resourceID.utf8), Data(resource.displayName.utf8), Data(document.utf8),
                      resolver.fingerprint, Data([resolver.omittedAttachments ? 1 : 0])] {
            var length = UInt64(bytes.count).bigEndian
            withUnsafeBytes(of: &length) { digest.update(bufferPointer: $0) }
            digest.update(data: bytes)
        }
        return Self(resource: resource, document: document, resolver: resolver,
                    inputs: identity(document: resource, resources: resources), fingerprint: Data(digest.finalize()))
    }

    private static func staticPNG(_ source: CGImage) -> Data? {
        guard let color = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(data: nil, width: source.width, height: source.height,
                bitsPerComponent: 8, bytesPerRow: 0, space: color,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        context.draw(source, in: CGRect(x: 0, y: 0, width: source.width, height: source.height))
        guard let image = context.makeImage() else { return nil }
        let output = PluginHTMLPNGOutput(limit: maximumEncodedBytes)
        let reference = Unmanaged.passRetained(output)
        var callbacks = CGDataConsumerCallbacks(putBytes: { info, bytes, count in
            guard let info else { return 0 }
            return Unmanaged<PluginHTMLPNGOutput>.fromOpaque(info).takeUnretainedValue().write(bytes, count: count)
        }, releaseConsumer: { info in
            if let info { Unmanaged<PluginHTMLPNGOutput>.fromOpaque(info).release() }
        })
        guard let consumer = CGDataConsumer(info: reference.toOpaque(), cbks: &callbacks) else {
            reference.release(); return nil
        }
        guard let destination = CGImageDestinationCreateWithDataConsumer(consumer, UTType.png.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination), !output.exceeded else { return nil }
        return output.data
    }
}

/// Refuse before growing the output buffer; ImageIO can report a short write.
final class PluginHTMLPNGOutput {
    private let limit: Int
    private(set) var data = Data()
    private(set) var exceeded = false
    init(limit: Int) { self.limit = max(0, limit) }
    func write(_ bytes: UnsafeRawPointer, count: Int) -> Int {
        guard !exceeded, count >= 0, count <= limit - data.count else { exceeded = true; return 0 }
        data.append(bytes.assumingMemoryBound(to: UInt8.self), count: count)
        return count
    }
}
