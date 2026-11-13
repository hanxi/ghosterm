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
        public let requiredCapabilities: [String]

        public init(status: UInt32, packageID: String?, packageDigest: String?,
                    command: Command?, requiredCapabilities: [String] = []) {
            self.status = status; self.packageID = packageID; self.packageDigest = packageDigest
            self.command = command; self.requiredCapabilities = requiredCapabilities
        }
    }

    public struct SSHHop: Sendable {
        public let host: String
        public let port: UInt16
        public let username: String
        public let knownHostsPath: String?
        public let addressFamily: UInt8?
        public let addressBytes: [UInt8]?
        public let scope: UInt32
        public let viaHost: String?

        public init(host: String, port: UInt16, username: String, knownHostsPath: String?,
                    addressFamily: UInt8? = nil, addressBytes: [UInt8]? = nil,
                    scope: UInt32 = 0, viaHost: String? = nil) {
            self.host = host; self.port = port; self.username = username
            self.knownHostsPath = knownHostsPath; self.addressFamily = addressFamily
            self.addressBytes = addressBytes; self.scope = scope; self.viaHost = viaHost
        }
    }

    public enum SSHCredential: Sendable {
        case password(String)
        case privateKey(pem: String, passphrase: String?)
    }

    public struct Authentication: Sendable {
        public let phase: UInt32
        public let hostState: UInt32
        public let error: Int32
        public let deadline: Bool
        public let methods: Int32
        public let keyType: String
        public let fingerprint: String
    }

    public struct ChannelState: Sendable {
        public let phase: UInt32
        public let stage: UInt32
        public let error: Int32
        public let cleanupError: Int32
        public let hop: UInt32
        public let challenge: UInt64
        public let authentication: [Authentication]
        public let diagnostic: String
    }

    public struct RPCMethod: Sendable {
        public let name: String
        public let capability: String
        public let receiver: UInt32
        public let inputShape: UInt32
        public let outputShape: UInt32

        public init(name: String, capability: String, receiver: UInt32,
                    inputShape: UInt32 = 1, outputShape: UInt32 = 1) {
            self.name = name; self.capability = capability; self.receiver = receiver
            self.inputShape = inputShape; self.outputShape = outputShape
        }
    }

    public struct SessionState: Sendable {
        public let packageID: String
        public let packageDigest: String
        public let phase: UInt32
        public let end: UInt32
        public let error: Int32
        public let cleanupError: Int32
        public let consentToken: String
    }

    public struct RuntimeState: Sendable {
        public let packageID: String
        public let packageDigest: String
        public let command: String
        public let phase: UInt32
        public let failure: UInt32
        public let cleanupError: Int32
        public let disposeError: Int32
    }

    public struct HostRPCRequest: Sendable {
        public let ticket: UInt64
        public let requestID: String
        public let method: String
        public let input: Data
    }

    public struct HostRPCPoll: Sendable {
        public let status: UInt32
        public let request: HostRPCRequest?
    }

    public struct ReceivedResource: Sendable, Equatable {
        public let resourceID: String
        public let sha256: String
        public let mime: String
        public let displayName: String
        public let size: UInt64
        public let bytes: Data

        public init(resourceID: String, sha256: String, mime: String,
                    displayName: String, size: UInt64, bytes: Data) {
            self.resourceID = resourceID
            self.sha256 = sha256
            self.mime = mime
            self.displayName = displayName
            self.size = size
            self.bytes = bytes
        }
    }

    public enum ControllerError: Error, Sendable {
        case invalidHostProfile
        case invalidPackageURL
        case packageCapture(String)
        case packageCheck(String)
        case registry(UInt32)
        case channelUnavailable
        case sessionUnavailable
        case runtimeUnavailable
        case resourceUnavailable
        case closed
    }

    private let worker: Worker

    public init(profile: HostProfile, resourceDirectory: URL? = nil) throws {
        guard !profile.capabilities.contains("resource.receive") ||
                (resourceDirectory?.isFileURL == true && resourceDirectory?.path.isEmpty == false) else {
            throw ControllerError.invalidHostProfile
        }
        worker = Worker(hostJSON: try profile.json(), resourceDirectory: resourceDirectory)
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

    /// Decodes argv against the exact checked package snapshot selected by ID,
    /// digest, and command. This is a preview only; Runtime repeats Core decoding.
    public func decodeArguments(packageID: String, packageDigest: String,
                                command: String, arguments: [String]) async throws -> String? {
        try await worker.decodeArguments(packageID: packageID, packageDigest: packageDigest,
                                         command: command, arguments: arguments)
    }

    public func openSSHChannel(hops: [SSHHop], bootstrap: String) async throws -> ChannelState {
        try await worker.openSSHChannel(hops: hops, bootstrap: bootstrap)
    }

    public func stepSSHChannel(milliseconds: Int32 = 0) async throws -> ChannelState {
        try await worker.stepSSHChannel(milliseconds: milliseconds)
    }

    public func decideSSHHost(hop: UInt32, challenge: UInt64, decision: UInt32) async throws -> Int32 {
        try await worker.decideSSHHost(hop: hop, challenge: challenge, decision: decision)
    }

    public func submitSSHCredential(hop: UInt32, challenge: UInt64,
                                    credential: SSHCredential) async throws -> Int32 {
        try await worker.submitSSHCredential(hop: hop, challenge: challenge, credential: credential)
    }

    public func adoptSSHSession(pluginID: String, packageDigest: String, executorDigest: String,
                                hostVersion: String, peerIdentity: String,
                                methods: [RPCMethod],
                                additionalRequiredCapabilities: [String] = []) async throws -> SessionState {
        try await worker.adoptSSHSession(pluginID: pluginID, packageDigest: packageDigest,
                                         executorDigest: executorDigest, hostVersion: hostVersion,
                                         peerIdentity: peerIdentity, methods: methods,
                                         additionalRequiredCapabilities: additionalRequiredCapabilities)
    }

    public func stepSession(milliseconds: Int32 = 0) async throws -> SessionState {
        try await worker.stepSession(milliseconds: milliseconds)
    }

    public func decideSession(consentToken: String, allow: Bool) async throws -> SessionState {
        try await worker.decideSession(consentToken: consentToken, allow: allow)
    }

    public func activateClient(pluginID: String, packageDigest: String, command: String,
                               arguments: [String]) async throws -> RuntimeState {
        try await worker.activateClient(pluginID: pluginID, packageDigest: packageDigest,
                                        command: command, arguments: arguments)
    }

    public func stepRuntime() async throws -> RuntimeState {
        try await worker.stepRuntime()
    }

    /// Sends a Native Host -> remote Executor call through the Session owned by
    /// the active Client Runtime. The returned handle is never retried for the caller.
    public func requestExecutorRPC(method: String, input: Data,
                                   timeoutMilliseconds: UInt32 = 30000) async throws -> ExtensionSession.RPCAdmission {
        try await worker.requestExecutorRPC(method: method, input: input,
                                             timeoutMilliseconds: timeoutMilliseconds)
    }

    public func executorRPCResult(handle: UInt64) async throws -> (ExtensionSession.RPCStatus, ExtensionSession.RPCResult?) {
        try await worker.executorRPCResult(handle: handle)
    }

    public func cancelExecutorRPC(handle: UInt64) async throws -> ExtensionSession.RPCStatus {
        try await worker.cancelExecutorRPC(handle: handle)
    }

    public func forgetExecutorRPC(handle: UInt64) async throws -> ExtensionSession.RPCStatus {
        try await worker.forgetExecutorRPC(handle: handle)
    }

    /// Pulls one Client -> Host request from an active, consented Session.
    /// The caller decides how to present and handle the method, then responds
    /// with a bounded JSON outcome using `respondHostRPC`.
    public func pollHostRPCRequest() async throws -> HostRPCPoll {
        try await worker.pollHostRPCRequest()
    }

    /// Takes one completely received resource and copies its bounded snapshot
    /// on the Session owner. `nil` means no resource is ready yet.
    public func takeSessionResource(maxBytes: UInt64 = 262144) async throws -> ReceivedResource? {
        try await worker.takeSessionResource(maxBytes: maxBytes)
    }

    public func respondHostRPC(ticket: UInt64, outcome: Data) async throws -> UInt32 {
        try await worker.respondHostRPC(ticket: ticket, outcome: outcome)
    }

    public func stopRuntime() async throws { try await worker.stopRuntime() }

    public func stopSession() async throws { try await worker.stopSession() }

    public func stopSSHChannel() async throws { try await worker.stopSSHChannel() }

    public func releaseFinished() async throws { try await worker.releaseFinished() }

    /// Explicitly releases package and registry owners. Repeated close is safe.
    @discardableResult public func close() async -> Bool { await worker.close() }

    private final class Worker: @unchecked Sendable {
        private let queue = DispatchQueue(label: "org.ghosterm.extension-controller")
        private let hostJSON: String
        private let resourceDirectory: URL?
        private var registry: ExtensionRegistry?
        private enum SourceDecision: Equatable { case unknown, denied, allowed }
        private struct Record {
            let package: ExtensionPackage
            let review: PackageReview
            var source: SourceDecision
        }
        private var packages: [String: Record] = [:]
        private var closed = false
        private var closeComplete = false
        private var closeResult = false
        private var channel: ExtensionChannel?
        private var session: ExtensionSession?
        private var sessionPackageKey: String?
        private var runtime: ExtensionRuntime?
        private var runtimePackageKey: String?
        private var runtimeCommand: String?
        private var resourceStore: ExtensionResourceStore?
        private var resourceRootFD: Int32 = -1
        private var resourceDirectoryName: String?
        private var resourceDirectoryDevice: dev_t?
        private var resourceDirectoryInode: ino_t?
        private let hostCapabilities: [String]

        init(hostJSON: String, resourceDirectory: URL?) {
            self.hostJSON = hostJSON
            self.resourceDirectory = resourceDirectory
            if let data = hostJSON.data(using: .utf8),
               let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                self.hostCapabilities = object["capabilities"] as? [String] ?? []
            } else { self.hostCapabilities = [] }
        }

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

        func decodeArguments(packageID: String, packageDigest: String,
                             command: String, arguments: [String]) async throws -> String? {
            try await perform {
                try self.checkOpen()
                guard let record = self.packages[self.key(packageID, packageDigest)],
                      record.source == .allowed,
                      record.review.commands.contains(where: { $0.path == command }) else {
                    throw ControllerError.registry(ExtensionRegistry.Status.stale.rawValue)
                }
                return try record.package.decodeCommandArguments(path: command, arguments: arguments)
            }
        }

        func openSSHChannel(hops input: [SSHHop], bootstrap: String) async throws -> ChannelState {
            try await perform {
                try self.checkOpen()
                guard self.channel == nil, self.session == nil, self.runtime == nil else {
                    throw ControllerError.channelUnavailable
                }
                guard (1...8).contains(input.count), !bootstrap.isEmpty,
                      !bootstrap.utf8.contains(0), bootstrap.utf8.count <= 4096 else {
                    throw ControllerError.invalidHostProfile
                }
                let hops = try input.enumerated().map { index, hop -> ExtensionChannel.Hop in
                    guard !hop.host.isEmpty, !hop.username.isEmpty,
                          !hop.host.utf8.contains(0), !hop.username.utf8.contains(0),
                          hop.port > 0,
                          (hop.knownHostsPath?.utf8.contains(0) != true),
                          (hop.viaHost?.utf8.contains(0) != true) else {
                        throw ControllerError.invalidHostProfile
                    }
                    let address: SSHAddress?
                    if index == 0 {
                        guard hop.viaHost == nil, let family = hop.addressFamily, let bytes = hop.addressBytes,
                              (family == 4 || family == 6), bytes.count == 16 else {
                            throw ControllerError.invalidHostProfile
                        }
                        address = SSHAddress(family: family, bytes: bytes, scope: hop.scope)
                    } else {
                        guard hop.addressFamily == nil, hop.addressBytes == nil,
                              let via = hop.viaHost, !via.isEmpty else {
                            throw ControllerError.invalidHostProfile
                        }
                        address = nil
                    }
                    let options = SSHOptions(host: hop.host, port: hop.port, username: hop.username,
                                             knownHostsPath: hop.knownHostsPath)
                    return ExtensionChannel.Hop(options: options, address: address, viaHost: hop.viaHost)
                }
                let channel = try ExtensionChannel(hops: hops, bootstrap: bootstrap, owner: self.queue)
                self.channel = channel
                _ = try channel.step(milliseconds: 0)
                return try self.channelState(channel)
            }
        }

        func stepSSHChannel(milliseconds: Int32) async throws -> ChannelState {
            try await perform {
                try self.checkOpen()
                guard milliseconds >= 0, let channel = self.channel else { throw ControllerError.channelUnavailable }
                _ = try channel.step(milliseconds: milliseconds)
                return try self.channelState(channel)
            }
        }

        func decideSSHHost(hop: UInt32, challenge: UInt64, decision: UInt32) async throws -> Int32 {
            try await perform {
                try self.checkOpen()
                guard let channel = self.channel,
                      let value = HostKeyDecision(rawValue: decision) else { throw ControllerError.channelUnavailable }
                return try channel.decide(hop: hop, challenge: challenge, decision: value)
            }
        }

        func submitSSHCredential(hop: UInt32, challenge: UInt64,
                                 credential: SSHCredential) async throws -> Int32 {
            try await perform {
                try self.checkOpen()
                guard let channel = self.channel else { throw ControllerError.channelUnavailable }
                let value: GhostermCore.SSHCredential
                switch credential {
                case .password(let password): value = .password(password)
                case .privateKey(let pem, let passphrase): value = .privateKey(pem: pem, passphrase: passphrase)
                }
                return try channel.supply(hop: hop, challenge: challenge, credential: value)
            }
        }

        func adoptSSHSession(pluginID: String, packageDigest: String, executorDigest: String,
                             hostVersion: String, peerIdentity: String,
                             methods input: [RPCMethod],
                             additionalRequiredCapabilities: [String]) async throws -> SessionState {
            try await perform {
                try self.checkOpen()
                guard self.session == nil, self.runtime == nil, let channel = self.channel else {
                    throw ControllerError.channelUnavailable
                }
                let packageKey = self.key(pluginID, packageDigest)
                guard let record = self.packages[packageKey], record.source == .allowed else {
                    throw ControllerError.registry(ExtensionRegistry.Status.unknown.rawValue)
                }
                guard executorDigest.utf8.count == 64,
                      executorDigest.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }),
                      !hostVersion.isEmpty, hostVersion.utf8.count <= 64,
                      !peerIdentity.isEmpty, peerIdentity.utf8.count <= 255,
                      !hostVersion.utf8.contains(0), !peerIdentity.utf8.contains(0),
                      (try channel.info()).phase == .ready else { throw ControllerError.channelUnavailable }
                let metadata = try record.package.metadata()
                let remoteProcessCapabilityList = ["process.spawn", "process.pipes"]
                let remoteProcessCapabilities = Set(remoteProcessCapabilityList)
                let requestedRemoteProcess = !Set(additionalRequiredCapabilities)
                    .intersection(remoteProcessCapabilities).isEmpty
                let remoteProcessPairComplete = Set(additionalRequiredCapabilities)
                    .intersection(remoteProcessCapabilities) == remoteProcessCapabilities
                guard metadata.id == pluginID, metadata.packageDigest == packageDigest,
                      !metadata.executor, metadata.requires.allSatisfy(self.hostCapabilities.contains),
                      additionalRequiredCapabilities.count <= 16,
                      Set(additionalRequiredCapabilities).count == additionalRequiredCapabilities.count,
                      additionalRequiredCapabilities.allSatisfy({
                          self.hostCapabilities.contains($0) || remoteProcessCapabilities.contains($0)
                      }),
                      !requestedRemoteProcess || remoteProcessPairComplete,
                      input.count <= 16 else { throw ControllerError.registry(ExtensionRegistry.Status.unsupported.rawValue) }
                var requiredCapabilities = metadata.requires
                for capability in additionalRequiredCapabilities where !requiredCapabilities.contains(capability) {
                    requiredCapabilities.append(capability)
                }
                let methods = try input.map { method -> ExtensionSession.Method in
                    guard let receiver = ExtensionSession.Role(rawValue: method.receiver),
                          let inputShape = ExtensionSession.JSONShape(rawValue: method.inputShape),
                          let outputShape = ExtensionSession.JSONShape(rawValue: method.outputShape) else {
                        throw ControllerError.invalidHostProfile
                    }
                    return ExtensionSession.Method(name: method.name, capability: method.capability,
                                                   receiver: receiver, inputShape: inputShape,
                                                   outputShape: outputShape)
                }
                let resourceStore = try self.openResourceStoreIfNeeded()
                let sessionAvailable = self.hostCapabilities + remoteProcessCapabilityList
                    .filter { requestedRemoteProcess && !self.hostCapabilities.contains($0) }
                let session = try ExtensionSession(channel: channel, role: .host,
                    config: .init(pluginID: metadata.id, pluginVersion: metadata.version,
                                  clientDigest: metadata.packageDigest, executorDigest: executorDigest,
                                  hostVersion: hostVersion, sdkVersion: metadata.sdkVersion,
                                  required: requiredCapabilities, available: sessionAvailable,
                                  peerKind: .ssh, peerIdentity: peerIdentity, methods: methods),
                    resourceStore: resourceStore)
                self.channel = nil
                self.session = session
                self.sessionPackageKey = packageKey
                return try self.sessionState(session, packageKey: packageKey)
            }
        }

        func stepSession(milliseconds: Int32) async throws -> SessionState {
            try await perform {
                try self.checkOpen()
                guard milliseconds >= 0, let session = self.session,
                      let packageKey = self.sessionPackageKey else { throw ControllerError.sessionUnavailable }
                _ = try session.step(milliseconds: milliseconds)
                return try self.sessionState(session, packageKey: packageKey)
            }
        }

        func decideSession(consentToken: String, allow: Bool) async throws -> SessionState {
            try await perform {
                try self.checkOpen()
                guard let session = self.session, let packageKey = self.sessionPackageKey else {
                    throw ControllerError.sessionUnavailable
                }
                let info = try session.info()
                guard info.phase == .consent, info.consentToken == consentToken,
                      try session.decide(ticket: consentToken, decision: allow ? .allow : .deny) == 0 else {
                    throw ControllerError.sessionUnavailable
                }
                return try self.sessionState(session, packageKey: packageKey)
            }
        }

        func activateClient(pluginID: String, packageDigest: String, command: String,
                            arguments: [String]) async throws -> RuntimeState {
            try await perform {
                try self.checkOpen()
                guard let session = self.session, let sessionKey = self.sessionPackageKey,
                      sessionKey == self.key(pluginID, packageDigest),
                      try session.info().phase == .active,
                      let record = self.packages[sessionKey], record.source == .allowed,
                      record.review.commands.contains(where: { $0.path == command }) else {
                    throw ControllerError.sessionUnavailable
                }
                let selected = try self.registryOnQueue().resolve(command: command, executor: false,
                                                                   selectedID: pluginID,
                                                                   selectedDigest: packageDigest)
                guard selected.status == .ok, let exact = selected.package,
                      selected.command?.path == command else {
                    try? selected.package?.close()
                    throw ControllerError.registry(selected.status.rawValue)
                }
                try exact.close()
                let runtime = try ExtensionRuntime(session: session, package: record.package,
                                                   role: .host, associated: true,
                                                   invocation: .init(command: command, arguments: arguments))
                self.session = nil; self.sessionPackageKey = nil
                self.runtime = runtime; self.runtimePackageKey = sessionKey
                self.runtimeCommand = command
                return try self.runtimeState(runtime, packageKey: sessionKey, command: command)
            }
        }

        func stepRuntime() async throws -> RuntimeState {
            try await perform {
                try self.checkOpen()
                guard let runtime = self.runtime, let packageKey = self.runtimePackageKey,
                      let command = self.runtimeCommand else { throw ControllerError.runtimeUnavailable }
                _ = try runtime.step()
                return try self.runtimeState(runtime, packageKey: packageKey, command: command)
            }
        }

        func requestExecutorRPC(method: String, input: Data,
                                timeoutMilliseconds: UInt32) async throws -> ExtensionSession.RPCAdmission {
            try await perform {
                try self.checkOpen()
                guard let runtime = self.runtime else { throw ControllerError.runtimeUnavailable }
                return try runtime.request(method: method, input: input,
                                           timeoutMilliseconds: timeoutMilliseconds)
            }
        }

        func executorRPCResult(handle: UInt64) async throws -> (ExtensionSession.RPCStatus, ExtensionSession.RPCResult?) {
            try await perform {
                try self.checkOpen()
                guard let runtime = self.runtime else { throw ControllerError.runtimeUnavailable }
                return try runtime.requestResult(handle: handle)
            }
        }

        func cancelExecutorRPC(handle: UInt64) async throws -> ExtensionSession.RPCStatus {
            try await perform {
                try self.checkOpen()
                guard let runtime = self.runtime else { throw ControllerError.runtimeUnavailable }
                return try runtime.cancelRequest(handle: handle)
            }
        }

        func forgetExecutorRPC(handle: UInt64) async throws -> ExtensionSession.RPCStatus {
            try await perform {
                try self.checkOpen()
                guard let runtime = self.runtime else { throw ControllerError.runtimeUnavailable }
                return try runtime.forgetRequest(handle: handle)
            }
        }

        func pollHostRPCRequest() async throws -> HostRPCPoll {
            try await perform {
                try self.checkOpen()
                let status: ExtensionSession.RPCStatus
                let request: ExtensionSession.RPCRequest?
                if let session = self.session,
                   (try session.info()).phase == .active,
                   let lease = try session.lease(),
                   try session.access(lease, capability: "session.rpc") == .allowed {
                    (status, request) = try session.takeRequest(lease)
                } else if let runtime = self.runtime, try runtime.info().phase == .active {
                    (status, request) = try runtime.takeHostRequest()
                } else {
                    throw ControllerError.sessionUnavailable
                }
                let snapshot = request.map {
                    HostRPCRequest(ticket: $0.ticket, requestID: $0.requestID,
                                   method: $0.method, input: $0.input)
                }
                return HostRPCPoll(status: UInt32(status.rawValue), request: snapshot)
            }
        }

        func takeSessionResource(maxBytes: UInt64) async throws -> ReceivedResource? {
            try await perform {
                try self.checkOpen()
                guard maxBytes <= 1048576, self.hostCapabilities.contains("resource.receive") else {
                    throw ControllerError.resourceUnavailable
                }
                let status: ExtensionResourceStore.Status
                let content: ExtensionSession.ResourceContent?
                if let session = self.session {
                    guard try session.info().phase == .active,
                          let lease = try session.lease() else { return nil }
                    (status, content) = try session.takeResourceData(lease, maxBytes: maxBytes)
                } else if let runtime = self.runtime {
                    guard try runtime.info().phase == .active else { return nil }
                    (status, content) = try runtime.takeResourceData(maxBytes: maxBytes)
                } else {
                    throw ControllerError.resourceUnavailable
                }
                guard status != .wait else { return nil }
                guard status == .ok, let content else { throw ControllerError.resourceUnavailable }
                return ReceivedResource(resourceID: content.resourceID, sha256: content.sha256,
                                        mime: content.mime, displayName: content.displayName,
                                        size: content.size, bytes: content.bytes)
            }
        }

        func respondHostRPC(ticket: UInt64, outcome: Data) async throws -> UInt32 {
            try await perform {
                try self.checkOpen()
                guard ticket > 0, !outcome.isEmpty, outcome.count <= 3072 else {
                    throw ControllerError.sessionUnavailable
                }
                if let session = self.session,
                   (try session.info()).phase == .active,
                   let lease = try session.lease(),
                   try session.access(lease, capability: "session.rpc") == .allowed {
                    return UInt32(try session.respond(lease, ticket: ticket, outcome: outcome).rawValue)
                }
                guard let runtime = self.runtime, try runtime.info().phase == .active else {
                    throw ControllerError.sessionUnavailable
                }
                return UInt32(try runtime.respondHostRequest(ticket: ticket, outcome: outcome).rawValue)
            }
        }

        func stopRuntime() async throws {
            try await perform {
                try self.checkOpen()
                guard let runtime = self.runtime else { throw ControllerError.runtimeUnavailable }
                runtime.stop()
            }
        }

        func stopSession() async throws {
            try await perform {
                try self.checkOpen()
                guard let session = self.session else { throw ControllerError.sessionUnavailable }
                session.stop()
            }
        }

        func stopSSHChannel() async throws {
            try await perform {
                try self.checkOpen()
                guard let channel = self.channel else { throw ControllerError.channelUnavailable }
                channel.stop()
            }
        }

        func releaseFinished() async throws {
            try await perform {
                try self.checkOpen()
                try self.releaseOwners(forceStop: false)
            }
        }

        func close() async -> Bool {
            (try? await perform {
                self.closed = true
                if self.closeComplete { return self.closeResult }
                do {
                    try self.releaseOwners(forceStop: true)
                    var cleanupFailed = false
                    if let registry = self.registry {
                        do {
                            if try registry.close() != .ok { cleanupFailed = true }
                        } catch { cleanupFailed = true }
                    }
                    self.registry = nil
                    for record in self.packages.values {
                        do { try record.package.close() } catch { cleanupFailed = true }
                    }
                    self.packages.removeAll()
                    self.closeComplete = true
                    self.closeResult = !cleanupFailed
                    return self.closeResult
                } catch { return false }
            }) ?? false
        }

        private func channelState(_ channel: ExtensionChannel) throws -> ChannelState {
            let info = try channel.info()
            return ChannelState(phase: info.phase.rawValue, stage: info.stage.rawValue,
                                error: info.error, cleanupError: info.cleanupError,
                                hop: info.hop, challenge: info.challenge,
                                authentication: info.authentication.map {
                                    Authentication(phase: $0.phase.rawValue, hostState: $0.host.rawValue,
                                                   error: $0.error, deadline: $0.deadline, methods: $0.methods,
                                                   keyType: $0.keyType, fingerprint: $0.fingerprint)
                                }, diagnostic: String(decoding: info.diagnostic, as: UTF8.self))
        }

        private func sessionState(_ session: ExtensionSession, packageKey: String) throws -> SessionState {
            let info = try session.info()
            guard let record = packages[packageKey] else { throw ControllerError.sessionUnavailable }
            return SessionState(packageID: record.review.id, packageDigest: record.review.digest,
                                phase: info.phase.rawValue, end: info.end.rawValue, error: info.error,
                                cleanupError: info.cleanupError, consentToken: info.consentToken)
        }

        private func runtimeState(_ runtime: ExtensionRuntime, packageKey: String,
                                  command: String) throws -> RuntimeState {
            let info = try runtime.info()
            guard let record = packages[packageKey] else { throw ControllerError.runtimeUnavailable }
            return RuntimeState(packageID: record.review.id, packageDigest: record.review.digest,
                                command: command, phase: info.phase.rawValue, failure: info.error.rawValue,
                                cleanupError: info.cleanupError, disposeError: info.disposeError ? 1 : 0)
        }

        private func releaseOwners(forceStop: Bool) throws {
            if let runtime {
                if forceStop { runtime.stop() }
                else {
                    let phase = try runtime.info().phase
                    guard phase != .created, phase != .starting, phase != .active else {
                        throw ControllerError.runtimeUnavailable
                    }
                }
                var released = false
                for _ in 0..<32 { if try runtime.closeStep() { released = true; break } }
                guard released else { throw ControllerError.runtimeUnavailable }
                self.runtime = nil; runtimePackageKey = nil; runtimeCommand = nil
            }
            if let session {
                if forceStop { session.stop() }
                else {
                    let phase = try session.info().phase
                    guard phase == .ended || phase == .closed else { throw ControllerError.sessionUnavailable }
                }
                var released = false
                for _ in 0..<32 { if try session.closeStep() { released = true; break } }
                guard released else { throw ControllerError.sessionUnavailable }
                self.session = nil; sessionPackageKey = nil
            }
            try closeResourceStore()
            if let channel {
                if forceStop { channel.stop() }
                else {
                    let phase = try channel.info().phase
                    guard phase == .failed || phase == .stopped || phase == .closed else {
                        throw ControllerError.channelUnavailable
                    }
                }
                var released = false
                for _ in 0..<32 { if try channel.closeStep() { released = true; break } }
                guard released else { throw ControllerError.channelUnavailable }
                self.channel = nil
            }
        }

        private func openResourceStoreIfNeeded() throws -> ExtensionResourceStore? {
            guard hostCapabilities.contains("resource.receive") else { return nil }
            if let resourceStore { return resourceStore }
            guard let directory = resourceDirectory, directory.isFileURL,
                  !directory.path.isEmpty, !directory.path.utf8.contains(0),
                  directory.path.utf8.count <= 4096 else { throw ControllerError.resourceUnavailable }
            let rootFD = directory.path.withCString {
                open($0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            }
            guard rootFD >= 0 else { throw ControllerError.resourceUnavailable }
            var rootInfo = stat()
            guard fstat(rootFD, &rootInfo) == 0, (rootInfo.st_mode & S_IFMT) == S_IFDIR,
                  rootInfo.st_uid == geteuid() else {
                _ = Darwin.close(rootFD)
                throw ControllerError.resourceUnavailable
            }
            let name = "resource-" + UUID().uuidString.lowercased()
            let created = name.withCString { mkdirat(rootFD, $0, 0o700) }
            guard created == 0 else {
                _ = Darwin.close(rootFD)
                throw ControllerError.resourceUnavailable
            }
            let directoryFD = name.withCString {
                openat(rootFD, $0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            }
            guard directoryFD >= 0 else {
                _ = name.withCString { unlinkat(rootFD, $0, AT_REMOVEDIR) }
                _ = Darwin.close(rootFD)
                throw ControllerError.resourceUnavailable
            }
            var childInfo = stat()
            guard fstat(directoryFD, &childInfo) == 0,
                  (childInfo.st_mode & S_IFMT) == S_IFDIR,
                  childInfo.st_uid == geteuid() else {
                _ = Darwin.close(directoryFD)
                _ = name.withCString { unlinkat(rootFD, $0, AT_REMOVEDIR) }
                _ = Darwin.close(rootFD)
                throw ControllerError.resourceUnavailable
            }
            do {
                let store = try ExtensionResourceStore(directoryFD: directoryFD, owner: queue)
                _ = Darwin.close(directoryFD)
                self.resourceRootFD = rootFD
                self.resourceDirectoryName = name
                self.resourceDirectoryDevice = childInfo.st_dev
                self.resourceDirectoryInode = childInfo.st_ino
                self.resourceStore = store
                return store
            } catch {
                _ = Darwin.close(directoryFD)
                _ = name.withCString { unlinkat(rootFD, $0, AT_REMOVEDIR) }
                _ = Darwin.close(rootFD)
                throw ControllerError.resourceUnavailable
            }
        }

        private func closeResourceStore() throws {
            if let resourceStore {
                let status = try resourceStore.close()
                if status == .ok || status == .io { self.resourceStore = nil }
                guard status == .ok else { throw ControllerError.resourceUnavailable }
            }
            if let name = resourceDirectoryName, resourceRootFD >= 0 {
                var current = stat()
                let observed = name.withCString {
                    fstatat(resourceRootFD, $0, &current, AT_SYMLINK_NOFOLLOW)
                }
                if observed == 0 {
                    guard (current.st_mode & S_IFMT) == S_IFDIR,
                          current.st_dev == resourceDirectoryDevice,
                          current.st_ino == resourceDirectoryInode else {
                        throw ControllerError.resourceUnavailable
                    }
                    guard name.withCString({ unlinkat(resourceRootFD, $0, AT_REMOVEDIR) }) == 0 else {
                        throw ControllerError.resourceUnavailable
                    }
                } else {
                    guard errno == ENOENT else { throw ControllerError.resourceUnavailable }
                }
                resourceDirectoryName = nil
                resourceDirectoryDevice = nil
                resourceDirectoryInode = nil
            }
            if resourceRootFD >= 0 {
                guard Darwin.close(resourceRootFD) == 0 else { throw ControllerError.resourceUnavailable }
                resourceRootFD = -1
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
                                               argsSchema: command.argsSchema, views: command.views),
                              requiredCapabilities: metadata.requires)
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
