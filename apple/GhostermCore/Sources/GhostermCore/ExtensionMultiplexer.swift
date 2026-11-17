import Cghosterm
import Foundation

/// Physical connection owner shared by at most eight explicitly registered
/// logical channels. Every child uses this same non-main serial queue.
public final class ExtensionMultiplexer {
    public struct Info {
        public let error: Int32, cleanupError: Int32, instances: Int
        public let received: UInt64, sent: UInt64, rejected: UInt64
        public let stopping: Bool, released: Bool
        fileprivate init(_ value: gt_ext_mux_info_t) {
            error = Int32(value.error.rawValue); cleanupError = Int32(value.cleanup_error.rawValue)
            instances = Int(value.instances); received = value.received; sent = value.sent
            rejected = value.rejected; stopping = value.stopping; released = value.released
        }
    }
    public struct ScopeInfo {
        public let connection: String, instance: String, epoch: UInt64
        public let error: Int32, closeError: Int32, queuedFrames: Int
        public let received: UInt64, sent: UInt64, rejected: UInt64
        public let closing: Bool, closeSent: Bool, released: Bool
    }
    private let owner: DispatchQueue
    private let stopLock = NSLock()
    private var raw: OpaquePointer?
    private var finalInfo: Info?
    private var finalTransport: ExtensionChannel.Info?
    private var finalProcess: ExtensionChannel.ProcessObservation?
    private var finalProcessStatus: Int32 = 7
    private func checkOwner() {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
    }
    private func pointer() throws -> OpaquePointer {
        checkOwner(); guard let raw else { throw GhostermError.failed("多实例物理 owner 已释放") }; return raw
    }
    public init(channel: ExtensionChannel, connection: String, frameBytes: UInt32 = 4096,
                receiveFrames: UInt32 = 64, instances: UInt32 = 8) throws {
        owner = channel.sessionOwner
        try Self.requireID(connection)
        var config = gt_ext_mux_config_t()
        config.frame_bytes = frameBytes; config.receive_frames = receiveFrames; config.instances = instances
        withUnsafeMutableBytes(of: &config.connection) { $0.copyBytes(from: Array(connection.utf8) + [0]) }
        raw = try channel.adoptMultiplexer(config: &config)
    }
    public func openChannel(instance: String, epoch: UInt64) throws -> ExtensionChannel {
        let p = try pointer(); try Self.requireID(instance)
        stopLock.lock(); defer { stopLock.unlock() }
        var channel: OpaquePointer?
        let status = instance.withCString { gt_ext_mux_open_channel(p, $0, epoch, &channel) }
        guard status == GT_OK, let channel else { throw GhostermError.status(Int32(status.rawValue), "无法登记多实例子通道") }
        return ExtensionChannel(adopting: channel, owner: owner, multiplex: self)
    }
    @discardableResult public func step(milliseconds: Int32 = 0) throws -> Int32 {
        Int32(gt_ext_mux_step(try pointer(), milliseconds).rawValue)
    }
    public func info() throws -> Info {
        checkOwner()
        if let finalInfo, raw == nil { return finalInfo }
        var value = gt_ext_mux_info_t()
        guard gt_ext_mux_info(try pointer(), &value) else { throw GhostermError.failed("多实例状态不可读") }
        return Info(value)
    }
    public func scopeInfo(instance: String, epoch: UInt64) throws -> ScopeInfo {
        let p = try pointer(); try Self.requireID(instance)
        var value = gt_ext_mux_scope_info_t()
        let status = instance.withCString { gt_ext_mux_scope_info(p, $0, epoch, &value) }
        guard status == GT_OK else { throw GhostermError.status(Int32(status.rawValue), "所选实例状态不可读") }
        return ScopeInfo(connection: Self.string(value.connection), instance: Self.string(value.instance), epoch: value.epoch,
            error: Int32(value.error.rawValue), closeError: Int32(value.close_error.rawValue), queuedFrames: Int(value.queued_frames),
            received: value.received, sent: value.sent, rejected: value.rejected,
            closing: value.closing, closeSent: value.close_sent, released: value.released)
    }
    public func transportInfo() throws -> ExtensionChannel.Info {
        checkOwner()
        if raw == nil, let finalTransport { return finalTransport }
        var value = gt_ext_channel_info_t()
        let status = gt_ext_mux_transport_info(try pointer(), &value)
        guard status == GT_OK else { throw GhostermError.status(Int32(status.rawValue), "物理通道观察不可读") }
        return try ExtensionChannel.decodeInfo(value)
    }
    public func processObservation() throws -> ExtensionChannel.ProcessObservation {
        checkOwner()
        if raw == nil {
            if let finalProcess { return finalProcess }
            throw GhostermError.status(finalProcessStatus, "物理进程观察不可读")
        }
        var value = gt_ext_transport_process_info_t()
        let status = gt_ext_mux_process_info(try pointer(), &value)
        guard status == GT_OK else { throw GhostermError.status(Int32(status.rawValue), "物理进程观察不可读") }
        return try ExtensionChannel.ProcessObservation(value)
    }
    public func stop() {
        stopLock.lock(); defer { stopLock.unlock() }; if let raw { gt_ext_mux_stop(raw) }
    }
    @discardableResult public func closeStep(milliseconds: Int32 = 0) throws -> Bool {
        let p = try pointer()
        let state = gt_ext_mux_close(p, milliseconds)
        if state == GT_EXT_CLOSE_ERROR { throw GhostermError.failed("物理连接仍待释放") }
        guard state == GT_EXT_RELEASED else { return false }
        finalInfo = try info()
        finalTransport = try? transportInfo()
        var process = gt_ext_transport_process_info_t()
        let processStatus = gt_ext_mux_process_info(p, &process)
        finalProcessStatus = Int32(processStatus.rawValue)
        if processStatus == GT_OK { finalProcess = try ExtensionChannel.ProcessObservation(process) }
        stopLock.lock(); defer { stopLock.unlock() }
        guard gt_ext_mux_destroy(&raw) == GT_OK else { throw GhostermError.failed("物理连接仍有子 owner") }
        return true
    }
    public static var pendingCloseCount: Int { MultiplexCleanup.count }
    public static func retryPendingClose(on owner: DispatchQueue) { MultiplexCleanup.retry(owner) }
    deinit { if let raw { MultiplexCleanup.enqueue(raw, owner: owner) } }
    private static func requireID(_ value: String) throws {
        guard value.utf8.count == 32, value.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }),
              value.utf8.contains(where: { $0 != 48 }) else { throw GhostermError.failed("多实例身份无效") }
    }
    private static func string<T>(_ value: T) -> String {
        withUnsafeBytes(of: value) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
    }
}

private final class MultiplexCleanup {
    private static let lock = NSLock()
    private static var entries: [UUID: MultiplexCleanup] = [:]
    private let id = UUID(), owner: DispatchQueue
    private var raw: OpaquePointer?
    init(_ raw: OpaquePointer, owner: DispatchQueue) { self.raw = raw; self.owner = owner }
    static var count: Int { lock.lock(); defer { lock.unlock() }; return entries.count }
    static func enqueue(_ raw: OpaquePointer, owner: DispatchQueue) {
        let item = MultiplexCleanup(raw, owner: owner)
        lock.lock(); entries[item.id] = item; lock.unlock(); owner.async { item.run() }
    }
    static func retry(_ owner: DispatchQueue) {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        lock.lock(); let values = entries.values.filter { $0.owner === owner }; lock.unlock()
        values.forEach { $0.run() }
    }
    private func run() {
        guard let raw else { return }
        gt_ext_mux_stop(raw)
        for _ in 0..<32 {
            let state = gt_ext_mux_close(raw, 0)
            if state == GT_EXT_CLOSE_ERROR { return }
            if state == GT_EXT_RELEASED {
                guard gt_ext_mux_destroy(&self.raw) == GT_OK else { return }
                Self.lock.lock(); Self.entries.removeValue(forKey: id); Self.lock.unlock(); return
            }
        }
    }
}
