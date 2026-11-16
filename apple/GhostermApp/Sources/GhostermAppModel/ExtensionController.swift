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

    /// Local Client publication grant. Never part of the remote relay command.
    public struct ClientWorkspace: Sendable, Equatable {
        public let directory: String
        public let authorized: Bool
        public init(directory: String, authorized: Bool) {
            self.directory = directory; self.authorized = authorized
        }
        public func validate(requiredCapabilities: [String]) throws {
            guard authorized, directory.hasPrefix("/"), directory.utf8.count <= 4096,
                  !directory.utf8.contains(0), !directory.contains("\n"), !directory.contains("\r"),
                  requiredCapabilities.contains("resource.receive") else {
                throw GhostermError.failed("本机资源目录需要显式允许，且 Client 必须声明 resource.receive")
            }
        }
        public var reviewDescription: String { "Native 只读发布目录：\(directory) · 单文件上限 256 KiB" }
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

    public struct PreparedRelease: Sendable, Equatable {
        public let id: String
        public let review: PackageReview
        public let signature: PluginDirectorySignature
        public let sdkVersion: String
        public let protocols: [UInt16]
        public let permissions: [ReleasePermission]
        public let executorDeclarations: String?
    }
    public struct ReleasePermission: Sendable, Equatable {
        public let side: String, capability: String, purpose: String, scope: String
        public let programs: [String]
    }
    public struct InstalledRelease: Sendable, Equatable {
        public let review: PackageReview
        public let signature: PluginDirectorySignature
        public let directory: URL
        public let durabilityConfirmed: Bool
        public var maintenanceIssue: String? = nil
    }
    public struct InstallCleanupError: Error, LocalizedError, Sendable {
        public let directory: URL
        public let reason: String
        init(directory: URL, error: Error) {
            self.directory = directory
            reason = (error as? LocalizedError)?.errorDescription ?? String(describing: error)
        }
        public var errorDescription: String? { "安装暂存清理失败，保留于 \(directory.path)：\(reason)" }
    }
    public struct InstalledVersion: Sendable, Equatable, Identifiable {
        public let releaseID: String, pluginID: String, version: String, publisher: String, packageSHA256: String
        public let selected: Bool, enabled: Bool
        public let inUse: Bool?
        public var id: String { releaseID }
    }
    public struct InstalledCatalog: Sendable, Equatable {
        public let revision: String?
        public let versions: [InstalledVersion]
        public let issues: [String]
        public let maintenanceCount: Int
        public let selections: [InstalledSelection]
        public var removalPending = false
    }
    public struct InstalledSelection: Sendable, Equatable, Identifiable {
        public let pluginID: String, releaseID: String
        public let enabled: Bool
        public var id: String { pluginID }
    }
    public struct VersionSelectionResult: Sendable, Equatable {
        public let release: InstalledRelease?
        public let catalog: InstalledCatalog
        public let durabilityConfirmed: Bool
    }
    public struct RemovalPreview: Sendable, Equatable {
        public struct File: Sendable, Equatable, Identifiable {
            public let path: String
            public let directory: Bool
            public let bytes: Int64
            public let sha256: String?
            public var id: String { path }
        }
        public let id: String, releaseID: String
        public let directory: URL
        public let files: [File]
        public let bytes: Int64
        public var unknownOrigin = false
    }
    public struct RemovalResult: Sendable, Equatable {
        public let releaseID: String
        public let removed: Int, observedAbsent: Int
        public let complete: Bool, durabilityConfirmed: Bool
        public let cleanupIssue: String?
    }
    public enum InstallationResidueState: String, Sendable { case active, abandoned, published, absent, unconfirmed, changed }
    public struct InstallationResidue: Sendable, Equatable, Identifiable {
        public let id: String, digest: String
        public let directory: URL
        public let state: InstallationResidueState
        public let publishedDirectory: URL?
    }
    public struct InstallationResidues: Sendable, Equatable {
        public let entries: [InstallationResidue]
        public let issues: [String]
    }
    public struct UnclassifiedEntry: Sendable, Equatable, Identifiable {
        public let name: String, identity: String
        public let directory: URL
        public let canPreview: Bool
        public let issue: String?
        public var id: String { identity }
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
        public let process: ProcessState?
        public let temporary: ExtensionLocalTemporaryState?
    }

    public struct ProcessState: Sendable {
        public let error, cleanupError, groupSignalError: Int32
        public let reason: UInt32, childExited: Bool, reaped: Bool, groupFenced: Bool
        public let exitCode, termSignal: Int32?
        public let stdoutEOF, stderrEOF, stopping: Bool
        public let stdoutBytes, stderrBytes: UInt64
        public let diagnostic: String, diagnosticTruncated: Bool
        init(_ p: ExtensionChannel.ProcessObservation) {
            error = p.error; cleanupError = p.cleanupError; groupSignalError = p.groupSignalError
            reason = p.reason.rawValue; childExited = p.childExited; reaped = p.reaped; groupFenced = p.groupFenced
            exitCode = p.exitCode; termSignal = p.termSignal
            stdoutEOF = p.stdoutEOF; stderrEOF = p.stderrEOF; stopping = p.stopping
            stdoutBytes = p.stdoutBytes; stderrBytes = p.stderrBytes
            diagnostic = String(decoding: p.diagnostic, as: UTF8.self); diagnosticTruncated = p.diagnosticTruncated
        }
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
        public let process: ProcessState?
        public let temporary: ExtensionLocalTemporaryState?
    }

    public struct ResourcePublication: Sendable {
        public var id: String, relativePath: String
        public var phase: UInt32, error: UInt32, cleanupError: UInt32
        public var confirmed: Bool, released: Bool, expired: Bool
        public var sha256: String, bytes: UInt64, accepted: UInt64
        public var reviewDescription: String {
            let message: String
            switch ExtensionFileSource.Phase(rawValue: phase) {
            case .hashing: message = "正在校验文件"
            case .ready, .sending: message = "正在发送：\(accepted) / \(bytes) 字节"
            case .receipt: message = "等待对端确认"
            case .cancelling: message = "正在取消，结果尚未确认"
            case .succeeded: message = confirmed ? "发布完成，对端已确认" : "发布结果尚未确认"
            case .stopped: message = "发布已停止"
            case .uncertain: message = "发布结果不明"
            case .failed:
                let reason = ExtensionFileSource.Status(rawValue: error).map { String(describing: $0) } ?? "unknown"
                message = "发布失败（\(reason)）"
            case nil: message = "发布状态不可用"
            }
            return message + (expired ? " · 已到期" : "") + (cleanupError == 0 ? "" : " · 资源释放失败")
        }
    }

    public struct RuntimeState: Sendable {
        public let packageID: String
        public let packageDigest: String
        public let command: String
        public let phase: UInt32
        public let failure: UInt32
        public let cleanupError: Int32
        public let disposeError: Int32
        public let publication: ResourcePublication?
        public let instanceID: String
        public let process: ProcessState?
        public let temporary: ExtensionLocalTemporaryState?
        public let views: Data?
        public let viewSessionInstance: String?
        public let viewSessionEpoch: String?
        public let viewIssue: String?
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
        case localProcessDraining
        case closed
    }

    private let worker: Worker

    public init(profile: HostProfile, resourceDirectory: URL? = nil, localJournalDirectory: URL? = nil,
                installationDirectory: URL? = nil) throws {
        guard !profile.capabilities.contains("resource.receive") ||
                (resourceDirectory?.isFileURL == true && resourceDirectory?.path.isEmpty == false) else {
            throw ControllerError.invalidHostProfile
        }
        worker = Worker(hostJSON: try profile.json(), resourceDirectory: resourceDirectory,
                        localJournalDirectory: localJournalDirectory ?? resourceDirectory?.appendingPathComponent("NativeLocalRuns", isDirectory: true),
                        installationDirectory: installationDirectory)
    }

    /// Reads a user-selected directory through a no-follow directory descriptor,
    /// captures and checks its immutable package, and registers it as UNKNOWN.
    public func review(directory: URL) async throws -> PackageReview {
        try await worker.review(directory: directory)
    }

    public func prepareSignedRelease(artifact: URL, descriptor: URL, signature: URL,
                                     trustedPolicy: URL) async throws -> PreparedRelease {
        try await worker.prepareSignedRelease(artifact: artifact, descriptor: descriptor,
            signature: signature, trustedPolicy: trustedPolicy)
    }
    public func installPreparedRelease(id: String) async throws -> InstalledRelease {
        try await worker.installPreparedRelease(id: id)
    }
    public func discardPreparedRelease(id: String) async throws { try await worker.discardPreparedRelease(id: id) }
    public func retryFailedInstallCleanup() async throws { try await worker.retryFailedInstallCleanup() }
    public func installedVersions() async throws -> InstalledCatalog { try await worker.installedVersions() }
    public func previewVersionRemoval(releaseID: String, expectedRevision: String?) async throws -> RemovalPreview {
        try await worker.previewVersionRemoval(releaseID: releaseID, expectedRevision: expectedRevision)
    }
    public func cancelVersionRemoval(id: String) async { await worker.cancelVersionRemoval(id: id) }
    public func confirmVersionRemoval(id: String, acknowledgedUnknown: Bool = false) async throws -> RemovalResult { try await worker.confirmVersionRemoval(id: id, acknowledgedUnknown: acknowledgedUnknown) }
    public func resumeVersionRemoval(id: String) async throws -> RemovalResult { try await worker.resumeVersionRemoval(id: id) }
    public func pendingVersionRemoval() async throws -> RemovalPreview? { try await worker.pendingVersionRemoval() }
    public func installationResidues() async throws -> InstallationResidues { try await worker.installationResidues() }
    public func unclassifiedEntries() async throws -> [UnclassifiedEntry] { try await worker.unclassifiedEntries() }
    public func previewUnclassifiedDirectory(name: String, expectedIdentity: String) async throws -> RemovalPreview {
        try await worker.previewUnclassifiedDirectory(name: name, expectedIdentity: expectedIdentity)
    }
    public func previewInstallationResidue(id: String, expectedDigest: String) async throws -> RemovalPreview {
        try await worker.previewInstallationResidue(id: id, expectedDigest: expectedDigest)
    }
    public func retireInstallationRecord(id: String, expectedDigest: String) async throws {
        try await worker.retireInstallationRecord(id: id, expectedDigest: expectedDigest)
    }
    public func cancelPendingVersionRemoval(id: String) async throws -> Bool { try await worker.cancelPendingVersionRemoval(id: id) }
    public func selectInstalledVersion(releaseID: String, expectedRevision: String?, trustedPolicy: URL) async throws -> VersionSelectionResult {
        try await worker.selectInstalledVersion(releaseID: releaseID, expectedRevision: expectedRevision, trustedPolicy: trustedPolicy)
    }
    public func disableInstalledPlugin(pluginID: String, expectedRevision: String?) async throws -> VersionSelectionResult {
        try await worker.disableInstalledPlugin(pluginID: pluginID, expectedRevision: expectedRevision)
    }

    /// Attests the immutable checked directory against an explicitly trusted
    /// public policy snapshot. Does not approve a source or grant capabilities.
    public func verifyDirectorySignature(packageID: String, digest: String, descriptor: URL,
                                         signature: URL, trustedPolicy: URL) async throws -> PluginDirectorySignature {
        try await worker.verifyDirectorySignature(packageID: packageID, digest: digest,
            descriptor: descriptor, signature: signature, trustedPolicy: trustedPolicy)
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
    public func openLocalChannel(executable: String, arguments: [String], launch: ExtensionLocalLaunch,
                                 environmentNames: [String] = []) async throws -> ChannelState {
        try await worker.openLocalChannel(executable: executable, arguments: arguments, launch: launch, environmentNames: environmentNames)
    }
    public func localProcessState() async throws -> ProcessState? { try await worker.localProcessState() }
    public func localTemporaryState() async throws -> ExtensionLocalTemporaryState? { try await worker.localTemporaryState() }
    public func inspectLocalReservations() async throws -> [ExtensionLocalJournalEntry] { try await worker.inspectLocalReservations() }
    public func releaseMissingLocalReservation(_ entry: ExtensionLocalJournalEntry, confirmed: Bool) async throws {
        try await worker.releaseMissingLocalReservation(entry, confirmed: confirmed)
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
    public func adoptLocalSession(pluginID: String, packageDigest: String, executorDigest: String,
                                   hostVersion: String, peerIdentity: String, methods: [RPCMethod],
                                   additionalRequiredCapabilities: [String] = []) async throws -> SessionState {
        try await worker.adoptSSHSession(pluginID: pluginID, packageDigest: packageDigest, executorDigest: executorDigest,
            hostVersion: hostVersion, peerIdentity: peerIdentity, methods: methods,
            additionalRequiredCapabilities: additionalRequiredCapabilities, local: true)
    }

    public func stepSession(milliseconds: Int32 = 0) async throws -> SessionState {
        try await worker.stepSession(milliseconds: milliseconds)
    }

    public func decideSession(consentToken: String, allow: Bool) async throws -> SessionState {
        try await worker.decideSession(consentToken: consentToken, allow: allow)
    }

    public func activateClient(pluginID: String, packageDigest: String, command: String,
                               arguments: [String], clientWorkspace: ClientWorkspace? = nil) async throws -> RuntimeState {
        try await worker.activateClient(pluginID: pluginID, packageDigest: packageDigest,
                                        command: command, arguments: arguments, clientWorkspace: clientWorkspace)
    }

    /// Explicit Native UI action; this method is never exposed to Client JS.
    public func publishClientResource(instanceID: String, root: String = "client", relativePath: String) async throws -> RuntimeState {
        try await worker.publishClientResource(instanceID: instanceID, root: root, relativePath: relativePath)
    }
    public func cancelClientPublication(id: String) async throws {
        try await worker.cancelClientPublication(id: id)
    }

    public func stepRuntime() async throws -> RuntimeState {
        try await worker.stepRuntime()
    }
    public func closeRuntimeView(instanceID: String, handle: String) async throws -> RuntimeState {
        try await worker.closeRuntimeView(instanceID: instanceID, handle: handle)
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
            var managedReleaseID: String? = nil
            var approvalGeneration: String? = nil
        }
        private var packages: [String: Record] = [:]
        private struct Prepared {
            let value: PreparedRelease
            let record: Record
            let stage: PluginReleaseStage
        }
        private var prepared: [String: Prepared] = [:]
        private var failedInstallDiscards = Set<String>()
        private var retainedInstallStages: [PluginReleaseStage] = []
        private let installationDirectory: URL?
        private var managedPluginIDs = Set<String>()
        private var registeredDigests: [String: String] = [:]
        private var closed = false
        private var closeComplete = false
        private var closeResult = false
        private var channel: ExtensionChannel?
        private var local = false
        private var lastLocalProcess: ProcessState?
        private var localTemporary: ExtensionLocalTemporaryDirectory?
        private var lastLocalTemporary: ExtensionLocalTemporaryState?
        private let localJournalDirectory: URL?
        private var localJournal: ExtensionLocalJournal?
        private var session: ExtensionSession?
        private var sessionPackageKey: String?
        private var runtime: ExtensionRuntime?
        private var sessionVersionLease: PluginReleaseCatalog.VersionHandle?
        private var runtimeVersionLease: PluginReleaseCatalog.VersionHandle?
        private var removalPreview: PluginReleaseRemoval?
        private var runtimePackageKey: String?
        private var runtimeInstanceID: String?
        private var runtimeCommand: String?
        private var runtimeViewScope: (instance: String, epoch: String)?
        private var runtimeViewIssue: String?
        private var clientRootFD: Int32 = -1
        private var clientSource: ExtensionFileSource?
        private var clientPublication: ResourcePublication?
        private var clientPublicationDeadline = 0.0
        private var resourceStore: ExtensionResourceStore?
        private var resourceRootFD: Int32 = -1
        private var resourceDirectoryName: String?
        private var resourceDirectoryDevice: dev_t?
        private var resourceDirectoryInode: ino_t?
        private let hostCapabilities: [String]

        init(hostJSON: String, resourceDirectory: URL?, localJournalDirectory: URL?, installationDirectory: URL?) {
            self.hostJSON = hostJSON
            self.resourceDirectory = resourceDirectory
            self.localJournalDirectory = localJournalDirectory
            self.installationDirectory = installationDirectory
            if let data = hostJSON.data(using: .utf8),
               let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                self.hostCapabilities = object["capabilities"] as? [String] ?? []
            } else { self.hostCapabilities = [] }
        }

        func review(directory: URL) async throws -> PackageReview {
            try await perform { try self.reviewOnQueue(directory: directory) }
        }

        private struct ManagedAccess {
            let owner: PluginReleaseCatalog?
            let choices: [String: PluginReleaseCatalog.Choice]
            let managedIDs: Set<String>
        }
        private func catalogOwner(exclusive: Bool) throws -> PluginReleaseCatalog? {
            if !exclusive, let pending = prepared.values.first { return try PluginReleaseCatalog(borrowing: pending.stage) }
            guard let directory = installationDirectory else { return nil }
            return try PluginReleaseCatalog(directory: directory, exclusive: exclusive)
        }
        private func managedAccess() throws -> ManagedAccess {
            guard let owner = try catalogOwner(exclusive: false) else {
                return ManagedAccess(owner: nil, choices: [:], managedIDs: managedPluginIDs)
            }
            let inventory = try owner.inventory(), selection = try owner.selection()
            guard inventory.issues.isEmpty else { throw PluginReleaseCatalog.Failure.invalid }
            for (id, choice) in selection.choices where choice.enabled {
                guard inventory.versions.contains(where: { $0.releaseID == choice.releaseID && $0.pluginID == id }) else {
                    throw PluginReleaseCatalog.Failure.invalid
                }
            }
            managedPluginIDs.formUnion(inventory.versions.map(\.pluginID))
            managedPluginIDs.formUnion(selection.choices.keys)
            return ManagedAccess(owner: owner, choices: selection.choices, managedIDs: managedPluginIDs)
        }
        private func admitted(_ record: Record, _ access: ManagedAccess, requireApproval: Bool) -> Bool {
            guard access.managedIDs.contains(record.review.id) else { return true }
            guard let choice = access.choices[record.review.id], choice.enabled,
                  record.managedReleaseID == choice.releaseID else { return false }
            return !requireApproval || record.approvalGeneration == choice.generation
        }
        private func versionLease(_ record: Record, access: ManagedAccess) throws -> PluginReleaseCatalog.VersionHandle? {
            guard let id = record.managedReleaseID else { return nil }
            guard let owner = access.owner else { throw PluginReleaseCatalog.Failure.inactive }
            let handle = try owner.openVersion(id)
            try handle.pin()
            return handle
        }
        private func catalogView(_ owner: PluginReleaseCatalog, selection: PluginReleaseCatalog.Selection? = nil) throws -> InstalledCatalog {
            let inventory = try owner.inventory(), current = try selection ?? owner.selection()
            var result = catalogView(inventory, selection: current)
            result.removalPending = try owner.hasPendingRemoval()
            return result
        }
        private func catalogView(_ inventory: PluginReleaseCatalog.Inventory, selection current: PluginReleaseCatalog.Selection) -> InstalledCatalog {
            return InstalledCatalog(revision: current.revision, versions: inventory.versions.map { version in
                let choice = current.choices[version.pluginID]
                return InstalledVersion(releaseID: version.releaseID, pluginID: version.pluginID, version: version.version,
                    publisher: version.publisher, packageSHA256: version.packageSHA256,
                    selected: choice?.releaseID == version.releaseID, enabled: choice?.enabled ?? false, inUse: version.inUse)
            }, issues: inventory.issues, maintenanceCount: inventory.maintenanceCount,
                selections: current.choices.keys.sorted().compactMap { id in
                    current.choices[id].map { InstalledSelection(pluginID: id, releaseID: $0.releaseID, enabled: $0.enabled) }
                })
        }
        func installedVersions() async throws -> InstalledCatalog {
            try await perform {
                try self.checkOpen()
                guard let owner = try self.catalogOwner(exclusive: false) else {
                    return InstalledCatalog(revision: nil, versions: [], issues: [], maintenanceCount: 0, selections: [])
                }
                return try self.catalogView(owner)
            }
        }
        private func invalidateSourceDecisions(pluginID: String) {
            for key in Array(packages.keys) where packages[key]?.review.id == pluginID {
                packages[key]?.source = .unknown
                packages[key]?.approvalGeneration = nil
            }
            managedPluginIDs.insert(pluginID)
        }
        func selectInstalledVersion(releaseID: String, expectedRevision: String?, trustedPolicy: URL) async throws -> VersionSelectionResult {
            try await perform {
                try self.checkOpen()
                guard let directory = self.installationDirectory,
                      let owner = try PluginReleaseCatalog(directory: directory, exclusive: true) else { throw PluginReleaseCatalog.Failure.root }
                guard try !owner.hasPendingRemoval() else { throw PluginReleaseRemoval.Failure.pending }
                var selection = try owner.selection()
                let inventory = try owner.inventory()
                guard selection.revision == expectedRevision else { throw PluginReleaseCatalog.Failure.conflict }
                let handle = try owner.openVersion(releaseID)
                let descriptor = try handle.read("release.json", limit: 65536)
                guard PluginReleaseCatalog.sha(descriptor) == releaseID else { throw PluginReleaseCatalog.Failure.invalid }
                let authenticated = try PluginDirectorySignatureVerifier.authenticate(descriptor: descriptor,
                    signature: handle.read("release.sig", limit: 64), trustedPolicy: PluginSignatureFile.read(trustedPolicy, limit: 131072))
                let payload = try handle.payload()
                defer { Darwin.close(payload) }
                let captured = try self.captureOnQueue(directoryFD: payload)
                do {
                    let metadata = try captured.package.metadata()
                    let proof = try authenticated.bind(.init(id: metadata.id, version: metadata.version,
                        publisher: metadata.publisher, packageSHA256: metadata.packageDigest))
                    try owner.validateVersion(releaseID, handle: handle, payload: payload)
                    let key = self.key(metadata.id, metadata.packageDigest)
                    let pinRuntime = self.runtimePackageKey == key && self.runtimeVersionLease == nil
                    let pinSession = self.sessionPackageKey == key && self.sessionVersionLease == nil
                    if pinRuntime || pinSession { try handle.pin() }
                    selection.choices[metadata.id] = PluginReleaseCatalog.Choice(releaseID: releaseID, enabled: true, generation: UUID().uuidString)
                    let (updated, durable) = try owner.writeSelection(selection, expected: expectedRevision)
                    self.invalidateSourceDecisions(pluginID: metadata.id)
                    if self.packages[key] == nil { self.packages[key] = captured }
                    else { try? captured.package.close() }
                    self.packages[key]?.managedReleaseID = releaseID
                    if pinRuntime { self.runtimeVersionLease = handle }
                    if pinSession { self.sessionVersionLease = handle }
                    let installed = InstalledRelease(review: captured.review, signature: proof,
                        directory: directory.appendingPathComponent(releaseID + "/payload", isDirectory: true), durabilityConfirmed: durable)
                    return VersionSelectionResult(release: installed, catalog: self.catalogView(inventory, selection: updated), durabilityConfirmed: durable)
                } catch { try? captured.package.close(); throw error }
            }
        }
        func disableInstalledPlugin(pluginID: String, expectedRevision: String?) async throws -> VersionSelectionResult {
            try await perform {
                try self.checkOpen()
                guard let directory = self.installationDirectory,
                      let owner = try PluginReleaseCatalog(directory: directory, exclusive: true) else { throw PluginReleaseCatalog.Failure.root }
                var selection = try owner.selection()
                let inventory = try owner.inventory()
                let pendingRemoval = try owner.hasPendingRemoval()
                guard selection.revision == expectedRevision else { throw PluginReleaseCatalog.Failure.conflict }
                guard let current = selection.choices[pluginID] else { throw PluginReleaseCatalog.Failure.inactive }
                selection.choices[pluginID] = PluginReleaseCatalog.Choice(releaseID: current.releaseID, enabled: false, generation: UUID().uuidString)
                let (updated, durable) = try owner.writeSelection(selection, expected: expectedRevision)
                self.invalidateSourceDecisions(pluginID: pluginID)
                var catalog = self.catalogView(inventory, selection: updated)
                catalog.removalPending = pendingRemoval
                return VersionSelectionResult(release: nil, catalog: catalog, durabilityConfirmed: durable)
            }
        }

        func previewVersionRemoval(releaseID: String, expectedRevision: String?) async throws -> RemovalPreview {
            try await perform {
                try self.checkOpen()
                guard self.removalPreview == nil else { throw PluginReleaseRemoval.Failure.pending }
                guard let owner = try self.catalogOwner(exclusive: true) else { throw PluginReleaseCatalog.Failure.root }
                let preview = try PluginReleaseRemoval(owner: owner, releaseID: releaseID, expectedRevision: expectedRevision)
                self.removalPreview = preview
                return RemovalPreview(id: preview.id, releaseID: releaseID, directory: preview.directory,
                    files: preview.members.map { RemovalPreview.File(path: $0.path, directory: $0.directory, bytes: $0.bytes, sha256: $0.sha256) }, bytes: preview.bytes)
            }
        }
        func installationResidues() async throws -> InstallationResidues {
            try await perform {
                try self.checkOpen()
                guard let owner = try self.catalogOwner(exclusive: false) else { return InstallationResidues(entries: [], issues: []) }
                let root = try owner.journalRoot(); defer { Darwin.close(root) }
                guard let journal = try PluginInstallJournal(parent: root) else { return InstallationResidues(entries: [], issues: []) }
                let listing = try journal.listing()
                return InstallationResidues(entries: listing.entries.map { entry in
                    InstallationResidue(id: entry.reference.id, digest: entry.reference.digest,
                        directory: owner.url.appendingPathComponent(entry.reference.name),
                        state: InstallationResidueState(rawValue: entry.state.rawValue)!,
                        publishedDirectory: entry.target.map { owner.url.appendingPathComponent($0) })
                }, issues: listing.issues)
            }
        }
        func unclassifiedEntries() async throws -> [UnclassifiedEntry] {
            try await perform {
                try self.checkOpen()
                guard let owner = try self.catalogOwner(exclusive: false) else { return [] }
                return try PluginUnclassifiedDirectories.listing(owner: owner).map { entry in
                    UnclassifiedEntry(name: entry.name, identity: entry.identity, directory: owner.url.appendingPathComponent(entry.name),
                        canPreview: entry.canPreview, issue: entry.issue)
                }
            }
        }
        func previewUnclassifiedDirectory(name: String, expectedIdentity: String) async throws -> RemovalPreview {
            try await perform {
                try self.checkOpen()
                guard self.removalPreview == nil else { throw PluginReleaseRemoval.Failure.pending }
                guard let owner = try self.catalogOwner(exclusive: true) else { throw PluginReleaseCatalog.Failure.root }
                let preview = try PluginReleaseRemoval(owner: owner, releaseID: PluginReleaseCatalog.sha(Data(name.utf8)),
                    expectedRevision: owner.selection().revision, unknownName: name, expectedIdentity: expectedIdentity)
                self.removalPreview = preview
                return RemovalPreview(id: preview.id, releaseID: preview.releaseID, directory: preview.directory,
                    files: preview.members.map { RemovalPreview.File(path: $0.path, directory: $0.directory, bytes: $0.bytes, sha256: $0.sha256) },
                    bytes: preview.bytes, unknownOrigin: true)
            }
        }
        func previewInstallationResidue(id: String, expectedDigest: String) async throws -> RemovalPreview {
            try await perform {
                try self.checkOpen()
                guard self.removalPreview == nil else { throw PluginReleaseRemoval.Failure.pending }
                guard let owner = try self.catalogOwner(exclusive: true) else { throw PluginReleaseCatalog.Failure.root }
                let root = try owner.journalRoot(); defer { Darwin.close(root) }
                guard let journal = try PluginInstallJournal(parent: root) else { throw PluginReleaseStage.Failure.changed }
                let reference = try journal.read(id).1
                guard reference.digest == expectedDigest else { throw PluginReleaseStage.Failure.changed }
                let preview = try PluginReleaseRemoval(owner: owner, releaseID: reference.digest,
                    expectedRevision: owner.selection().revision, stage: reference)
                self.removalPreview = preview
                return RemovalPreview(id: preview.id, releaseID: reference.digest, directory: preview.directory,
                    files: preview.members.map { RemovalPreview.File(path: $0.path, directory: $0.directory, bytes: $0.bytes, sha256: $0.sha256) }, bytes: preview.bytes)
            }
        }
        func retireInstallationRecord(id: String, expectedDigest: String) async throws {
            try await perform {
                try self.checkOpen()
                guard let owner = try self.catalogOwner(exclusive: true), try !owner.hasPendingRemoval() else { throw PluginReleaseRemoval.Failure.pending }
                let root = try owner.journalRoot(); defer { Darwin.close(root) }
                guard let journal = try PluginInstallJournal(parent: root) else { throw PluginReleaseStage.Failure.changed }
                let reference = try journal.read(id).1
                guard reference.digest == expectedDigest else { throw PluginReleaseStage.Failure.changed }
                try journal.retire(reference)
            }
        }
        func pendingVersionRemoval() async throws -> RemovalPreview? {
            try await perform {
                try self.checkOpen()
                guard let owner = try self.catalogOwner(exclusive: true),
                      let (id, releaseID, directory, members, unknown) = try PluginReleaseRemoval.pendingPreview(owner: owner) else { return nil }
                return RemovalPreview(id: id, releaseID: releaseID, directory: directory,
                    files: members.map { RemovalPreview.File(path: $0.path, directory: $0.directory, bytes: $0.bytes, sha256: $0.sha256) },
                    bytes: members.reduce(0) { $0 + $1.bytes }, unknownOrigin: unknown)
            }
        }
        func cancelPendingVersionRemoval(id: String) async throws -> Bool {
            try await perform {
                try self.checkOpen()
                guard self.removalPreview == nil else { throw PluginReleaseRemoval.Failure.pending }
                guard let owner = try self.catalogOwner(exclusive: true) else { throw PluginReleaseCatalog.Failure.root }
                return try PluginReleaseRemoval.cancelPending(owner: owner, id: id)
            }
        }
        func cancelVersionRemoval(id: String) async {
            _ = try? await perform { if self.removalPreview?.id == id { self.removalPreview = nil } }
        }
        private func removalResult(_ result: PluginReleaseRemoval.Result, owner: PluginReleaseCatalog) -> RemovalResult {
            var cleanupIssue: String?
            if result.complete, let reference = result.stage {
                do {
                    let root = try owner.journalRoot(); defer { Darwin.close(root) }
                    guard let journal = try PluginInstallJournal(parent: root) else { throw PluginReleaseStage.Failure.changed }
                    try journal.retire(reference)
                } catch { cleanupIssue = "目录操作已完成，安装归属记录保留待核对：\(error)" }
            }
            for key in Array(packages.keys) where result.stage == nil && result.unknownName == nil && packages[key]?.managedReleaseID == result.releaseID {
                guard var record = packages[key] else { continue }
                record.source = .unknown; record.approvalGeneration = nil; packages[key] = record
                if result.complete, key != runtimePackageKey, key != sessionPackageKey {
                    do {
                        if registeredDigests[record.review.id] == record.review.digest, let registry {
                            let status = try registry.remove(id: record.review.id, executor: false, expectedDigest: record.review.digest)
                            guard status == .ok else { throw ControllerError.registry(status.rawValue) }
                            registeredDigests.removeValue(forKey: record.review.id)
                        }
                        try record.package.close(); packages.removeValue(forKey: key)
                    } catch { cleanupIssue = (error as? LocalizedError)?.errorDescription ?? String(describing: error) }
                }
            }
            return RemovalResult(releaseID: result.releaseID, removed: result.removed, observedAbsent: result.observedAbsent,
                complete: result.complete, durabilityConfirmed: result.durabilityConfirmed, cleanupIssue: cleanupIssue)
        }
        func confirmVersionRemoval(id: String, acknowledgedUnknown: Bool) async throws -> RemovalResult {
            try await perform {
                try self.checkOpen()
                guard let preview = self.removalPreview, preview.id == id else { throw PluginReleaseRemoval.Failure.changed }
                guard !preview.unknownOrigin || acknowledgedUnknown else { throw PluginReleaseRemoval.Failure.confirmation }
                defer { self.removalPreview = nil }
                guard let owner = try self.catalogOwner(exclusive: true) else { throw PluginReleaseCatalog.Failure.root }
                return try self.removalResult(preview.confirm(owner: owner, acknowledgedUnknown: acknowledgedUnknown), owner: owner)
            }
        }
        func resumeVersionRemoval(id: String) async throws -> RemovalResult {
            try await perform {
                try self.checkOpen()
                guard self.removalPreview == nil else { throw PluginReleaseRemoval.Failure.pending }
                guard let owner = try self.catalogOwner(exclusive: true) else { throw PluginReleaseCatalog.Failure.root }
                guard try PluginReleaseRemoval.pendingPreview(owner: owner)?.0 == id else { throw PluginReleaseRemoval.Failure.changed }
                return try self.removalResult(PluginReleaseRemoval.resume(owner: owner), owner: owner)
            }
        }

        func prepareSignedRelease(artifact: URL, descriptor: URL, signature: URL,
                                  trustedPolicy: URL) async throws -> PreparedRelease {
            try await perform {
                try self.checkOpen()
                if let failed = self.retainedInstallStages.last ?? self.failedInstallDiscards.first.flatMap({ self.prepared[$0]?.stage }) {
                    throw InstallCleanupError(directory: failed.payloadURL.deletingLastPathComponent(), error: PluginReleaseStage.Failure.changed)
                }
                guard self.prepared.isEmpty else { throw PluginReleaseStage.Failure.busy }
                guard let directory = self.installationDirectory else { throw ControllerError.runtimeUnavailable }
                let payload = try PluginSignatureFile.read(descriptor, limit: 65536)
                let signed = try PluginSignatureFile.read(signature, limit: 64)
                let policy = try PluginSignatureFile.read(trustedPolicy, limit: 131072)
                let authentication = try PluginDirectorySignatureVerifier.authenticate(descriptor: payload,
                    signature: signed, trustedPolicy: policy)
                let archive = try PluginSignatureFile.read(artifact, limit: PluginReleaseArchive.limit)
                let members = try PluginReleaseArchive.inspect(archive, proof: authentication.proof)
                let stage = try PluginReleaseStage(directory: directory)
                var record: Record?
                do {
                    for member in members {
                        try stage.write(PluginReleaseArchive.contents(archive, member: member), path: "payload/" + member.path,
                            executable: member.executable)
                    }
                    try stage.write(payload, path: "release.json")
                    try stage.write(signed, path: "release.sig")
                    try stage.write(policy, path: "policy.json")
                    let captured = try self.captureOnQueue(directory: stage.payloadURL)
                    record = captured
                    let metadata = try captured.package.metadata()
                    let proof = try authentication.bind(.init(id: metadata.id, version: metadata.version,
                        publisher: metadata.publisher, packageSHA256: metadata.packageDigest))
                    guard let member = members.first(where: { $0.path == "plugin.json" }),
                          let manifest = try JSONSerialization.jsonObject(with: PluginReleaseArchive.contents(archive, member: member)) as? [String: Any],
                          let permissionValues = manifest["permissions"] as? [[String: Any]] else { throw ControllerError.invalidPackageURL }
                    // These display fields come from the same authenticated ZIP
                    // whose manifest Core checked, never from a reopened path.
                    let permissions = try permissionValues.map { value -> ReleasePermission in
                        guard let side = value["side"] as? String, let capability = value["capability"] as? String,
                              let purpose = value["purpose"] as? String, let scope = value["scope"] as? String else { throw ControllerError.invalidPackageURL }
                        let programs: [String]
                        if let value = value["programs"] {
                            guard let values = value as? [String] else { throw ControllerError.invalidPackageURL }; programs = values
                        } else { programs = [] }
                        return ReleasePermission(side: side, capability: capability, purpose: purpose, scope: scope, programs: programs)
                    }
                    let executor = try manifest["executor"].map { value in
                        String(decoding: try JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys]), as: UTF8.self)
                    }
                    let value = PreparedRelease(id: UUID().uuidString, review: captured.review, signature: proof,
                        sdkVersion: metadata.sdkVersion, protocols: metadata.protocols, permissions: permissions, executorDeclarations: executor)
                    self.prepared[value.id] = Prepared(value: value, record: captured, stage: stage)
                    return value
                } catch {
                    try? record?.package.close()
                    // If cleanup fails, report the retained namespace instead
                    // of silently declaring that failed preparation left none.
                    do { try stage.discard() }
                    catch {
                        self.retainedInstallStages.append(stage)
                        throw InstallCleanupError(directory: stage.payloadURL.deletingLastPathComponent(), error: error)
                    }
                    throw error
                }
            }
        }

        func installPreparedRelease(id: String) async throws -> InstalledRelease {
            try await perform {
                try self.checkOpen()
                guard let pending = self.prepared[id] else { throw ControllerError.invalidPackageURL }
                let result = try pending.stage.publish(id: pending.value.signature.descriptorSHA256)
                self.prepared.removeValue(forKey: id)
                let review = pending.value.review, packageKey = self.key(review.id, review.digest)
                if self.packages[packageKey] == nil { self.packages[packageKey] = pending.record }
                else { try? pending.record.package.close() }
                return InstalledRelease(review: review, signature: pending.value.signature,
                    directory: result.directory.appendingPathComponent("payload", isDirectory: true),
                    durabilityConfirmed: result.durabilityConfirmed, maintenanceIssue: pending.stage.maintenanceIssue)
            }
        }
        func discardPreparedRelease(id: String) async throws {
            try await perform { try self.discardPreparedOnQueue(id: id) }
        }
        private func discardPreparedOnQueue(id: String) throws {
            guard let pending = prepared[id] else { return }
            do {
                // Keep the Core owner valid until filesystem cleanup succeeds;
                // a transient failure must remain retryable on this same owner.
                try pending.stage.discard()
                try pending.record.package.close()
            } catch {
                failedInstallDiscards.insert(id)
                throw InstallCleanupError(directory: pending.stage.payloadURL.deletingLastPathComponent(), error: error)
            }
            prepared.removeValue(forKey: id)
            failedInstallDiscards.remove(id)
        }
        func retryFailedInstallCleanup() async throws {
            try await perform {
                for id in Array(self.failedInstallDiscards) { try self.discardPreparedOnQueue(id: id) }
                while let stage = self.retainedInstallStages.last {
                    do { try stage.discard(); self.retainedInstallStages.removeLast() }
                    catch { throw InstallCleanupError(directory: stage.payloadURL.deletingLastPathComponent(), error: error) }
                }
            }
        }

        func verifyDirectorySignature(packageID: String, digest: String, descriptor: URL,
                                      signature: URL, trustedPolicy: URL) async throws -> PluginDirectorySignature {
            try await perform {
                try self.checkOpen()
                guard let record = self.packages[self.key(packageID, digest)] else {
                    throw PluginDirectorySignatureVerifier.Failure.identity
                }
                let metadata = try record.package.metadata()
                return try PluginDirectorySignatureVerifier.verify(
                    descriptor: PluginSignatureFile.read(descriptor, limit: 65536),
                    signature: PluginSignatureFile.read(signature, limit: 64),
                    trustedPolicy: PluginSignatureFile.read(trustedPolicy, limit: 131072),
                    identity: .init(id: metadata.id, version: metadata.version,
                        publisher: metadata.publisher, packageSHA256: metadata.packageDigest))
            }
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
                let access = try self.managedAccess()
                defer { withExtendedLifetime(access.owner) {} }
                guard self.admitted(record, access, requireApproval: true) else { throw PluginReleaseCatalog.Failure.inactive }
                return try record.package.decodeCommandArguments(path: command, arguments: arguments)
            }
        }

        func openSSHChannel(hops input: [SSHHop], bootstrap: String) async throws -> ChannelState {
            try await perform {
                try self.checkOpen()
                guard self.channel == nil, self.session == nil, self.runtime == nil, self.localTemporary == nil else {
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
                self.local = false; self.lastLocalProcess = nil; self.lastLocalTemporary = nil
                self.channel = channel
                _ = try channel.step(milliseconds: 0)
                return try self.channelState(channel)
            }
        }

        func openLocalChannel(executable: String, arguments: [String], launch: ExtensionLocalLaunch,
                              environmentNames: [String]) async throws -> ChannelState {
            try await perform {
                try self.checkOpen()
                guard self.channel == nil, self.session == nil, self.runtime == nil, self.localTemporary == nil else { throw ControllerError.channelUnavailable }
                let names = try ExtensionRelayBootstrap.parseProcessEnvironmentJSON(
                    String(decoding: JSONEncoder().encode(environmentNames), as: UTF8.self))
                guard names.allSatisfy({ !$0.hasPrefix("LD_") && !$0.hasPrefix("DYLD_") && $0 != "NODE_OPTIONS" && $0 != "NODE_PATH" }) else {
                    throw GhostermLocalLaunchError.invalid("不接受动态加载器或 Node 注入环境")
                }
                let ambient = ProcessInfo.processInfo.environment
                var environment: [String] = []
                for name in ["HOME", "PATH", "XDG_CONFIG_HOME", "CODEX_HOME"] {
                    if let value = ambient[name] { environment.append(name + "=" + value) }
                }
                for name in names {
                    guard let value = ambient[name] else { throw GhostermLocalLaunchError.invalid("缺少显式选择的执行环境：\(name)") }
                    environment.append(name + "=" + value)
                }
                let channel = try launch.withValidatedDirectory(executable: executable) { fd, temporaryFD in
                    let temporary = try ExtensionLocalTemporaryDirectory(parentFD: temporaryFD, base: launch.temporaryDirectory, journal: self.journal())
                    self.local = true; self.lastLocalProcess = nil; self.localTemporary = temporary
                    self.lastLocalTemporary = temporary.observation
                    environment.append("TMPDIR=" + (try temporary.executionPath()))
                    return try ExtensionChannel(process: .init(executable: executable, arguments: arguments, directoryFD: fd,
                        environment: environment, executionAuthorized: launch.authorized, lifetimeMilliseconds: 600000,
                        stopGraceMilliseconds: 500, stdoutLimit: 67108864, stderrLimit: 1048576), owner: self.queue)
                }
                self.local = true; self.lastLocalProcess = nil; self.channel = channel
                return try self.channelState(channel)
            }
        }

        func localProcessState() async throws -> ProcessState? {
            try await perform { try self.captureLocalProcess(); return self.lastLocalProcess }
        }
        func localTemporaryState() async throws -> ExtensionLocalTemporaryState? {
            try await perform { self.localTemporary?.observation ?? self.lastLocalTemporary }
        }
        private func journal() throws -> ExtensionLocalJournal {
            if let localJournal { return localJournal }
            guard let localJournalDirectory else { throw GhostermLocalLaunchError.invalid("本机执行需要明确的持久预约记录目录") }
            let journal = try ExtensionLocalJournal(directory: localJournalDirectory)
            localJournal = journal; return journal
        }
        func inspectLocalReservations() async throws -> [ExtensionLocalJournalEntry] {
            try await perform { try self.checkOpen(); return try self.journal().inspect() }
        }
        func releaseMissingLocalReservation(_ entry: ExtensionLocalJournalEntry, confirmed: Bool) async throws {
            try await perform {
                try self.checkOpen()
                try self.journal().releaseMissing(id: entry.id, snapshot: entry.snapshot, confirmed: confirmed)
            }
        }

        private func captureLocalProcess() throws {
            guard local else { return }
            if let runtime { lastLocalProcess = try ProcessState(runtime.transportProcessObservation()) }
            else if let session { lastLocalProcess = try ProcessState(session.processObservation()) }
            else if let channel { lastLocalProcess = try ProcessState(channel.processObservation()) }
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
                             additionalRequiredCapabilities: [String], local: Bool = false) async throws -> SessionState {
            try await perform {
                try self.checkOpen()
                guard self.local == local, self.session == nil, self.runtime == nil, let channel = self.channel else {
                    throw ControllerError.channelUnavailable
                }
                let packageKey = self.key(pluginID, packageDigest)
                guard let record = self.packages[packageKey], record.source == .allowed else {
                    throw ControllerError.registry(ExtensionRegistry.Status.unknown.rawValue)
                }
                let access = try self.managedAccess()
                defer { withExtendedLifetime(access.owner) {} }
                guard self.admitted(record, access, requireApproval: true) else { throw PluginReleaseCatalog.Failure.inactive }
                guard executorDigest.utf8.count == 64,
                      executorDigest.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }),
                      !hostVersion.isEmpty, hostVersion.utf8.count <= 64,
                      !peerIdentity.isEmpty, peerIdentity.utf8.count <= 255,
                      !hostVersion.utf8.contains(0), !peerIdentity.utf8.contains(0),
                      (try channel.info()).phase == .ready else { throw ControllerError.channelUnavailable }
                let versionLease = try self.versionLease(record, access: access)
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
                let resourceStore = requiredCapabilities.contains("resource.receive")
                    ? try self.openResourceStoreIfNeeded() : nil
                let sessionAvailable = self.hostCapabilities + remoteProcessCapabilityList
                    .filter { requestedRemoteProcess && !self.hostCapabilities.contains($0) }
                let session = try ExtensionSession(channel: channel, role: .host,
                    config: .init(pluginID: metadata.id, pluginVersion: metadata.version,
                                  clientDigest: metadata.packageDigest, executorDigest: executorDigest,
                                  hostVersion: hostVersion, sdkVersion: metadata.sdkVersion,
                                  required: requiredCapabilities, available: sessionAvailable,
                                  peerKind: local ? .local : .ssh, peerIdentity: peerIdentity, methods: methods),
                    resourceStore: resourceStore)
                self.channel = nil
                self.session = session
                self.sessionPackageKey = packageKey
                self.sessionVersionLease = versionLease
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
                if allow {
                    let access = try self.managedAccess()
                    defer { withExtendedLifetime(access.owner) {} }
                    guard let record = self.packages[packageKey], record.source == .allowed,
                          self.admitted(record, access, requireApproval: true) else { throw PluginReleaseCatalog.Failure.inactive }
                    guard info.phase == .consent, info.consentToken == consentToken,
                          try session.decide(ticket: consentToken, decision: .allow) == 0 else { throw ControllerError.sessionUnavailable }
                    return try self.sessionState(session, packageKey: packageKey)
                }
                guard info.phase == .consent, info.consentToken == consentToken,
                      try session.decide(ticket: consentToken, decision: allow ? .allow : .deny) == 0 else {
                    throw ControllerError.sessionUnavailable
                }
                return try self.sessionState(session, packageKey: packageKey)
            }
        }

        func activateClient(pluginID: String, packageDigest: String, command: String,
                            arguments: [String], clientWorkspace: ClientWorkspace?) async throws -> RuntimeState {
            try await perform {
                try self.checkOpen()
                guard let session = self.session, let sessionKey = self.sessionPackageKey,
                      sessionKey == self.key(pluginID, packageDigest),
                      try session.info().phase == .active,
                      let record = self.packages[sessionKey], record.source == .allowed,
                      record.review.commands.contains(where: { $0.path == command }) else {
                    throw ControllerError.sessionUnavailable
                }
                let access = try self.managedAccess()
                defer { withExtendedLifetime(access.owner) {} }
                guard self.admitted(record, access, requireApproval: true) else { throw PluginReleaseCatalog.Failure.inactive }
                let selected = try self.registryOnQueue().resolve(command: command, executor: false,
                                                                   selectedID: pluginID,
                                                                   selectedDigest: packageDigest)
                guard selected.status == .ok, let exact = selected.package,
                      selected.command?.path == command else {
                    try? selected.package?.close()
                    throw ControllerError.registry(selected.status.rawValue)
                }
                try exact.close()
                var rootFD: Int32 = -1
                defer { if rootFD >= 0 { Darwin.close(rootFD) } }
                if let clientWorkspace {
                    try clientWorkspace.validate(requiredCapabilities: record.review.requiredCapabilities)
                    rootFD = clientWorkspace.directory.withCString {
                        open($0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                    }
                    var info = stat()
                    guard rootFD >= 0, fstat(rootFD, &info) == 0,
                          (info.st_mode & S_IFMT) == S_IFDIR else { throw ControllerError.resourceUnavailable }
                }
                let sessionInfo = try session.info()
                let registry = record.review.requiredCapabilities.contains("ui.views")
                var viewKinds: UInt32 = 0
                if registry {
                    for kind in selected.command?.views ?? [] {
                        let bit: UInt32 = kind == "table" ? 1 : kind == "diff" ? 2 : kind == "task" ? 4 : 0
                        guard bit != 0, record.review.requiredCapabilities.contains("ui." + kind) else { throw ControllerError.runtimeUnavailable }
                        viewKinds |= bit
                    }
                    guard viewKinds != 0, record.review.requiredCapabilities.contains("session.rpc") else { throw ControllerError.runtimeUnavailable }
                }
                let runtime = try ExtensionRuntime(session: session, package: record.package,
                                                   role: .host, associated: true,
                                                   invocation: .init(command: command, arguments: arguments))
                self.clientRootFD = rootFD; rootFD = -1
                self.session = nil; self.sessionPackageKey = nil
                self.runtime = runtime; self.runtimePackageKey = sessionKey
                self.runtimeVersionLease = self.sessionVersionLease; self.sessionVersionLease = nil
                self.runtimeCommand = command; self.runtimeInstanceID = UUID().uuidString
                self.runtimeViewScope = registry ? (sessionInfo.instance, String(sessionInfo.epoch)) : nil
                self.runtimeViewIssue = nil
                if registry {
                    do { try runtime.enableViews(kinds: viewKinds) }
                    catch {
                        // Session was already adopted. Retain the real owner for
                        // stop/release, rather than leaving a stale Session path.
                        self.runtimeViewIssue = "无法启用插件视图注册：\(error)"
                        runtime.stop()
                    }
                }
                return try self.runtimeState(runtime, packageKey: sessionKey, command: command)
            }
        }

        func publishClientResource(instanceID: String, root: String, relativePath: String) async throws -> RuntimeState {
            try await perform {
                try self.checkOpen()
                guard let runtime = self.runtime, let key = self.runtimePackageKey,
                      let command = self.runtimeCommand, try runtime.info().phase == .active,
                      self.runtimeInstanceID == instanceID,
                      self.clientRootFD >= 0, self.clientSource == nil else { throw ControllerError.resourceUnavailable }
                let admission: ExtensionFileSource.Admission
                if root != "client" {
                    self.clientPublication = ResourcePublication(id: UUID().uuidString, relativePath: relativePath,
                        phase: ExtensionFileSource.Phase.failed.rawValue, error: ExtensionFileSource.Status.denied.rawValue,
                        cleanupError: 0, confirmed: false, released: true, expired: false, sha256: "", bytes: 0, accepted: 0)
                } else {
                    admission = try runtime.openSource(rootFD: self.clientRootFD, relativePath: relativePath, byteLimit: 262144)
                    self.clientSource = admission.source
                    self.clientPublicationDeadline = ProcessInfo.processInfo.systemUptime + 10
                    self.clientPublication = ResourcePublication(id: UUID().uuidString, relativePath: relativePath,
                        phase: (admission.status == .ok ? ExtensionFileSource.Phase.hashing : .failed).rawValue,
                        error: admission.status.rawValue, cleanupError: 0, confirmed: false,
                        released: admission.source == nil, expired: false, sha256: "", bytes: 0, accepted: 0)
                }
                return try self.runtimeState(runtime, packageKey: key, command: command)
            }
        }

        func cancelClientPublication(id: String) async throws {
            try await perform {
                try self.checkOpen()
                guard self.clientPublication?.id == id, let source = self.clientSource else { throw ControllerError.resourceUnavailable }
                try source.cancel()
            }
        }

        private func stepClientPublication() throws {
            guard let source = clientSource, var publication = clientPublication else { return }
            if ProcessInfo.processInfo.systemUptime >= clientPublicationDeadline {
                publication.expired = true; try source.cancel()
            }
            let phase = try source.step()
            if phase == .ready && !publication.expired {
                let status = try source.send()
                guard status == .ok || status == .wait else { throw ControllerError.resourceUnavailable }
            }
            let info = try source.info()
            publication.phase = info.phase.rawValue; publication.error = info.error.rawValue
            publication.cleanupError = info.cleanupError.rawValue
            publication.confirmed = info.transfer.confirmed
            publication.sha256 = info.descriptor.sha256; publication.bytes = info.descriptor.size
            publication.accepted = info.accepted
            if [.succeeded, .failed, .stopped, .uncertain].contains(info.phase) {
                let closed = try source.closeStep()
                if closed == .ok { clientSource = nil; publication.released = true }
                else if closed != .wait { throw ControllerError.resourceUnavailable }
            }
            clientPublication = publication
        }

        private func closeClientSource() throws {
            guard let source = clientSource else { return }
            try source.cancel()
            let status = try source.closeStep()
            if status == .ok { clientSource = nil }
            else if status != .wait { throw ControllerError.resourceUnavailable }
        }

        func stepRuntime() async throws -> RuntimeState {
            try await perform {
                try self.checkOpen()
                guard let runtime = self.runtime, let packageKey = self.runtimePackageKey,
                      let command = self.runtimeCommand else { throw ControllerError.runtimeUnavailable }
                // One native step consumes at most one wire frame. Drain a
                // bounded batch on this worker so the UI snapshot cadence does
                // not impose a 50 ms delay on every resource fragment. The wall
                // budget applies between calls; each call retains its CPU guard.
                let deadline = ProcessInfo.processInfo.systemUptime + 0.005
                for _ in 0..<32 {
                    try self.stepClientPublication()
                    let phase = try runtime.step()
                    if phase != .starting && phase != .active {
                        try self.clientSource?.cancel(); try self.stepClientPublication(); break
                    }
                    if ProcessInfo.processInfo.systemUptime >= deadline { break }
                }
                return try self.runtimeState(runtime, packageKey: packageKey, command: command)
            }
        }
        func closeRuntimeView(instanceID: String, handle: String) async throws -> RuntimeState {
            try await perform {
                try self.checkOpen()
                guard let runtime = self.runtime, self.runtimeInstanceID == instanceID,
                      self.runtimeViewScope != nil, try runtime.info().phase == .active,
                      let packageKey = self.runtimePackageKey, let command = self.runtimeCommand else { throw ControllerError.runtimeUnavailable }
                try runtime.closeView(handle: handle)
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
            let deadline = ProcessInfo.processInfo.systemUptime + 2
            while true {
                do {
                    try await perform { try self.checkOpen(); try self.releaseOwners(forceStop: false) }
                    return
                } catch ControllerError.localProcessDraining {
                    guard ProcessInfo.processInfo.systemUptime < deadline else { throw ControllerError.localProcessDraining }
                    try await Task.sleep(nanoseconds: 10_000_000)
                }
            }
        }

        func close() async -> Bool {
            let deadline = ProcessInfo.processInfo.systemUptime + 2
            while true {
                let result = await closeOnce()
                if result != 0 { return result == 1 }
                guard ProcessInfo.processInfo.systemUptime < deadline else { return false }
                do { try await Task.sleep(nanoseconds: 10_000_000) } catch { return false }
            }
        }

        private func closeOnce() async -> Int {
            (try? await perform {
                self.closed = true
                self.removalPreview = nil
                if self.closeComplete { return self.closeResult ? 1 : -1 }
                do {
                    try self.releaseOwners(forceStop: true)
                    var cleanupFailed = (self.lastLocalTemporary?.error ?? 0) != 0
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
                    for id in Array(self.prepared.keys) {
                        do { try self.discardPreparedOnQueue(id: id) } catch { cleanupFailed = true }
                    }
                    while let stage = self.retainedInstallStages.last {
                        do { try stage.discard(); self.retainedInstallStages.removeLast() }
                        catch { cleanupFailed = true; break }
                    }
                    self.closeComplete = self.prepared.isEmpty && self.retainedInstallStages.isEmpty
                    self.closeResult = !cleanupFailed
                    return self.closeResult ? 1 : -1
                } catch ControllerError.localProcessDraining { return 0 }
                catch { return -1 }
            }) ?? -1
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
                                }, diagnostic: String(decoding: info.diagnostic, as: UTF8.self), process: try processSnapshot(), temporary: localTemporary?.observation ?? lastLocalTemporary)
        }

        private func processSnapshot() throws -> ProcessState? {
            try captureLocalProcess(); return local ? lastLocalProcess : nil
        }

        private func sessionState(_ session: ExtensionSession, packageKey: String) throws -> SessionState {
            let info = try session.info()
            guard let record = packages[packageKey] else { throw ControllerError.sessionUnavailable }
            return SessionState(packageID: record.review.id, packageDigest: record.review.digest,
                                phase: info.phase.rawValue, end: info.end.rawValue, error: info.error,
                                cleanupError: info.cleanupError, consentToken: info.consentToken, process: try processSnapshot(), temporary: localTemporary?.observation ?? lastLocalTemporary)
        }

        private func runtimeState(_ runtime: ExtensionRuntime, packageKey: String,
                                  command: String) throws -> RuntimeState {
            let info = try runtime.info()
            guard let record = packages[packageKey] else { throw ControllerError.runtimeUnavailable }
            return RuntimeState(packageID: record.review.id, packageDigest: record.review.digest,
                                command: command, phase: info.phase.rawValue, failure: info.error.rawValue,
                                cleanupError: info.cleanupError, disposeError: info.disposeError ? 1 : 0, publication: clientPublication, instanceID: runtimeInstanceID ?? "", process: try processSnapshot(), temporary: localTemporary?.observation ?? lastLocalTemporary,
                                views: runtimeViewScope != nil && info.phase == .active && runtimeViewIssue == nil ? try runtime.viewsSnapshot() : nil,
                                viewSessionInstance: runtimeViewScope?.instance, viewSessionEpoch: runtimeViewScope?.epoch, viewIssue: runtimeViewIssue)
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
                for _ in 0..<32 {
                    try closeClientSource()
                    let done = try runtime.closeStep(); try captureLocalProcess()
                    if done { released = true; break }
                }
                guard released, clientSource == nil else { throw local ? ControllerError.localProcessDraining : ControllerError.runtimeUnavailable }
                self.runtime = nil; runtimePackageKey = nil; runtimeCommand = nil; runtimeInstanceID = nil
                runtimeVersionLease = nil
                runtimeViewScope = nil; runtimeViewIssue = nil
                clientPublication = nil
                if clientRootFD >= 0 {
                    let fd = clientRootFD; clientRootFD = -1
                    guard Darwin.close(fd) == 0 else { throw ControllerError.resourceUnavailable }
                }
            }
            if let session {
                if forceStop { session.stop() }
                else {
                    let phase = try session.info().phase
                    guard phase == .ended || phase == .closed else { throw ControllerError.sessionUnavailable }
                }
                var released = false
                for _ in 0..<32 {
                    let done = try session.closeStep(); try captureLocalProcess()
                    if done { released = true; break }
                }
                guard released else { throw local ? ControllerError.localProcessDraining : ControllerError.sessionUnavailable }
                self.session = nil; sessionPackageKey = nil; sessionVersionLease = nil
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
                for _ in 0..<32 {
                    let done = try channel.closeStep(); try captureLocalProcess()
                    if done { released = true; break }
                }
                guard released else { throw local ? ControllerError.localProcessDraining : ControllerError.channelUnavailable }
                self.channel = nil
            }
            if let temporary = localTemporary {
                do {
                    let done = try temporary.closeStep(); lastLocalTemporary = temporary.observation
                    guard done else { throw ControllerError.localProcessDraining }
                    self.localTemporary = nil
                } catch { lastLocalTemporary = temporary.observation; throw error }
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
            let record = try captureOnQueue(directory: directory)
            let packageKey = key(record.review.id, record.review.digest)
            guard packages[packageKey] == nil else {
                try? record.package.close()
                throw ControllerError.registry(ExtensionRegistry.Status.conflict.rawValue)
            }
            packages[packageKey] = record
            return record.review
        }

        private func captureOnQueue(directory: URL) throws -> Record {
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

            return try captureOnQueue(directoryFD: fd)
        }
        private func captureOnQueue(directoryFD fd: Int32) throws -> Record {
            try checkOpen()

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
                let review = PackageReview(id: metadata.id, version: metadata.version,
                                           publisher: metadata.publisher, license: metadata.license,
                                           digest: metadata.packageDigest, runtime: metadata.runtime,
                                           requiredCapabilities: metadata.requires,
                                           optionalMissingCapabilities: metadata.optionalMissing,
                                           authorizationGranted: metadata.authorizationGranted,
                                           publisherAuthenticated: metadata.publisherAuthenticated,
                                           commands: commands,
                                           sourceApproved: false)
                return Record(package: package, review: review, source: .unknown)
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
                let access = try managedAccess()
                defer { withExtendedLifetime(access.owner) {} }
                guard admitted(record, access, requireApproval: false) else { return ExtensionRegistry.Status.denied.rawValue }
                let expected = record.source == .allowed ? digest : (record.managedReleaseID == nil ? nil : registeredDigests[id])
                let status = try registryOnQueue().put(record.package, source: .allowed,
                                                       expectedDigest: expected)
                guard status == .ok else { return status.rawValue }
                record.source = .allowed
                record.approvalGeneration = access.choices[id]?.generation
                registeredDigests[id] = digest
                packages[key(id, digest)] = record
                return ExtensionRegistry.Status.ok.rawValue
            }
            if record.source == .allowed, registeredDigests[id] == digest {
                let status = try registryOnQueue().remove(id: id, executor: false,
                                                          expectedDigest: digest)
                guard status == .ok else { return status.rawValue }
                registeredDigests.removeValue(forKey: id)
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
            let access = try packages.isEmpty
                ? ManagedAccess(owner: nil, choices: [:], managedIDs: managedPluginIDs) : managedAccess()
            defer { withExtendedLifetime(access.owner) {} }
            let matches = packages.values.filter { record in
                guard record.source != .denied else { return false }
                guard admitted(record, access, requireApproval: false) else { return false }
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
                guard admitted(chosen, access, requireApproval: chosen.source == .allowed) else {
                    return Resolution(status: ExtensionRegistry.Status.denied.rawValue,
                        packageID: chosen.review.id, packageDigest: chosen.review.digest, command: nil)
                }
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
