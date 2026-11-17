import Cghosterm
import Foundation

/// Passive native observer. A hint is neither an authenticated target nor a
/// live IPC observation. Caller serializes this owner with transport IO.
public final class ExtensionDiscovery {
    public struct Offer: Sendable, Equatable {
        public let directory, instance, clientDigest: String
        public let sequence: UInt64
    }
    private var raw: OpaquePointer?
    public init() throws {
        let status = gt_ext_discovery_open(&raw)
        guard status == GT_OK else { throw GhostermError.status(Int32(status.rawValue), "前台发现观察器不可用") }
    }
    public func feed(_ bytes: UnsafeRawBufferPointer, now: UInt64) throws {
        var at = 0
        repeat {
            let count = min(65536, bytes.count - at)
            let status = gt_ext_discovery_feed(raw, bytes.baseAddress?.advanced(by: at), count, now)
            guard status == GT_OK else { throw GhostermError.status(Int32(status.rawValue), "前台发现观察失败") }
            at += count
        } while at < bytes.count
    }
    public func poll() throws -> (Offer?, UInt32) {
        var value = gt_ext_discovery_snapshot_t()
        let status = gt_ext_discovery_poll(raw, &value)
        guard status == GT_OK else { throw GhostermError.status(Int32(status.rawValue), "前台发现快照不可用") }
        func text<T>(_ buffer: T) -> String {
            withUnsafeBytes(of: buffer) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        }
        let offer = value.available ? Offer(directory: text(value.directory), instance: text(value.instance),
            clientDigest: text(value.client_digest), sequence: value.sequence) : nil
        return (offer, value.skipped)
    }
    deinit { gt_ext_discovery_close(&raw) }
}
