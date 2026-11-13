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
    public struct Invocation {
        public let command: String, arguments: [String]
        public init(command: String, arguments: [String]) { self.command = command; self.arguments = arguments }
    }
    private static func withInvocation<T>(_ value: Invocation?, _ body: (UnsafePointer<gt_ext_invocation_t>?) throws -> T) throws -> T {
        guard let value else { return try body(nil) }
        guard !value.command.utf8.contains(0), value.command.utf8.count < 520, value.arguments.count <= 32 else { throw GhostermError.failed("命令调用超限或无效") }
        var size = 0
        for argument in value.arguments {
            size += argument.utf8.count + 1
            guard !argument.utf8.contains(0), argument.utf8.count <= 4096, size <= 16384 else { throw GhostermError.failed("命令参数超限或无效") }
        }
        var storage: [UnsafeMutablePointer<CChar>] = []
        defer { storage.forEach { $0.deallocate() } }
        for text in [value.command] + value.arguments {
            let bytes = Array(text.utf8CString), p = UnsafeMutablePointer<CChar>.allocate(capacity: bytes.count)
            p.initialize(from: bytes, count: bytes.count); storage.append(p)
        }
        let arguments: [UnsafePointer<CChar>?] = storage.dropFirst().map { UnsafePointer($0) }
        return try arguments.withUnsafeBufferPointer { values in
            var c = gt_ext_invocation_t(command: UnsafePointer(storage[0]), argv: values.baseAddress, argc: values.count)
            return try withUnsafePointer(to: &c) { try body($0) }
        }
    }
    public struct Config {
        public struct WorkspaceRoot {
            public let id: String, directoryFD: Int32, byteLimit: UInt64
            public init(id: String, directoryFD: Int32, byteLimit: UInt64) { self.id = id; self.directoryFD = directoryFD; self.byteLimit = byteLimit }
        }
        public struct ProcessProgram {
            public let name: String, executable: String, verifiedVersion: String, verifiedSHA256: String
            public init(name: String, executable: String, verifiedVersion: String, verifiedSHA256: String) {
                self.name = name; self.executable = executable; self.verifiedVersion = verifiedVersion
                self.verifiedSHA256 = verifiedSHA256
            }
        }
        public struct ProcessEnvironment {
            public let name: String, value: String
            public init(name: String, value: String) { self.name = name; self.value = value }
        }
        public let modules: [Module], entry: String, role: ExtensionSession.Role
        public let roots: [WorkspaceRoot]
        public let processPrograms: [ProcessProgram], processEnvironment: [ProcessEnvironment]
        public let invocation: Invocation?
        public let memoryBytes: UInt64, stackBytes: UInt64
        public let cpuMilliseconds: UInt32, jobsPerStep: UInt32, activationMilliseconds: UInt32
        public let processLifetimeMilliseconds: UInt32, processOutputBytes: UInt64
        public init(modules: [Module], entry: String, role: ExtensionSession.Role,
                    memoryBytes: UInt64 = 0, stackBytes: UInt64 = 0, cpuMilliseconds: UInt32 = 0,
                    jobsPerStep: UInt32 = 0, activationMilliseconds: UInt32 = 0, roots: [WorkspaceRoot] = [],
                    processPrograms: [ProcessProgram] = [], processEnvironment: [ProcessEnvironment] = [],
                    processLifetimeMilliseconds: UInt32 = 0, processOutputBytes: UInt64 = 0,
                    invocation: Invocation? = nil) {
            self.modules = modules; self.entry = entry; self.role = role; self.memoryBytes = memoryBytes; self.stackBytes = stackBytes
            self.cpuMilliseconds = cpuMilliseconds; self.jobsPerStep = jobsPerStep; self.activationMilliseconds = activationMilliseconds
            self.roots = roots; self.processPrograms = processPrograms; self.processEnvironment = processEnvironment
            self.processLifetimeMilliseconds = processLifetimeMilliseconds; self.processOutputBytes = processOutputBytes
            self.invocation = invocation
        }
    }
    public struct Info {
        public let phase: Phase, error: Failure, cleanupError: Int32, sessionEnd: ExtensionSession.End, sessionError: Int32
        public let disposeError: Bool, engineReleased: Bool, steps: UInt64, jobs: UInt64, moduleBytes: UInt64
    }
    public static var supportStatus: Int32 { Int32(gt_ext_runtime_support().rawValue) }
    private let owner: DispatchQueue, stopLock = NSLock()
    private let session: ExtensionSession?
    private let store: ExtensionResourceStore?
    private var raw: OpaquePointer?
    private func pointer() throws -> OpaquePointer {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard let raw else { throw GhostermError.failed("插件运行时已释放") }; return raw
    }
    public init(session: ExtensionSession, config: Config) throws {
        self.session = session; owner = session.runtimeOwner; store = session.runtimeStore
        guard !config.modules.isEmpty, config.modules.count <= 64, !config.entry.utf8.contains(0), config.entry.utf8.count <= 255,
              config.roots.count <= 8, config.processPrograms.count <= 16, config.processEnvironment.count <= 32,
              config.processLifetimeMilliseconds <= 3600000, config.processOutputBytes <= 268435456 else { throw GhostermError.failed("运行时模块配置无效") }
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
        var programs: [gt_ext_process_program_mapping_t] = []
        for program in config.processPrograms {
            guard !program.name.utf8.contains(0), !program.executable.utf8.contains(0), !program.verifiedVersion.utf8.contains(0),
                  program.name.utf8.count <= 128, program.executable.hasPrefix("/"), program.executable.utf8.count <= 4096,
                  !program.verifiedVersion.isEmpty, program.verifiedVersion.utf8.count <= 128,
                  program.verifiedSHA256.utf8.count == 64,
                  program.verifiedSHA256.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else { throw GhostermError.failed("进程程序映射无效") }
            programs.append(gt_ext_process_program_mapping_t(
                name: UnsafePointer(copy(Data(program.name.utf8)).assumingMemoryBound(to: CChar.self)),
                executable: UnsafePointer(copy(Data(program.executable.utf8)).assumingMemoryBound(to: CChar.self)),
                verified_version: UnsafePointer(copy(Data(program.verifiedVersion.utf8)).assumingMemoryBound(to: CChar.self)),
                verified_sha256: UnsafePointer(copy(Data(program.verifiedSHA256.utf8)).assumingMemoryBound(to: CChar.self))))
        }
        var environment: [gt_ext_process_environment_t] = []
        for item in config.processEnvironment {
            guard !item.name.utf8.contains(0), !item.value.utf8.contains(0), item.name.utf8.count <= 128,
                  item.value.utf8.count <= 4096 else { throw GhostermError.failed("进程环境项无效") }
            environment.append(gt_ext_process_environment_t(
                name: UnsafePointer(copy(Data(item.name.utf8)).assumingMemoryBound(to: CChar.self)),
                value: UnsafePointer(copy(Data(item.value.utf8)).assumingMemoryBound(to: CChar.self))))
        }
        raw = try Self.withInvocation(config.invocation) { invocation in try roots.withUnsafeBufferPointer { scopes in
            try programs.withUnsafeBufferPointer { programValues in try environment.withUnsafeBufferPointer { environmentValues in try modules.withUnsafeBufferPointer { values in
            var c = gt_ext_runtime_config_t(modules: values.baseAddress, module_count: values.count, entry: entry,
                role: gt_ext_role_t(rawValue: config.role.rawValue), memory_bytes: config.memoryBytes, stack_bytes: config.stackBytes,
                cpu_ms: config.cpuMilliseconds, jobs_per_step: config.jobsPerStep, activation_ms: config.activationMilliseconds,
                roots: scopes.baseAddress, root_count: scopes.count, process_programs: programValues.baseAddress,
                process_program_count: programValues.count, process_environment: environmentValues.baseAddress,
                process_environment_count: environmentValues.count, process_lifetime_ms: config.processLifetimeMilliseconds,
                process_output_bytes: config.processOutputBytes, invocation: invocation)
            return try session.adoptRuntime(config: &c)
        } } } } }
    }
    public init(session: ExtensionSession, package: ExtensionPackage, role: ExtensionSession.Role,
                associated: Bool = false, memoryBytes: UInt64 = 0, stackBytes: UInt64 = 0,
                cpuMilliseconds: UInt32 = 0, jobsPerStep: UInt32 = 0, activationMilliseconds: UInt32 = 0,
                roots: [Config.WorkspaceRoot] = [], processPrograms: [Config.ProcessProgram] = [],
                processEnvironment: [Config.ProcessEnvironment] = [], processLifetimeMilliseconds: UInt32 = 0,
                processOutputBytes: UInt64 = 0, invocation: Invocation? = nil) throws {
        self.session = session; owner = session.runtimeOwner; store = session.runtimeStore
        guard roots.count <= 8, processPrograms.count <= 16, processEnvironment.count <= 32,
              processLifetimeMilliseconds <= 3600000, processOutputBytes <= 268435456 else { throw GhostermError.failed("运行时授权根或进程配置超限") }
        var names: [UnsafeMutablePointer<CChar>] = []
        defer { names.forEach { $0.deallocate() } }
        var scopes: [gt_ext_runtime_root_t] = []
        for root in roots {
            guard !root.id.utf8.contains(0), root.id.utf8.count <= 64 else { throw GhostermError.failed("运行时授权根标识无效") }
            let bytes = Array(root.id.utf8CString), name = UnsafeMutablePointer<CChar>.allocate(capacity: bytes.count)
            name.initialize(from: bytes, count: bytes.count); names.append(name)
            scopes.append(gt_ext_runtime_root_t(id: UnsafePointer(name), directory_fd: root.directoryFD, byte_limit: root.byteLimit))
        }
        var programs: [gt_ext_process_program_mapping_t] = []
        for program in processPrograms {
            guard !program.name.utf8.contains(0), !program.executable.utf8.contains(0), !program.verifiedVersion.utf8.contains(0),
                  program.name.utf8.count <= 128, program.executable.hasPrefix("/"), program.executable.utf8.count <= 4096,
                  !program.verifiedVersion.isEmpty, program.verifiedVersion.utf8.count <= 128,
                  program.verifiedSHA256.utf8.count == 64,
                  program.verifiedSHA256.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else { throw GhostermError.failed("进程程序映射无效") }
            let n = Array(program.name.utf8CString), e = Array(program.executable.utf8CString), v = Array(program.verifiedVersion.utf8CString), d = Array(program.verifiedSHA256.utf8CString)
            let np = UnsafeMutablePointer<CChar>.allocate(capacity: n.count), ep = UnsafeMutablePointer<CChar>.allocate(capacity: e.count), vp = UnsafeMutablePointer<CChar>.allocate(capacity: v.count), dp = UnsafeMutablePointer<CChar>.allocate(capacity: d.count)
            np.initialize(from: n, count: n.count); ep.initialize(from: e, count: e.count); vp.initialize(from: v, count: v.count); dp.initialize(from: d, count: d.count)
            names += [np, ep, vp, dp]
            programs.append(gt_ext_process_program_mapping_t(name: UnsafePointer(np), executable: UnsafePointer(ep), verified_version: UnsafePointer(vp), verified_sha256: UnsafePointer(dp)))
        }
        var environment: [gt_ext_process_environment_t] = []
        for item in processEnvironment {
            guard !item.name.utf8.contains(0), !item.value.utf8.contains(0), item.name.utf8.count <= 128,
                  item.value.utf8.count <= 4096 else { throw GhostermError.failed("进程环境项无效") }
            let n = Array(item.name.utf8CString), v = Array(item.value.utf8CString)
            let np = UnsafeMutablePointer<CChar>.allocate(capacity: n.count), vp = UnsafeMutablePointer<CChar>.allocate(capacity: v.count)
            np.initialize(from: n, count: n.count); vp.initialize(from: v, count: v.count)
            names += [np, vp]
            environment.append(gt_ext_process_environment_t(name: UnsafePointer(np), value: UnsafePointer(vp)))
        }
        raw = try Self.withInvocation(invocation) { invocation in try scopes.withUnsafeBufferPointer { values in
            try programs.withUnsafeBufferPointer { programValues in try environment.withUnsafeBufferPointer { environmentValues in
            var c = gt_ext_runtime_config_t(modules: nil, module_count: 0, entry: nil,
                role: gt_ext_role_t(rawValue: role.rawValue), memory_bytes: memoryBytes, stack_bytes: stackBytes,
                cpu_ms: cpuMilliseconds, jobs_per_step: jobsPerStep, activation_ms: activationMilliseconds,
                roots: values.baseAddress, root_count: values.count, process_programs: programValues.baseAddress,
                process_program_count: programValues.count, process_environment: environmentValues.baseAddress,
                process_environment_count: environmentValues.count, process_lifetime_ms: processLifetimeMilliseconds,
                process_output_bytes: processOutputBytes, invocation: invocation)
            return try session.adoptPackageRuntime(package: package, config: &c, associated: associated)
        } } } }
    }
    public func step() throws -> Phase { Phase(rawValue: gt_ext_runtime_step(try pointer()).rawValue) ?? .failed }
    public func info() throws -> Info {
        var v = gt_ext_runtime_info_t(); guard gt_ext_runtime_info(try pointer(), &v) else { throw GhostermError.failed("运行时观察无效") }
        return Info(phase: Phase(rawValue: v.phase.rawValue) ?? .failed, error: Failure(rawValue: v.error.rawValue) ?? .io,
            cleanupError: Int32(v.cleanup_error.rawValue), sessionEnd: ExtensionSession.End(rawValue: v.session_end.rawValue) ?? .internal,
            sessionError: Int32(v.session_error.rawValue), disposeError: v.dispose_error, engineReleased: v.engine_released,
            steps: v.steps, jobs: v.jobs, moduleBytes: v.module_bytes)
    }
    /// Pulls one completed incoming resource while this Runtime owns the Session.
    /// The data is copied and its Core snapshot released on the Session owner.
    public func takeResourceData(maxBytes: UInt64 = 262144) throws -> (ExtensionResourceStore.Status, ExtensionSession.ResourceContent?) {
        guard try info().phase == .active else { return (.closed, nil) }
        guard maxBytes <= 1048576 else { return (.limit, nil) }
        guard let session else { return (.closed, nil) }
        return try session.takeRuntimeResourceData(maxBytes: maxBytes)
    }
    /// Takes one Executor -> Host request while this Runtime owns the Session.
    public func takeHostRequest() throws -> (ExtensionSession.RPCStatus, ExtensionSession.RPCRequest?) {
        guard try info().phase == .active, let session else {
            throw GhostermError.failed("插件 Runtime 未处于活动状态")
        }
        return try session.takeRuntimeRequest()
    }
    /// Replies to one Executor -> Host request while this Runtime owns the Session.
    public func respondHostRequest(ticket: UInt64, outcome: Data) throws -> ExtensionSession.RPCStatus {
        guard try info().phase == .active, ticket > 0, !outcome.isEmpty,
              outcome.count <= 3072, let session else {
            throw GhostermError.failed("插件 Host RPC 回复无效")
        }
        return try session.respondRuntimeRequest(ticket: ticket, outcome: outcome)
    }
    /// Sends one Host -> Executor RPC using the Session now owned by this Runtime.
    /// The caller must retain the handle and poll its result; unknown outcomes
    /// are never retried automatically.
    public func request(method: String, input: Data,
                        timeoutMilliseconds: UInt32 = 30000) throws -> ExtensionSession.RPCAdmission {
        guard try info().phase == .active, let session else {
            throw GhostermError.failed("插件 Runtime 未处于活动状态")
        }
        return try session.requestRuntime(method: method, input: input,
                                          timeoutMilliseconds: timeoutMilliseconds)
    }
    public func requestResult(handle: UInt64) throws -> (ExtensionSession.RPCStatus, ExtensionSession.RPCResult?) {
        guard let session else { throw GhostermError.failed("插件 Runtime 未持有 Session") }
        return try session.runtimeRequestResult(handle: handle)
    }
    public func cancelRequest(handle: UInt64) throws -> ExtensionSession.RPCStatus {
        guard let session else { throw GhostermError.failed("插件 Runtime 未持有 Session") }
        return try session.cancelRuntimeRequest(handle: handle)
    }
    public func forgetRequest(handle: UInt64) throws -> ExtensionSession.RPCStatus {
        guard let session else { throw GhostermError.failed("插件 Runtime 未持有 Session") }
        return try session.forgetRuntimeRequest(handle: handle)
    }
    public func stop() { stopLock.lock(); defer { stopLock.unlock() }; if let raw { gt_ext_runtime_stop(raw) } }
    @discardableResult public func closeStep() throws -> Bool {
        let p = try pointer(); let close = gt_ext_runtime_close(p)
        if close == GT_EXT_CLOSE_ERROR { throw GhostermError.failed("运行时关闭错误，保留 owner") }
        guard close == GT_EXT_RELEASED else { return false }
        stopLock.lock(); let status = gt_ext_runtime_destroy(&raw); stopLock.unlock()
        guard status == GT_OK else { throw GhostermError.failed("运行时仍有未释放资源") }
        session?.clearRuntimeBorrow()
        ExtensionResourceStore.retryPendingClose(on: owner); return true
    }
    public static var pendingCloseCount: Int { ExtensionRuntimeCleanup.count }
    public static func retryPendingClose(on owner: DispatchQueue) { ExtensionRuntimeCleanup.retry(owner) }
    deinit { if let raw { ExtensionRuntimeCleanup.enqueue(raw, owner: owner, session: session) } }
}
private final class ExtensionRuntimeCleanup {
    private static let lock = NSLock()
    private static var entries: [UUID: ExtensionRuntimeCleanup] = [:]
    private let id = UUID(), owner: DispatchQueue
    private let session: ExtensionSession?
    private var raw: OpaquePointer?
    init(_ raw: OpaquePointer, owner: DispatchQueue, session: ExtensionSession?) {
        self.raw = raw; self.owner = owner; self.session = session
    }
    static var count: Int { lock.lock(); defer { lock.unlock() }; return entries.count }
    static func enqueue(_ raw: OpaquePointer, owner: DispatchQueue, session: ExtensionSession?) {
        let value = ExtensionRuntimeCleanup(raw, owner: owner, session: session); lock.lock(); entries[value.id] = value; lock.unlock(); owner.async { value.run() }
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
                session?.clearRuntimeBorrow()
                Self.lock.lock(); Self.entries.removeValue(forKey: id); Self.lock.unlock(); ExtensionResourceStore.retryPendingClose(on: owner); return
            }
        }
    }
}
