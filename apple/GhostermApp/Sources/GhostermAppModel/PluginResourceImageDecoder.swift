import CoreGraphics
import Foundation
import ImageIO

/// Bounded decoder for static images received through an Extension Session.
public enum PluginResourceImageDecoder {
    private static let supportedMIME = Set([
        "image/png", "image/jpeg", "image/gif", "image/heic", "image/tiff", "image/webp",
    ])

    /// Reads only the first frame, rejects oversized source dimensions before decoding,
    /// and returns a thumbnail no larger than `maxThumbnailDimension` on either axis.
    public static func decode(_ data: Data,
                              mime: String,
                              maxPixels: UInt64 = 16_777_216,
                              maxThumbnailDimension: Int = 2048) -> CGImage? {
        guard !data.isEmpty, supportedMIME.contains(mime.lowercased()),
              maxPixels > 0, maxThumbnailDimension > 0,
              let source = CGImageSourceCreateWithData(data as CFData, nil),
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = properties[kCGImagePropertyPixelWidth] as? UInt64,
              let height = properties[kCGImagePropertyPixelHeight] as? UInt64,
              width > 0, height > 0, width <= maxPixels / height,
              let thumbnail = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: maxThumbnailDimension,
                kCGImageSourceShouldCacheImmediately: true,
              ] as CFDictionary),
              thumbnail.width > 0, thumbnail.height > 0,
              thumbnail.width <= maxThumbnailDimension,
              thumbnail.height <= maxThumbnailDimension else {
            return nil
        }
        return thumbnail
    }
}
