import Cghosterm
import Foundation

/// Owns the native source; Core pins Session independently of Swift ARC. A
/// dropped Session can stop its transport while the source safely observes loss
/// and releases. No strong Source/Session cycle or access to a transfer handle.
public final class ExtensionFileSource {
    public enum Status: UInt32 {
        case ok, wait, invalid, denied, unsupported, io, limit, changed, cancelled, busy, unknown, closed, notFound
        internal init(_ value: gt_ext_source_status_t) { self = Status(rawValue: value.rawValue) ?? .io }
    }
    public enum Phase: UInt32 { case hashing, ready, sending, receipt, cancelling, succeeded, failed, stopped, uncertain }
    public struct Admission { public let status: Status, source: ExtensionFileSource? }
    public struct Transfer {
        public let state: ExtensionSession.TransferState, confirmed: Bool, cancelRequested: Bool
        public let accepted: UInt64, resourceID: String, code: String, generation: UInt64
    }
    public struct Info {
        public let phase: Phase, error: Status, cleanupError: Status, closed: Bool, hasTransfer: Bool
        public let hashed: UInt64, readBytes: UInt64, accepted: UInt64
        public let descriptor: ExtensionResourceStore.Descriptor, transfer: Transfer
    }
    private let owner: DispatchQueue
    private var raw: OpaquePointer?
    internal init(owner: DispatchQueue) { self.owner = owner }
    internal func adopt(_ value: OpaquePointer) { raw = value }
    private func pointer() throws -> OpaquePointer {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard let raw else { throw GhostermError.failed("源文件句柄已释放") }; return raw
    }
    public func step() throws -> Phase { Phase(rawValue: gt_ext_source_step(try pointer()).rawValue) ?? .failed }
    public func send() throws -> Status { Status(gt_ext_source_send(try pointer())) }
    public func cancel() throws { gt_ext_source_cancel(try pointer()) }
    public func info() throws -> Info {
        var v = gt_ext_source_info_t(); guard gt_ext_source_info(try pointer(), &v) else { throw GhostermError.failed("源文件状态无效") }
        let d = v.descriptor
        return Info(phase: Phase(rawValue: v.phase.rawValue) ?? .failed, error: Status(v.error), cleanupError: Status(v.cleanup_error), closed: v.closed, hasTransfer: v.has_transfer,
            hashed: v.hashed, readBytes: v.read_bytes, accepted: v.accepted,
            descriptor: ExtensionResourceStore.Descriptor(sha256: sourceString(d.sha256), mime: sourceString(d.mime), displayName: sourceString(d.display_name), size: d.size,
                sizeKnown: d.size_known, resourceID: sourceString(d.resource_id), generation: d.generation),
            transfer: Transfer(state: ExtensionSession.TransferState(rawValue: v.transfer_state.rawValue) ?? .unknown, confirmed: v.confirmed, cancelRequested: v.cancel_requested,
                accepted: v.transfer_accepted, resourceID: sourceString(v.resource_id), code: sourceString(v.code), generation: v.generation))
    }
    @discardableResult public func closeStep() throws -> Status {
        let status = Status(gt_ext_source_close(try pointer())); guard status == .ok else { return status }
        let destroyed = Status(gt_ext_source_destroy(&raw))
        if destroyed == .ok { reapParents() }; return destroyed
    }
    private func reapParents() {
        // Asynchronous: never recursively re-enter a Session cleanup which may
        // currently be calling source cleanup while waiting on its last pin.
        let q = owner; q.async { ExtensionSession.retryPendingClose(on: q); ExtensionResourceStore.retryPendingClose(on: q) }
    }
    public static var pendingCloseCount: Int { ExtensionSourceCleanup.count }
    public static func retryPendingClose(on owner: DispatchQueue) { ExtensionSourceCleanup.retry(owner, includeErrors: true) }
    internal static func reapDroppedSources(on owner: DispatchQueue) { ExtensionSourceCleanup.retry(owner, includeErrors: false) }
    deinit { if let raw { ExtensionSourceCleanup.enqueue(raw, owner: owner) } }
}
private func sourceString<T>(_ value: T) -> String { withUnsafeBytes(of: value) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) } }
private final class ExtensionSourceCleanup {
    private static let lock = NSLock()
    private static var entries: [UUID: ExtensionSourceCleanup] = [:]
    private let id = UUID(), owner: DispatchQueue
    private var raw: OpaquePointer?
    private var requiresExplicitRetry = false
    init(_ raw: OpaquePointer, owner: DispatchQueue) { self.raw = raw; self.owner = owner }
    static var count: Int { lock.lock(); defer { lock.unlock() }; return entries.count }
    static func enqueue(_ raw: OpaquePointer, owner: DispatchQueue) {
        let item = ExtensionSourceCleanup(raw, owner: owner); lock.lock(); entries[item.id] = item; lock.unlock(); owner.async { item.run() }
    }
    static func retry(_ owner: DispatchQueue, includeErrors: Bool) {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        lock.lock(); let values = entries.values.filter { $0.owner === owner }; lock.unlock(); values.filter { includeErrors || !$0.requiresExplicitRetry }.forEach { $0.run() }
    }
    private func run() {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard let raw else { return }; gt_ext_source_cancel(raw)
        let close = gt_ext_source_close(raw)
        if close == GT_EXT_SOURCE_IO { requiresExplicitRetry = true; return }
        guard close == GT_EXT_SOURCE_OK else { return }
        let destroy = gt_ext_source_destroy(&self.raw)
        if destroy == GT_EXT_SOURCE_IO { requiresExplicitRetry = true; return }
        guard destroy == GT_EXT_SOURCE_OK else { return }
        Self.lock.lock(); Self.entries.removeValue(forKey: id); Self.lock.unlock()
        let q = owner; q.async { ExtensionSession.retryPendingClose(on: q); ExtensionResourceStore.retryPendingClose(on: q) }
    }
}
