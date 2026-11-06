import Cghosterm
import Foundation

/// Trusted package snapshot worker; current context supplies RPC and lifecycle.
/// No App activation/permission UI. Only stop is cross-thread.
public final class ExtensionRuntime {
    public enum Phase: UInt32 { case created, starting, active, failed, stopped, closed }
    public enum Failure: UInt32 { case none, script, memory, cpu, cancelled, jobs, `protocol`, io, expired }
    public struct Module {
        public let name: String, source: Data, sha256: String
        public init(name: String, source: Data, sha256: String) { self.name = name; self.source = source; self.sha256 = sha256 }
    }
    public struct Config {
        public struct WorkspaceRoot {
            public let id: String, directoryFD: Int32, byteLimit: UInt64
            public init(id: String, directoryFD: Int32, byteLimit: UInt64) { self.id = id; self.directoryFD = directoryFD; self.byteLimit = byteLimit }
        }
        public let modules: [Module], entry: String, role: ExtensionSession.Role
        public let roots: [WorkspaceRoot]
        public let memoryBytes: UInt64, stackBytes: UInt64
        public let cpuMilliseconds: UInt32, jobsPerStep: UInt32, activationMilliseconds: UInt32
        public init(modules: [Module], entry: String, role: ExtensionSession.Role,
                    memoryBytes: UInt64 = 0, stackBytes: UInt64 = 0, cpuMilliseconds: UInt32 = 0,
                    jobsPerStep: UInt32 = 0, activationMilliseconds: UInt32 = 0, roots: [WorkspaceRoot] = []) {
            self.modules = modules; self.entry = entry; self.role = role; self.memoryBytes = memoryBytes; self.stackBytes = stackBytes
            self.cpuMilliseconds = cpuMilliseconds; self.jobsPerStep = jobsPerStep; self.activationMilliseconds = activationMilliseconds
            self.roots = roots
        }
    }
    public struct Info {
        public let phase: Phase, error: Failure, cleanupError: Int32, sessionEnd: ExtensionSession.End, sessionError: Int32
        public let disposeError: Bool, engineReleased: Bool, steps: UInt64, jobs: UInt64, moduleBytes: UInt64
    }
    public static var supportStatus: Int32 { Int32(gt_ext_runtime_support().rawValue) }
    private let owner: DispatchQueue, stopLock = NSLock()
    private let store: ExtensionResourceStore?
    private var raw: OpaquePointer?
    private func pointer() throws -> OpaquePointer {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard let raw else { throw GhostermError.failed("插件运行时已释放") }; return raw
    }
    public init(session: ExtensionSession, config: Config) throws {
        owner = session.runtimeOwner; store = session.runtimeStore
        guard !config.modules.isEmpty, config.modules.count <= 64, !config.entry.utf8.contains(0), config.entry.utf8.count <= 255, config.roots.count <= 8 else { throw GhostermError.failed("运行时模块配置无效") }
        var buffers: [UnsafeMutableRawPointer] = []
        defer { buffers.forEach { $0.deallocate() } }
        func copy(_ data: Data) -> UnsafeMutableRawPointer {
            let p = UnsafeMutableRawPointer.allocate(byteCount: max(1, data.count + 1), alignment: 1)
            data.withUnsafeBytes { if let src = $0.baseAddress, !$0.isEmpty { p.copyMemory(from: src, byteCount: $0.count) } }
            p.storeBytes(of: UInt8(0), toByteOffset: data.count, as: UInt8.self); buffers.append(p); return p
        }
        var modules: [gt_ext_runtime_module_t] = [], total = 0
        for m in config.modules {
            guard !m.name.utf8.contains(0), m.name.utf8.count <= 255, !m.source.isEmpty, m.source.count <= 131072,
                  m.sha256.utf8.count == 64, m.sha256.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }), total <= 1048576 - m.source.count else { throw GhostermError.failed("运行时模块参数无效或超限") }
            total += m.source.count
            var value = gt_ext_runtime_module_t()
            value.name = UnsafePointer(copy(Data(m.name.utf8)).assumingMemoryBound(to: CChar.self))
            value.source = UnsafeRawPointer(copy(m.source)); value.size = m.source.count
            withUnsafeMutableBytes(of: &value.sha256) { dst in for (i, byte) in m.sha256.utf8.enumerated() { dst[i] = byte }; dst[64] = 0 }
            modules.append(value)
        }
        let entry = UnsafePointer(copy(Data(config.entry.utf8)).assumingMemoryBound(to: CChar.self))
        var roots: [gt_ext_runtime_root_t] = []
        for root in config.roots {
            guard !root.id.utf8.contains(0), root.id.utf8.count <= 64 else { throw GhostermError.failed("运行时授权根标识无效") }
            roots.append(gt_ext_runtime_root_t(id: UnsafePointer(copy(Data(root.id.utf8)).assumingMemoryBound(to: CChar.self)), directory_fd: root.directoryFD, byte_limit: root.byteLimit))
        }
        raw = try roots.withUnsafeBufferPointer { scopes in try modules.withUnsafeBufferPointer { values in
            var c = gt_ext_runtime_config_t(modules: values.baseAddress, module_count: values.count, entry: entry,
                role: gt_ext_role_t(rawValue: config.role.rawValue), memory_bytes: config.memoryBytes, stack_bytes: config.stackBytes,
                cpu_ms: config.cpuMilliseconds, jobs_per_step: config.jobsPerStep, activation_ms: config.activationMilliseconds, roots: scopes.baseAddress, root_count: scopes.count)
            return try session.adoptRuntime(config: &c)
        } }
    }
    public func step() throws -> Phase { Phase(rawValue: gt_ext_runtime_step(try pointer()).rawValue) ?? .failed }
    public func info() throws -> Info {
        var v = gt_ext_runtime_info_t(); guard gt_ext_runtime_info(try pointer(), &v) else { throw GhostermError.failed("运行时观察无效") }
        return Info(phase: Phase(rawValue: v.phase.rawValue) ?? .failed, error: Failure(rawValue: v.error.rawValue) ?? .io,
            cleanupError: Int32(v.cleanup_error.rawValue), sessionEnd: ExtensionSession.End(rawValue: v.session_end.rawValue) ?? .internal,
            sessionError: Int32(v.session_error.rawValue), disposeError: v.dispose_error, engineReleased: v.engine_released,
            steps: v.steps, jobs: v.jobs, moduleBytes: v.module_bytes)
    }
    public func stop() { stopLock.lock(); defer { stopLock.unlock() }; if let raw { gt_ext_runtime_stop(raw) } }
    @discardableResult public func closeStep() throws -> Bool {
        let p = try pointer(); let close = gt_ext_runtime_close(p)
        if close == GT_EXT_CLOSE_ERROR { throw GhostermError.failed("运行时关闭错误，保留 owner") }
        guard close == GT_EXT_RELEASED else { return false }
        stopLock.lock(); let status = gt_ext_runtime_destroy(&raw); stopLock.unlock()
        guard status == GT_OK else { throw GhostermError.failed("运行时仍有未释放资源") }
        ExtensionResourceStore.retryPendingClose(on: owner); return true
    }
    public static var pendingCloseCount: Int { ExtensionRuntimeCleanup.count }
    public static func retryPendingClose(on owner: DispatchQueue) { ExtensionRuntimeCleanup.retry(owner) }
    deinit { if let raw { ExtensionRuntimeCleanup.enqueue(raw, owner: owner) } }
}
private final class ExtensionRuntimeCleanup {
    private static let lock = NSLock()
    private static var entries: [UUID: ExtensionRuntimeCleanup] = [:]
    private let id = UUID(), owner: DispatchQueue
    private var raw: OpaquePointer?
    init(_ raw: OpaquePointer, owner: DispatchQueue) { self.raw = raw; self.owner = owner }
    static var count: Int { lock.lock(); defer { lock.unlock() }; return entries.count }
    static func enqueue(_ raw: OpaquePointer, owner: DispatchQueue) {
        let value = ExtensionRuntimeCleanup(raw, owner: owner); lock.lock(); entries[value.id] = value; lock.unlock(); owner.async { value.run() }
    }
    static func retry(_ owner: DispatchQueue) {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        lock.lock(); let values = entries.values.filter { $0.owner === owner }; lock.unlock(); values.forEach { $0.run() }
    }
    private func run() {
        guard let raw else { return }; gt_ext_runtime_stop(raw)
        for _ in 0..<32 {
            let close = gt_ext_runtime_close(raw)
            if close == GT_EXT_CLOSE_ERROR { return }
            if close == GT_EXT_RELEASED {
                guard gt_ext_runtime_destroy(&self.raw) == GT_OK else { return }
                Self.lock.lock(); Self.entries.removeValue(forKey: id); Self.lock.unlock(); ExtensionResourceStore.retryPendingClose(on: owner); return
            }
        }
    }
}
