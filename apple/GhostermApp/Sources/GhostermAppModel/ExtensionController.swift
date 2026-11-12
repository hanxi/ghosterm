import Darwin
import Foundation
import GhostermCore

/// App-side owner for importing and resolving Client contributions.
/// Package compatibility, package source approval, and runtime activation are
/// deliberately separate operations; reviewing a package never approves it.
public actor ExtensionController {
    public enum Side: String, Sendable { case client, executor }

    public struct HostProfile: Sendable {
        public let side: Side
        public let sdkVersion: String
        public let protocols: [UInt16]
        public let runtimes: [String]
        public let capabilities: [String]

        public init(side: Side, sdkVersion: String, protocols: [UInt16],
                    runtimes: [String], capabilities: [String]) throws {
            guard !sdkVersion.utf8.contains(0), sdkVersion.utf8.count <= 128,
                  protocols.count <= 8, runtimes.count <= 16, capabilities.count <= 16,
                  protocols.allSatisfy({ $0 > 0 }), Set(protocols).count == protocols.count,
                  runtimes.allSatisfy({ !$0.isEmpty && !$0.utf8.contains(0) && $0.utf8.count <= 128 }),
                  capabilities.allSatisfy({ !$0.isEmpty && !$0.utf8.contains(0) && $0.utf8.count <= 128 }),
                  Set(runtimes).count == runtimes.count, Set(capabilities).count == capabilities.count else {
                throw ControllerError.invalidHostProfile
            }
            self.side = side
            self.sdkVersion = sdkVersion
            self.protocols = protocols
            self.runtimes = runtimes
            self.capabilities = capabilities
        }

        fileprivate func json() throws -> String {
            let value: [String: Any] = [
                "side": side.rawValue,
                "sdkVersion": sdkVersion,
                "protocols": protocols.map(Int.init),
                "runtimes": runtimes,
                "capabilities": capabilities,
            ]
            let data = try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
            guard let json = String(data: data, encoding: .utf8), json.utf8.count <= 65536 else {
                throw ControllerError.invalidHostProfile
            }
            return json
        }
    }

    public struct Command: Sendable, Equatable {
        public let path: String
        public let title: String
        public let argsSchema: String
        public let views: [String]
    }

    public struct PackageReview: Sendable, Equatable {
        public let id: String
        public let version: String
        public let publisher: String
        public let license: String
        public let digest: String
        public let runtime: String
        public let requiredCapabilities: [String]
        public let optionalMissingCapabilities: [String]
        public let authorizationGranted: Bool
        public let publisherAuthenticated: Bool
        public let commands: [Command]
        /// Newly imported packages remain untrusted until `approve` is called.
        public let sourceApproved: Bool
    }

    public struct Resolution: Sendable, Equatable {
        /// 0 is success; other values preserve ExtensionRegistry.Status raw values.
        public let status: UInt32
        public let packageID: String?
        public let packageDigest: String?
        public let command: Command?
    }

    public enum ControllerError: Error, Sendable {
        case invalidHostProfile
        case invalidPackageURL
        case packageCapture(String)
        case packageCheck(String)
        case registry(UInt32)
        case closed
    }

    private let worker: Worker

    public init(profile: HostProfile) throws {
        worker = Worker(hostJSON: try profile.json())
    }

    /// Reads a user-selected directory through a no-follow directory descriptor,
    /// captures and checks its immutable package, and registers it as UNKNOWN.
    public func review(directory: URL) async throws -> PackageReview {
        try await worker.review(directory: directory)
    }

    /// Explicit source decision for the exact reviewed id + digest pair.
    @discardableResult
    public func approve(id: String, digest: String) async throws -> UInt32 {
        try await worker.setSource(id: id, digest: digest, approved: true)
    }

    @discardableResult
    public func deny(id: String, digest: String) async throws -> UInt32 {
        try await worker.setSource(id: id, digest: digest, approved: false)
    }

    /// Resolve the Client side only. A successful result selects metadata; it
    /// does not start JavaScript or grant Session capabilities.
    public func resolve(command: String, selectedID: String? = nil,
                        selectedDigest: String? = nil) async throws -> Resolution {
        try await worker.resolve(command: command, selectedID: selectedID,
                                 selectedDigest: selectedDigest)
    }

    /// Explicitly releases package and registry owners. Repeated close is safe.
    public func close() async { await worker.close() }

    private final class Worker: @unchecked Sendable {
        private let queue = DispatchQueue(label: "org.ghosterm.extension-controller")
        private let hostJSON: String
        private var registry: ExtensionRegistry?
        private enum SourceDecision: Equatable { case unknown, denied, allowed }
        private struct Record {
            let package: ExtensionPackage
            let review: PackageReview
            var source: SourceDecision
        }
        private var packages: [String: Record] = [:]
        private var closed = false

        init(hostJSON: String) { self.hostJSON = hostJSON }

        func review(directory: URL) async throws -> PackageReview {
            try await perform { try self.reviewOnQueue(directory: directory) }
        }

        func setSource(id: String, digest: String, approved: Bool) async throws -> UInt32 {
            try await perform { try self.setSourceOnQueue(id: id, digest: digest, approved: approved) }
        }

        func resolve(command: String, selectedID: String?, selectedDigest: String?) async throws -> Resolution {
            try await perform { try self.resolveOnQueue(command: command, selectedID: selectedID,
                                                        selectedDigest: selectedDigest) }
        }

        func close() async {
            _ = try? await perform {
                guard !self.closed else { return () }
                self.closed = true
                // The registry owns references to every approved package. Drop
                // those references first so package.close() can synchronously
                // destroy each remaining owner before AppKit is told to exit.
                if let registry = self.registry { _ = try? registry.close() }
                self.registry = nil
                for record in self.packages.values { try? record.package.close() }
                self.packages.removeAll()
            }
        }

        private func perform<T: Sendable>(_ operation: @escaping @Sendable () throws -> T) async throws -> T {
            try await withCheckedThrowingContinuation { continuation in
                queue.async {
                    do { continuation.resume(returning: try operation()) }
                    catch { continuation.resume(throwing: error) }
                }
            }
        }

        private func checkOpen() throws {
            dispatchPrecondition(condition: .onQueue(queue))
            guard !Thread.isMainThread else { throw ControllerError.closed }
            guard !closed else { throw ControllerError.closed }
        }

        private func registryOnQueue() throws -> ExtensionRegistry {
            try checkOpen()
            if let registry { return registry }
            let value = try ExtensionRegistry(owner: queue)
            registry = value
            return value
        }

        private func key(_ id: String, _ digest: String) -> String { id + "\0" + digest }

        private func reviewOnQueue(directory: URL) throws -> PackageReview {
            try checkOpen()
            guard directory.isFileURL, !directory.path.isEmpty,
                  !directory.path.utf8.contains(0), directory.path.utf8.count <= 4096 else {
                throw ControllerError.invalidPackageURL
            }
            let scoped = directory.startAccessingSecurityScopedResource()
            defer { if scoped { directory.stopAccessingSecurityScopedResource() } }
            let fd = directory.path.withCString { open($0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) }
            guard fd >= 0 else { throw ControllerError.invalidPackageURL }
            defer { _ = Darwin.close(fd) }

            let package: ExtensionPackage
            do { package = try ExtensionPackage(directoryFD: fd, owner: queue) }
            catch { throw ControllerError.packageCapture(String(describing: error)) }

            do {
                var phase = try package.step()
                var steps = 1
                while phase == .capturing && steps < 100_000 {
                    phase = try package.step(); steps += 1
                }
                guard phase == .captured else {
                    let info = try package.info()
                    throw ControllerError.packageCapture(info.code)
                }
                guard try package.check(hostJSON: hostJSON) == .checked else {
                    let info = try package.info()
                    throw ControllerError.packageCheck(info.code)
                }
                let metadata = try package.metadata()
                let commands = try package.commands().map {
                    Command(path: $0.path, title: $0.title, argsSchema: $0.argsSchema, views: $0.views)
                }
                let packageKey = key(metadata.id, metadata.packageDigest)
                guard packages[packageKey] == nil else {
                    throw ControllerError.registry(ExtensionRegistry.Status.conflict.rawValue)
                }
                let review = PackageReview(id: metadata.id, version: metadata.version,
                                           publisher: metadata.publisher, license: metadata.license,
                                           digest: metadata.packageDigest, runtime: metadata.runtime,
                                           requiredCapabilities: metadata.requires,
                                           optionalMissingCapabilities: metadata.optionalMissing,
                                           authorizationGranted: metadata.authorizationGranted,
                                           publisherAuthenticated: metadata.publisherAuthenticated,
                                           commands: commands,
                                           sourceApproved: false)
                packages[packageKey] = Record(package: package, review: review, source: .unknown)
                return review
            } catch {
                try? package.close()
                throw error
            }
        }

        private func setSourceOnQueue(id: String, digest: String, approved: Bool) throws -> UInt32 {
            try checkOpen()
            guard id.utf8.count <= 128, digest.utf8.count == 64,
                  !id.utf8.contains(0), !digest.utf8.contains(0),
                  var record = packages[key(id, digest)] else {
                throw ControllerError.invalidPackageURL
            }
            if approved {
                let expected = record.source == .allowed ? digest : nil
                let status = try registryOnQueue().put(record.package, source: .allowed,
                                                       expectedDigest: expected)
                guard status == .ok else { return status.rawValue }
                record.source = .allowed
                packages[key(id, digest)] = record
                return ExtensionRegistry.Status.ok.rawValue
            }
            if record.source == .allowed {
                let status = try registryOnQueue().remove(id: id, executor: false,
                                                          expectedDigest: digest)
                guard status == .ok else { return status.rawValue }
            }
            record.source = .denied
            packages[key(id, digest)] = record
            return ExtensionRegistry.Status.denied.rawValue
        }

        private func resolveOnQueue(command: String, selectedID: String?,
                                    selectedDigest: String?) throws -> Resolution {
            try checkOpen()
            guard validCommand(command) else {
                return Resolution(status: ExtensionRegistry.Status.invalid.rawValue,
                                  packageID: nil, packageDigest: nil, command: nil)
            }
            guard (selectedID == nil) == (selectedDigest == nil) else {
                return Resolution(status: ExtensionRegistry.Status.invalid.rawValue,
                                  packageID: nil, packageDigest: nil, command: nil)
            }
            let matches = packages.values.filter { record in
                guard record.source != .denied else { return false }
                return record.review.commands.contains { overlaps(command, $0.path) }
            }
            if selectedID == nil, matches.count > 1 {
                return Resolution(status: ExtensionRegistry.Status.conflict.rawValue,
                                  packageID: nil, packageDigest: nil, command: nil)
            }
            let chosen: Record?
            if let selectedID, let selectedDigest {
                guard let record = packages[key(selectedID, selectedDigest)] else {
                    return Resolution(status: ExtensionRegistry.Status.stale.rawValue,
                                      packageID: nil, packageDigest: nil, command: nil)
                }
                chosen = record
            } else {
                chosen = matches.first
            }
            if let chosen {
                switch chosen.source {
                case .unknown:
                    return Resolution(status: ExtensionRegistry.Status.unknown.rawValue,
                                      packageID: chosen.review.id,
                                      packageDigest: chosen.review.digest, command: nil)
                case .denied:
                    return Resolution(status: ExtensionRegistry.Status.denied.rawValue,
                                      packageID: chosen.review.id,
                                      packageDigest: chosen.review.digest, command: nil)
                case .allowed:
                    break
                }
            }
            let selection = try registryOnQueue().resolve(command: command, executor: false,
                                                           selectedID: selectedID ?? chosen?.review.id,
                                                           selectedDigest: selectedDigest ?? chosen?.review.digest)
            guard selection.status == .ok, let package = selection.package,
                  let command = selection.command else {
                return Resolution(status: selection.status.rawValue, packageID: nil,
                                  packageDigest: nil, command: nil)
            }
            let metadata = try package.metadata()
            try package.close()
            return Resolution(status: ExtensionRegistry.Status.ok.rawValue,
                              packageID: metadata.id, packageDigest: metadata.packageDigest,
                              command: Command(path: command.path, title: command.title,
                                               argsSchema: command.argsSchema, views: command.views))
        }

        private func overlaps(_ lhs: String, _ rhs: String) -> Bool {
            lhs == rhs || lhs.hasPrefix(rhs + " ") || rhs.hasPrefix(lhs + " ")
        }

        private func validCommand(_ value: String) -> Bool {
            let bytes = Array(value.utf8)
            guard !bytes.isEmpty, bytes.count <= 519 else { return false }
            var componentLength = 0, components = 1
            for byte in bytes {
                if byte == 32 {
                    guard componentLength > 0, components < 8 else { return false }
                    componentLength = 0; components += 1
                } else {
                    guard (97...122).contains(byte) || (48...57).contains(byte) || byte == 45,
                          componentLength > 0 || (97...122).contains(byte) else { return false }
                    componentLength += 1
                    guard componentLength <= 64 else { return false }
                }
            }
            return componentLength > 0
        }
    }
}
