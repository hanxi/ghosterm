import Cghosterm
import Foundation

/// Worker-owned fixed package. Compatibility is never publisher trust or consent.
public final class ExtensionPackage {
    public enum Phase: UInt32 { case capturing, captured, checked, failed, closed }
    public enum Failure: UInt32 { case none, invalid, unknown, unsupported, denied, limit, memory, cancelled, io, `internal`, changed }
    public struct Info {
        public let phase: Phase, error: Failure, code: String, members: UInt64, bytes: UInt64
    }
    public struct Metadata {
        public let id: String, version: String, publisher: String, license: String, entry: String
        public let runtime: String, variant: String, sdkVersion: String, packageDigest: String, manifestDigest: String
        public let requires: [String], optionalMissing: [String], protocols: [UInt16]
        public let executor: Bool, associationRequired: Bool, authorizationGranted: Bool, publisherAuthenticated: Bool
    }
    internal let owner: DispatchQueue
    private let stopLock = NSLock()
    private var raw: OpaquePointer?
    internal func pointer(on queue: DispatchQueue) throws -> OpaquePointer {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard queue === owner, let raw else { throw GhostermError.failed("插件包已释放或不属于此 worker") }; return raw
    }
    public init(directoryFD: Int32, owner: DispatchQueue, members: UInt32 = 1024, entries: UInt32 = 4096,
                memberBytes: UInt64 = 16777216, totalBytes: UInt64 = 67108864) throws {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread); self.owner = owner
        var c = gt_ext_package_config_t(directory_fd: directoryFD, members: members, entries: entries, member_bytes: memberBytes, total_bytes: totalBytes)
        let status = gt_ext_package_open(&c, &raw)
        guard status == GT_OK else { throw GhostermError.status(Int32(status.rawValue), "无法采集插件包") }
    }
    internal init(adopting raw: OpaquePointer, owner: DispatchQueue) {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        self.owner = owner; self.raw = raw
    }
    public struct Command {
        public let path: String, title: String, argsSchema: String, views: [String]
        internal init(_ value: gt_ext_package_command_t) {
            path = packageString(value.path); title = packageString(value.title); argsSchema = packageString(value.args_schema)
            views = withUnsafeBytes(of: value.views) { bytes in
                (0..<min(Int(value.view_count), 8)).map { i in String(decoding: bytes[(i * 16)..<((i + 1) * 16)].prefix(while: { $0 != 0 }), as: UTF8.self) }
            }
        }
    }
    public func commands() throws -> [Command] {
        let p = try pointer(on: owner); var count: UInt32 = 0
        guard gt_ext_package_command_count(p, &count), count <= 64 else { throw GhostermError.failed("插件命令尚不可读") }
        return try (0..<count).map { index in
            var command = gt_ext_package_command_t()
            guard gt_ext_package_command(p, index, &command) else { throw GhostermError.failed("插件命令读取失败") }
            return Command(command)
        }
    }
    public func step() throws -> Phase { Phase(rawValue: gt_ext_package_step(try pointer(on: owner)).rawValue) ?? .failed }
    public func check(hostJSON: String?, registryJSON: String? = nil, memoryBytes: UInt64 = 0,
                      stackBytes: UInt64 = 0, cpuMilliseconds: UInt32 = 0) throws -> Phase {
        let p = try pointer(on: owner)
        guard !(hostJSON?.utf8.contains(0) ?? false), !(registryJSON?.utf8.contains(0) ?? false),
              (hostJSON?.utf8.count ?? 0) <= 65536, (registryJSON?.utf8.count ?? 0) <= 1048576 else { throw GhostermError.failed("插件兼容事实无效或超限") }
        return (hostJSON ?? "").withCString { host in (registryJSON ?? "").withCString { registry in
            var c = gt_ext_package_check_config_t(host_json: hostJSON == nil ? nil : host, registry_json: registryJSON == nil ? nil : registry,
                memory_bytes: memoryBytes, stack_bytes: stackBytes, cpu_ms: cpuMilliseconds)
            return Phase(rawValue: gt_ext_package_check(p, &c).rawValue) ?? .failed
        } }
    }
    public func info() throws -> Info {
        var v = gt_ext_package_info_t(); guard gt_ext_package_info(try pointer(on: owner), &v) else { throw GhostermError.failed("插件包观察无效") }
        return Info(phase: Phase(rawValue: v.phase.rawValue) ?? .failed, error: Failure(rawValue: v.error.rawValue) ?? .internal,
                    code: packageString(v.code), members: v.members, bytes: v.bytes)
    }
    public func metadata() throws -> Metadata {
        var v = gt_ext_package_metadata_t(); guard gt_ext_package_metadata(try pointer(on: owner), &v) else { throw GhostermError.failed("插件包尚未通过兼容校验") }
        func names<T>(_ value: T, _ count: UInt32) -> [String] {
            withUnsafeBytes(of: value) { bytes in (0..<min(Int(count), 16)).map { i in String(decoding: bytes[(i * 129)..<((i + 1) * 129)].prefix(while: { $0 != 0 }), as: UTF8.self) } }
        }
        let protocols = withUnsafeBytes(of: v.protocols) { Array($0.bindMemory(to: UInt16.self).prefix(min(Int(v.protocol_count), 8))) }
        return Metadata(id: packageString(v.id), version: packageString(v.version), publisher: packageString(v.publisher), license: packageString(v.license), entry: packageString(v.entry),
            runtime: packageString(v.runtime), variant: packageString(v.variant), sdkVersion: packageString(v.sdk_version), packageDigest: packageString(v.package_digest), manifestDigest: packageString(v.manifest_digest),
            requires: names(v.requires, v.requires_count), optionalMissing: names(v.optional_missing, v.optional_missing_count), protocols: protocols,
            executor: v.executor, associationRequired: v.association_required, authorizationGranted: v.authorization_granted, publisherAuthenticated: v.publisher_authenticated)
    }
    public func stop() { stopLock.lock(); defer { stopLock.unlock() }; if let raw { gt_ext_package_stop(raw) } }
    public func close() throws {
        _ = try pointer(on: owner); stopLock.lock(); let status = gt_ext_package_destroy(&raw); stopLock.unlock()
        guard status == GT_OK else { throw GhostermError.status(Int32(status.rawValue), "插件包关闭错误，引用已释放") }
    }
    deinit { if let raw { owner.async { var value: OpaquePointer? = raw; _ = gt_ext_package_destroy(&value) } } }
}
private func packageString<T>(_ value: T) -> String { withUnsafeBytes(of: value) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) } }
