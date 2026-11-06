import Cghosterm
import Foundation

/// Native command catalog on one worker. Source decisions do not grant execution.
public final class ExtensionRegistry {
    public enum Status: UInt32 { case ok, invalid, unknown, denied, limit, memory, notFound, conflict, stale, io, unsupported, cancelled }
    public enum Source: UInt32 { case unknown, denied, allowed }
    public struct RegistryError: Error { public let status: Status }
    public struct Selection {
        public let status: Status, package: ExtensionPackage?, command: ExtensionPackage.Command?
    }
    private let owner: DispatchQueue
    private var raw: OpaquePointer?
    private func pointer() throws -> OpaquePointer {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard let raw else { throw GhostermError.failed("插件注册表已释放") }; return raw
    }
    private static func status(_ value: gt_ext_registry_status_t) -> Status { Status(rawValue: value.rawValue) ?? .invalid }
    private static func validate(_ value: String?, limit: Int) throws {
        guard let value else { return }
        guard !value.utf8.contains(0), value.utf8.count <= limit else { throw GhostermError.failed("插件注册参数无效或超限") }
    }
    public init(owner: DispatchQueue) throws {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread); self.owner = owner
        let status = Self.status(gt_ext_registry_open(&raw))
        guard status == .ok else { throw RegistryError(status: status) }
    }
    public func put(_ package: ExtensionPackage, source: Source, expectedDigest: String? = nil) throws -> Status {
        let p = try pointer(), bytes = try package.pointer(on: owner); try Self.validate(expectedDigest, limit: 64)
        return (expectedDigest ?? "").withCString { digest in
            Self.status(gt_ext_registry_put(p, bytes, gt_ext_registry_source_t(rawValue: source.rawValue), expectedDigest == nil ? nil : digest))
        }
    }
    public func remove(id: String, executor: Bool, expectedDigest: String) throws -> Status {
        let p = try pointer(); try Self.validate(id, limit: 128); try Self.validate(expectedDigest, limit: 64)
        return id.withCString { name in expectedDigest.withCString { digest in Self.status(gt_ext_registry_remove(p, name, executor, digest)) } }
    }
    public func resolve(command: String, executor: Bool, selectedID: String? = nil, selectedDigest: String? = nil) throws -> Selection {
        let p = try pointer(); try Self.validate(command, limit: 519); try Self.validate(selectedID, limit: 128); try Self.validate(selectedDigest, limit: 64)
        var package: OpaquePointer?, contribution = gt_ext_package_command_t()
        let status = command.withCString { path in (selectedID ?? "").withCString { id in (selectedDigest ?? "").withCString { digest in
            Self.status(gt_ext_registry_resolve(p, path, executor, selectedID == nil ? nil : id, selectedDigest == nil ? nil : digest, &package, &contribution))
        } } }
        guard status == .ok, let package else { return Selection(status: status, package: nil, command: nil) }
        return Selection(status: status, package: ExtensionPackage(adopting: package, owner: owner), command: ExtensionPackage.Command(contribution))
    }
    /// Even an IO result consumes this registry; held packages stay valid.
    @discardableResult public func close() throws -> Status { _ = try pointer(); return Self.status(gt_ext_registry_destroy(&raw)) }
    deinit { if let raw { owner.async { var value: OpaquePointer? = raw; _ = gt_ext_registry_destroy(&value) } } }
}
