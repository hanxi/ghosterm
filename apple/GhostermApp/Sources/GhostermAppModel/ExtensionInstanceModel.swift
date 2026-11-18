import Combine
import CoreFoundation
import Foundation
import GhostermCore

/// One plugin invocation, with its own RPC generation, approvals, resources and
/// native view state. Workspace focus never selects the recipient of a reply.
@MainActor public final class ExtensionInstanceModel: ObservableObject, Identifiable {
    public let id = UUID()
    public struct ExtensionRun: Sendable {
        public let packageID: String
        public let packageDigest: String
        public let command: String
        public let arguments: [String]
        public let decodedArguments: String?
        public let executorDigest: String
        public let peerIdentity: String
        public let remoteCommand: String
        public let executorExecution: ExtensionRelayBootstrap.Execution?
        public let executorAttachment: ExtensionRelayBootstrap.Attachment?
        public let localLaunch: ExtensionLocalLaunch?
        public let clientWorkspace: ExtensionController.ClientWorkspace?
        public let methods: [String]
        public let hostMethods: [String]
        public let requiredCapabilities: [String]
        public let session: ExtensionController.SessionState?
        public let runtime: ExtensionController.RuntimeState?
        public let receivedResources: [ExtensionController.ReceivedResource]
        public let codexEvents: [String]
    }

    public struct CodexPendingApproval: Identifiable, Sendable, Equatable {
        public let id: String
        public let method: String
        public let paramsJSON: String
        public let itemID: String?
        public let identity: CodexApprovalIdentity?
        public let permissionReviewID: String?
        public var stableID: String { Data(id.utf8).base64EncodedString() }
    }

    public let extensionController: ExtensionController?
    var connectSSH: @MainActor (String, String) async throws -> ExtensionController.ChannelState = { _, _ in
        throw GhostermError.failed("插件 SSH owner 已释放")
    }
    public init(controller: ExtensionController?) { self.extensionController = controller }

    @Published public private(set) var extensionLastLocalProcess: ExtensionController.ProcessState?
    @Published public private(set) var extensionLastLocalTemporary: ExtensionLocalTemporaryState?
    @Published public private(set) var extensionRun: ExtensionRun?
    @Published public private(set) var extensionRunBusy = false
    @Published public private(set) var extensionRunIssue: String?
    @Published public private(set) var runtimeObservationIssue: String?
    public var canInteractWithRuntime: Bool {
        runtimeObservationIssue == nil && !closeRequested && !extensionRunBusy &&
        extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue
    }
    public var runtimeStateIsUnknown: Bool {
        guard runtimeObservationIssue != nil else { return false }
        guard let runtime = extensionRun?.runtime else { return true }
        return Self.runtimeIsRunning(runtime.phase)
    }
    @Published public private(set) var codexThreadID: String?
    @Published public var codexMessageDraft = ""
    @Published public private(set) var codexCallBusy = false
    @Published public private(set) var codexTurnActive = false
    @Published public private(set) var codexSendUnconfirmed = false
    @Published public private(set) var codexPendingApprovals: [CodexPendingApproval] = []
    @Published public private(set) var codexTimeline: [CodexTimelineEntry] = []
    @Published public private(set) var codexChangeReview: CodexChangeReviewState?
    private var codexChangeReadGeneration = UUID()
    private var codexChangeReadTask: Task<Void, Never>?
    private var extensionRunGeneration = UUID()
    private var extensionRunInvocation: (packageID: String, digest: String, command: String, arguments: [String], clientWorkspace: ExtensionController.ClientWorkspace?)?
    private var lastCodexEventSequence: UInt64 = 0
    private var pendingExtensionRPCHandles: Set<UInt64> = []
    private var claudeRPC: ClaudeRPCClient?
    @Published public private(set) var extensionTable: PluginTableModel?
    @Published public private(set) var extensionDiff: PluginDiffModel?
    @Published public private(set) var extensionTask: PluginTaskModel?
    @Published public private(set) var extensionRegisteredViews: PluginViewsModel?
    private var extensionViews: [String] = []
    @Published public private(set) var claudeConversation: ClaudeConversation?
    @Published public private(set) var closeRequested = false
    @Published public private(set) var cleanupPending = false
    private var launchTask: Task<Void, Never>?
    private var runtimeTask: Task<Void, Never>?
    private var launchGeneration = UUID()
    public var canDiscard: Bool { !extensionRunBusy && launchTask == nil && extensionRun == nil && !cleanupPending }
    public var closeNeedsConfirmation: Bool { !closeRequested && !canDiscard }

    /// Closing keeps this owner until startup, Runtime and local cleanup finish.
    /// A failed cleanup leaves the pane available for an explicit retry.
    @discardableResult public func requestClose() async -> Bool {
        closeRequested = true
        launchTask?.cancel()
        if let launchTask {
            let token = launchGeneration
            await launchTask.value
            if launchGeneration == token { self.launchTask = nil }
        }
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while extensionRunBusy && ProcessInfo.processInfo.systemUptime < deadline {
            do { try await Task.sleep(nanoseconds: 10_000_000) } catch { return false }
        }
        guard !extensionRunBusy else { return false }
        if extensionRun != nil { await stopExtensionRun() }
        if cleanupPending { await retryLocalExtensionCleanup() }
        return canDiscard
    }

    struct ClientPlan { let methods: [String], services: [String] }
    static func clientPlan(selected: ExtensionController.Resolution, methods: [String], hostMethods: [String]) throws -> ClientPlan {
    let tableContribution = selected.command?.views.contains("table") == true
    if tableContribution && !["session.rpc", "ui.table"].allSatisfy(selected.requiredCapabilities.contains) {
        throw GhostermError.failed("表格贡献必须声明 session.rpc 与 ui.table")
    }
    let diffContribution = selected.command?.views.contains("diff") == true
    if diffContribution && !["session.rpc", "ui.diff"].allSatisfy(selected.requiredCapabilities.contains) {
        throw GhostermError.failed("差异贡献必须声明 session.rpc 与 ui.diff")
    }
    let taskContribution = selected.command?.views.contains("task") == true
    if taskContribution && !["session.rpc", "ui.task"].allSatisfy(selected.requiredCapabilities.contains) {
        throw GhostermError.failed("任务贡献必须声明 session.rpc 与 ui.task")
    }
    let registeredViews = selected.requiredCapabilities.contains("ui.views")
    if registeredViews && !(tableContribution || diffContribution || taskContribution) {
        throw GhostermError.failed("视图注册服务需要所选命令声明标准视图")
    }
    var sessionMethods = !registeredViews && tableContribution ? Array(Set(methods + PluginTableModel.methods)).sorted() : methods
    if !registeredViews && diffContribution { sessionMethods = Array(Set(sessionMethods + PluginDiffModel.methods)).sorted() }
    if !registeredViews && taskContribution { sessionMethods = Array(Set(sessionMethods + PluginTaskModel.methods)).sorted() }
    var clientServices = tableContribution ? ["ui.table"] : []
    if diffContribution { clientServices.append("ui.diff") }
    if taskContribution { clientServices.append("ui.task") }
    if registeredViews { clientServices.append("ui.views") }
    guard (sessionMethods.isEmpty && hostMethods.isEmpty) || selected.requiredCapabilities.contains("session.rpc") else {
        throw GhostermError.failed("此 Client 包未声明 session.rpc，不能协商 RPC 方法")
    }
        return ClientPlan(methods: sessionMethods, services: clientServices)
    }

    public func startExtensionClient(packageID: String, packageDigest: String, command: String,
                                     arguments: [String], sshAlias: String, executable: String,
                                     executorDirectory: String, executorDigest: String,
                                     methods: [String], hostMethods: [String],
                                     scopedRoots: [ExtensionRelayBootstrap.ScopedRoot] = [],
                                     processPrograms: [ExtensionRelayBootstrap.ProcessProgram] = [],
                                     processEnvironmentNames: [String] = [],
                                     executorExecution: ExtensionRelayBootstrap.Execution? = nil,
                                     executorAttachment: ExtensionRelayBootstrap.Attachment? = nil,
                                     executorReleaseEvidence: ExtensionRelayBootstrap.ReleaseEvidence? = nil,
                                     executorVariantID: String? = nil,
                                     clientWorkspace: ExtensionController.ClientWorkspace? = nil,
                                     localLaunch: ExtensionLocalLaunch? = nil, multiplexedConnectionCommand: String? = nil,
                                     onValidated: (@MainActor () throws -> Void)? = nil) async {
        guard launchTask == nil, !extensionRunBusy, extensionRun == nil, !cleanupPending else {
            extensionRunIssue = "已有插件运行或资源仍待释放"; return
        }
        closeRequested = false
        runtimeObservationIssue = nil
        let token = UUID(); launchGeneration = token
        let task = Task {
            await self.startClient(packageID: packageID, packageDigest: packageDigest, command: command,
                arguments: arguments, sshAlias: sshAlias, executable: executable,
                executorDirectory: executorDirectory, executorDigest: executorDigest,
                methods: methods, hostMethods: hostMethods, scopedRoots: scopedRoots,
                processPrograms: processPrograms, processEnvironmentNames: processEnvironmentNames,
                executorExecution: executorExecution, executorAttachment: executorAttachment, executorReleaseEvidence: executorReleaseEvidence, executorVariantID: executorVariantID,
                clientWorkspace: clientWorkspace, localLaunch: localLaunch, multiplexedConnectionCommand: multiplexedConnectionCommand,
                onValidated: onValidated)
        }
        launchTask = task
        await withTaskCancellationHandler(operation: { await task.value }, onCancel: { task.cancel() })
        if launchGeneration == token { launchTask = nil }
    }

    private func startClient(packageID: String, packageDigest: String, command: String,
                             arguments: [String], sshAlias: String, executable: String,
                             executorDirectory: String, executorDigest: String,
                             methods: [String], hostMethods: [String],
                             scopedRoots: [ExtensionRelayBootstrap.ScopedRoot],
                             processPrograms: [ExtensionRelayBootstrap.ProcessProgram],
                             processEnvironmentNames: [String], executorExecution: ExtensionRelayBootstrap.Execution?,
                             executorAttachment: ExtensionRelayBootstrap.Attachment?,
                             executorReleaseEvidence: ExtensionRelayBootstrap.ReleaseEvidence?,
                             executorVariantID: String?,
                             clientWorkspace: ExtensionController.ClientWorkspace?, localLaunch: ExtensionLocalLaunch?, multiplexedConnectionCommand: String?,
                             onValidated: (@MainActor () throws -> Void)?) async {
        guard let extensionController else { extensionRunIssue = "此构建未启用插件运行时"; return }
        guard !extensionRunBusy, extensionRun == nil else { extensionRunIssue = "已有插件运行或授权流程"; return }
        extensionRunBusy = true; extensionRunIssue = nil; extensionLastLocalProcess = nil; extensionLastLocalTemporary = nil
        do {
            try Task.checkCancellation()
            let selected = try await extensionController.resolve(command: command, selectedID: packageID,
                                                                  selectedDigest: packageDigest)
            guard selected.status == 0, selected.packageID == packageID,
                  selected.packageDigest == packageDigest, selected.command?.path == command else {
                throw GhostermError.failed("只能启动本进程已批准摘要中的精确 Client 命令")
            }
            let plan = try Self.clientPlan(selected: selected, methods: methods, hostMethods: hostMethods)
            let sessionMethods = plan.methods, clientServices = plan.services
            try clientWorkspace?.validate(requiredCapabilities: selected.requiredCapabilities)
            if ["ai codex", "ai claude"].contains(command), let executorAttachment, !executorAttachment.remoteProcesses {
                throw GhostermError.failed("此命令需要明确允许沿用前台实例已有的远端业务进程范围")
            }
            let decodedArguments = try await extensionController.decodeArguments(packageID: packageID,
                packageDigest: packageDigest, command: command, arguments: arguments)
            let request = ExtensionRelayBootstrap.Request(executable: executable,
                executorDirectory: executorDirectory, executorDigest: executorDigest,
                clientDigest: packageDigest, methods: sessionMethods, hostMethods: hostMethods,
                clientCapabilities: clientServices,
                scopedRoots: scopedRoots, processPrograms: processPrograms,
                processEnvironmentNames: processEnvironmentNames, execution: executorExecution, attachment: executorAttachment, releaseEvidence: executorReleaseEvidence, variantID: executorVariantID,
                command: command, arguments: arguments)
            let argv = try localLaunch.flatMap { _ in multiplexedConnectionCommand == nil ? try ExtensionRelayBootstrap.buildLocal(request) : nil }
            let bootstrap = try multiplexedConnectionCommand ?? argv.map { String(decoding: try JSONEncoder().encode($0), as: UTF8.self) } ?? ExtensionRelayBootstrap.build(request)
            if let localLaunch, let executorExecution, localLaunch.temporaryDirectory != executorExecution.temporaryDirectory {
                throw GhostermError.failed("本机宿主与 Executor 必须使用同一显式临时目录")
            }
            let peerIdentity = localLaunch.map { "local:" + $0.executableSHA256 } ?? sshAlias
            let additionalCapabilities = ((scopedRoots.isEmpty && clientWorkspace == nil && executorAttachment?.scopedFiles != true) || !selected.requiredCapabilities.contains("resource.receive")
                ? [] : ["file.readScoped"]) +
                (processPrograms.isEmpty && executorAttachment?.remoteProcesses != true ? [] : ["process.spawn", "process.pipes"])
            try Task.checkCancellation()
            try localLaunch?.validateConfiguration(executable: executable)
            try onValidated?()
            if multiplexedConnectionCommand != nil {
                // The connection owner admitted this exact logical channel.
            } else if let localLaunch, let argv {
                _ = try await extensionController.openLocalChannel(executable: executable, arguments: argv, launch: localLaunch, environmentNames: processEnvironmentNames)
            } else {
                _ = try await connectSSH(sshAlias, bootstrap)
            }
            try Task.checkCancellation()
            let rpcMethods = sessionMethods.map {
                ExtensionController.RPCMethod(name: $0, capability: "session.rpc",
                    receiver: GhostermCore.ExtensionSession.Role.executor.rawValue)
            } + hostMethods.map {
                ExtensionController.RPCMethod(name: $0, capability: "session.rpc",
                    receiver: GhostermCore.ExtensionSession.Role.host.rawValue)
            }
            let version = AppModel.extensionHostProtocolVersion(
                Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String)
            let session: ExtensionController.SessionState
            if localLaunch != nil {
                session = try await extensionController.adoptLocalSession(pluginID: packageID, packageDigest: packageDigest, executorDigest: executorDigest, hostVersion: version, peerIdentity: peerIdentity, methods: rpcMethods, additionalRequiredCapabilities: additionalCapabilities)
            } else {
                session = try await extensionController.adoptSSHSession(pluginID: packageID, packageDigest: packageDigest, executorDigest: executorDigest, hostVersion: version, peerIdentity: peerIdentity, methods: rpcMethods, additionalRequiredCapabilities: additionalCapabilities)
            }
            extensionRunInvocation = (packageID, packageDigest, command, arguments, clientWorkspace)
            extensionRegisteredViews?.invalidate()
            extensionTable?.invalidate(); extensionTable = nil
            extensionDiff?.invalidate(); extensionDiff = nil
            extensionTask?.invalidate(); extensionTask = nil
            extensionRegisteredViews = nil
            extensionViews = selected.command?.views ?? []
            claudeRPC?.invalidate(); claudeRPC = nil
            claudeConversation?.invalidate(); claudeConversation = nil
            extensionRunGeneration = UUID()
            codexCallBusy = false
            lastCodexEventSequence = 0
            codexThreadID = nil
            codexMessageDraft = ""
            codexTurnActive = false
            codexSendUnconfirmed = false
            codexPendingApprovals = []
            codexTimeline = []
            cancelCodexChangeReview()
            extensionRun = ExtensionRun(packageID: packageID, packageDigest: packageDigest,
                command: command, arguments: arguments, decodedArguments: decodedArguments,
                executorDigest: executorDigest,
                peerIdentity: peerIdentity, remoteCommand: bootstrap, executorExecution: executorExecution, executorAttachment: executorAttachment, localLaunch: localLaunch, clientWorkspace: clientWorkspace, methods: sessionMethods, hostMethods: hostMethods,
                requiredCapabilities: Array(Set(selected.requiredCapabilities + additionalCapabilities)).sorted(),
                session: session, runtime: nil, receivedResources: [], codexEvents: [])
            let deadline = ProcessInfo.processInfo.systemUptime + 60
            var current = session
            while current.phase != GhostermCore.ExtensionSession.Phase.consent.rawValue &&
                  current.phase != GhostermCore.ExtensionSession.Phase.active.rawValue &&
                  current.phase != GhostermCore.ExtensionSession.Phase.ended.rawValue &&
                  current.phase != GhostermCore.ExtensionSession.Phase.closed.rawValue {
                try Task.checkCancellation()
                guard ProcessInfo.processInfo.systemUptime < deadline else {
                    throw GhostermError.failed("插件 Session 握手超过 60 秒")
                }
                current = try await extensionController.stepSession(milliseconds: 10)
                updateExtensionRun(session: current)
                try await Task.sleep(nanoseconds: 20_000_000)
            }
            guard current.phase == GhostermCore.ExtensionSession.Phase.consent.rawValue else {
                throw GhostermError.failed("插件 Session 未进入待授权状态（phase \(current.phase), end \(current.end), error \(current.error), cleanup \(current.cleanupError)）")
            }
        } catch {
            extensionRunIssue = "插件启动失败：\(error)"
            try? await extensionController.stopSession()
            try? await extensionController.stopSSHChannel()
            do { try await extensionController.releaseFinished(); cleanupPending = false }
            catch { cleanupPending = true; extensionRunIssue = "插件启动结束，资源仍待释放：\(error)" }
            extensionLastLocalProcess = try? await extensionController.localProcessState()
            extensionLastLocalTemporary = try? await extensionController.localTemporaryState()
            extensionRun = nil; extensionRunInvocation = nil
        }
        extensionRunBusy = false
    }

    public func decideExtensionConsent(allow: Bool) async {
        guard let extensionController, let run = extensionRun,
              let session = run.session,
              session.phase == GhostermCore.ExtensionSession.Phase.consent.rawValue,
              !session.consentToken.isEmpty, !extensionRunBusy, !closeRequested else { return }
        extensionRunBusy = true; extensionRunIssue = nil
        do {
            var current = try await extensionController.decideSession(consentToken: session.consentToken, allow: allow)
            updateExtensionRun(session: current)
            let deadline = ProcessInfo.processInfo.systemUptime + 60
            if allow {
                while current.phase != GhostermCore.ExtensionSession.Phase.active.rawValue &&
                      current.phase != GhostermCore.ExtensionSession.Phase.ended.rawValue &&
                      current.phase != GhostermCore.ExtensionSession.Phase.closed.rawValue {
                    try Task.checkCancellation()
                    guard ProcessInfo.processInfo.systemUptime < deadline else {
                        throw GhostermError.failed("插件 Session 授权握手超过 60 秒")
                    }
                    current = try await extensionController.stepSession(milliseconds: 10)
                    updateExtensionRun(session: current)
                    try await Task.sleep(nanoseconds: 20_000_000)
                }
                guard current.phase == GhostermCore.ExtensionSession.Phase.active.rawValue,
                      let invocation = extensionRunInvocation else {
                    throw GhostermError.failed("插件 Session 未进入 ACTIVE")
                }
                let runtime = try await extensionController.activateClient(pluginID: invocation.packageID,
                    packageDigest: invocation.digest, command: invocation.command,
                    arguments: invocation.arguments, clientWorkspace: invocation.clientWorkspace)
                updateExtensionRun(runtime: runtime)
                runtimeTask = Task { await self.advanceExtensionRuntime() }
            } else {
                while current.phase != GhostermCore.ExtensionSession.Phase.ended.rawValue &&
                      current.phase != GhostermCore.ExtensionSession.Phase.closed.rawValue {
                    current = try await extensionController.stepSession(milliseconds: 10)
                    updateExtensionRun(session: current)
                    try await Task.sleep(nanoseconds: 20_000_000)
                }
            }
        } catch { extensionRunIssue = "插件授权失败：\(error)" }
        extensionRunBusy = false
    }

    func stopIfPublisherRevoked() async throws -> Bool {
        guard let extensionController, extensionRun != nil, !extensionRunBusy else { return false }
        let generation = extensionRunGeneration
        guard try await extensionController.currentInstanceHasRevokedPublisher(),
              generation == extensionRunGeneration, extensionRun != nil, !extensionRunBusy else { return false }
        await stopExtensionRun()
        if let extensionRunIssue { throw GhostermError.failed(extensionRunIssue) }
        return extensionRun == nil
    }

    public func stopExtensionRun() async {
        guard let extensionController, extensionRun != nil, !extensionRunBusy else { return }
        extensionRegisteredViews?.invalidate()
        extensionTable?.invalidate()
        extensionDiff?.invalidate()
        extensionTask?.invalidate()
        claudeRPC?.invalidate(); claudeRPC = nil
        claudeConversation?.invalidate(); claudeConversation = nil
        extensionRunGeneration = UUID()
        runtimeTask?.cancel(); runtimeTask = nil
        codexCallBusy = false
        cancelCodexChangeReview()
        extensionRunBusy = true; extensionRunIssue = nil
        let runtimeOwnsSession = extensionRun?.runtime != nil
        do {
            var temporaryOnly = false
            if extensionRun?.localLaunch != nil, let temporary = try await extensionController.localTemporaryState() {
                temporaryOnly = temporary.phase != .active
            }
            if !temporaryOnly, extensionRun?.runtime != nil {
                try await extensionController.stopRuntime()
                var stopped = false
                for _ in 0..<200 {
                    let state = try await extensionController.stepRuntime()
                    updateExtensionRun(runtime: state)
                    if !Self.runtimeIsRunning(state.phase) { stopped = true; break }
                    try await Task.sleep(nanoseconds: 20_000_000)
                }
                guard stopped else { throw GhostermError.failed("插件 Runtime 未在停止期限内结束，保留 owner 供重试") }
            }
            if !temporaryOnly, !runtimeOwnsSession, let session = extensionRun?.session,
               session.phase != GhostermCore.ExtensionSession.Phase.ended.rawValue,
               session.phase != GhostermCore.ExtensionSession.Phase.closed.rawValue {
                try await extensionController.stopSession()
                var ended = false
                for _ in 0..<200 {
                    let state = try await extensionController.stepSession(milliseconds: 10)
                    updateExtensionRun(session: state)
                    if state.phase == GhostermCore.ExtensionSession.Phase.ended.rawValue ||
                       state.phase == GhostermCore.ExtensionSession.Phase.closed.rawValue { ended = true; break }
                    try await Task.sleep(nanoseconds: 20_000_000)
                }
                guard ended else { throw GhostermError.failed("插件 Session 未在停止期限内结束，保留 owner 供重试") }
            }
            try await extensionController.releaseFinished()
            cleanupPending = false
            extensionLastLocalProcess = try? await extensionController.localProcessState()
            extensionLastLocalTemporary = try? await extensionController.localTemporaryState()
            extensionRun = nil; extensionRunInvocation = nil
            extensionTable = nil; extensionDiff = nil; extensionTask = nil; extensionViews = []; extensionRegisteredViews = nil
            codexThreadID = nil; codexMessageDraft = ""
            codexTurnActive = false
            codexSendUnconfirmed = false
            codexPendingApprovals = []
            codexTimeline = []
            pendingExtensionRPCHandles.removeAll()
        } catch {
            cleanupPending = true
            extensionLastLocalProcess = try? await extensionController.localProcessState()
            extensionLastLocalTemporary = try? await extensionController.localTemporaryState()
            extensionRunIssue = "释放插件资源失败：\(error)"
        }
        extensionRunBusy = false
    }

    public func retryLocalExtensionCleanup() async {
        guard let extensionController, extensionRun == nil, !extensionRunBusy,
              cleanupPending || extensionLastLocalProcess != nil || extensionLastLocalTemporary != nil else { return }
        extensionRunBusy = true
        try? await extensionController.stopRuntime()
        try? await extensionController.stopSession()
        try? await extensionController.stopSSHChannel()
        do { try await extensionController.releaseFinished(); cleanupPending = false; extensionRunIssue = nil }
        catch { cleanupPending = true; extensionRunIssue = "释放本机插件资源失败，仍保留 owner：\(error)" }
        extensionLastLocalProcess = try? await extensionController.localProcessState()
        extensionLastLocalTemporary = try? await extensionController.localTemporaryState()
        extensionRunBusy = false
    }

    public func startCodexThread() async {
        guard let run = extensionRun, canInteractWithRuntime, run.command == "ai codex", !codexCallBusy,
              run.methods.contains("codex.thread.start") else { return }
        let generation = extensionRunGeneration
        codexCallBusy = true
        defer { if generation == extensionRunGeneration { codexCallBusy = false } }
        do {
            struct Started: Decodable { let threadID: String }
            let value: Started = try await callCodexExecutor("codex.thread.start", input: [:])
            codexThreadID = value.threadID
        } catch { if generation == extensionRunGeneration { extensionRunIssue = "无法启动 Codex 对话：\(error)" } }
    }

    public func sendCodexMessage() async {
        guard let run = extensionRun, canInteractWithRuntime, run.command == "ai codex", codexThreadID != nil,
              !codexTurnActive, !codexSendUnconfirmed,
              !codexCallBusy, run.methods.contains("codex.turn.send") else { return }
        let text = codexMessageDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        let generation = extensionRunGeneration
        codexCallBusy = true
        codexSendUnconfirmed = true
        defer { if generation == extensionRunGeneration { codexCallBusy = false } }
        do {
            struct Sent: Decodable { let turnID: String }
            let sent: Sent = try await callCodexExecutor("codex.turn.send", input: ["text": text], distinguishNoEffect: true)
            guard !sent.turnID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
                throw GhostermError.failed("Codex 回合 ID 缺失")
            }
            codexMessageDraft = ""
            codexSendUnconfirmed = false
        } catch let failure as CodexNoEffect {
            if generation == extensionRunGeneration {
                codexSendUnconfirmed = false
                extensionRunIssue = "Codex 未发送消息：\(failure.message)"
            }
        } catch {
            if generation == extensionRunGeneration { extensionRunIssue = "无法确认 Codex 消息发送：\(error)" }
        }
    }

    public func interruptCodexTurn() async {
        guard let run = extensionRun, canInteractWithRuntime, run.command == "ai codex", codexTurnActive,
              !codexCallBusy, run.methods.contains("codex.turn.interrupt") else { return }
        let generation = extensionRunGeneration
        codexCallBusy = true
        defer { if generation == extensionRunGeneration { codexCallBusy = false } }
        do {
            struct Interrupted: Decodable { let accepted: Bool }
            let result: Interrupted = try await callCodexExecutor("codex.turn.interrupt", input: [:])
            guard result.accepted else { throw GhostermError.failed("Executor 未接受中断请求") }
        } catch { if generation == extensionRunGeneration { extensionRunIssue = "Codex 中断失败：\(error)" } }
    }

    public func codexApprovalSnapshot(_ approval: CodexPendingApproval) -> String? {
        if approval.method == "item/permissions/requestApproval" { return approval.permissionReviewID }
        return approval.identity?.reviewedSnapshot(codexChangeReview, isCurrent: currentCodexChange)
    }

    public func respondCodexApproval(id: String, allow: Bool, snapshotID: String? = nil) async {
        guard let run = extensionRun, canInteractWithRuntime, run.command == "ai codex", !codexCallBusy,
              let approval = codexPendingApprovals.first(where: { $0.id.utf8.elementsEqual(id.utf8) }),
              run.methods.contains("codex.approval.respond") else { return }
        let fileApproval = approval.method == "item/fileChange/requestApproval"
        let permissionApproval = approval.method == "item/permissions/requestApproval"
        if permissionApproval && (snapshotID == nil || snapshotID != approval.permissionReviewID) {
            extensionRunIssue = "权限请求已更新，请重新核对当前完整权限范围。"
            return
        }
        if allow && fileApproval && (snapshotID == nil || snapshotID != codexApprovalSnapshot(approval)) {
            extensionRunIssue = "请先读取并核对当前完整补丁，再批准此版本。"
            return
        }
        let generation = extensionRunGeneration
        codexCallBusy = true
        defer { if generation == extensionRunGeneration { codexCallBusy = false } }
        do {
            struct Responded: Decodable { let accepted: Bool }
            let decisionJSON = String(data: try JSONEncoder().encode(allow ? "accept" : "decline"), encoding: .utf8)!
            let result: Responded = try await callCodexExecutor("codex.approval.respond",
                input: ["requestID": id, "decisionJSON": decisionJSON,
                        "snapshotID": permissionApproval || (allow && fileApproval) ? snapshotID! : ""])
            guard result.accepted else { throw GhostermError.failed("Executor 未接受审批决定") }
            codexPendingApprovals.removeAll { $0.id.utf8.elementsEqual(id.utf8) }
        } catch {
            if generation == extensionRunGeneration {
                if allow && fileApproval { cancelCodexChangeReview() }
                extensionRunIssue = "Codex 审批响应失败：\(error)" +
                    (allow && fileApproval ? "；请重新读取完整补丁后核对。" : "")
            }
        }
    }

    private struct CodexNoEffect: Error { let message: String }

    private struct CodexRPCEnvelope<Value: Decodable>: Decodable {
        struct Failure: Decodable {
            let effect: String
            let message: String
        }
        let kind: String
        let value: Value?
        let error: Failure?
    }

    private func callCodexExecutor<Value: Decodable>(_ method: String, input: [String: Any],
                                                       distinguishNoEffect: Bool = false) async throws -> Value {
        let bytes = try JSONSerialization.data(withJSONObject: input, options: [.sortedKeys])
        let outcome = try await callExtensionExecutor(method, inputBytes: bytes, command: "ai codex",
                                                      returnTerminalOutcome: distinguishNoEffect)
        let envelope = try JSONDecoder().decode(CodexRPCEnvelope<Value>.self, from: outcome)
        if distinguishNoEffect, envelope.kind == "failed", let failure = envelope.error, failure.effect == "none" {
            throw CodexNoEffect(message: failure.message)
        }
        guard envelope.kind == "success", let value = envelope.value else {
            throw GhostermError.failed("Codex RPC 返回了无效结果 envelope")
        }
        return value
    }

    /// Complete vendor records for the Native Claude conversation adapter.
    /// A replacement run never reuses the previous adapter or child sequence.
    public func requestClaudeAction(method: String, parameters: Data) async throws -> Data {
        let generation = extensionRunGeneration
        try checkExtensionRunGeneration(generation)
        guard !extensionRunBusy, extensionRun?.command == "ai claude",
              canInteractWithRuntime,
              ClaudeRPCClient.methods.allSatisfy({ extensionRun?.methods.contains($0) == true }) else {
            throw GhostermError.failed("Claude 需要本次已授权的 ACTIVE Runtime 和完整 RPC allowlist")
        }
        if claudeRPC == nil {
            claudeRPC = ClaudeRPCClient { [weak self] name, input in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.callExtensionExecutor(name, inputBytes: input, command: "ai claude")
            }
        }
        return try await claudeRPC!.request(method: method, parameters: parameters)
    }

    /// Current-Runtime observation only; never creates a replacement adapter.
    public func observeClaudeOutcome() async throws -> ClaudeRPCClient.Observation {
        let generation = extensionRunGeneration
        try checkExtensionRunGeneration(generation)
        guard !extensionRunBusy, extensionRun?.command == "ai claude",
              canInteractWithRuntime,
              ClaudeRPCClient.methods.allSatisfy({ extensionRun?.methods.contains($0) == true }),
              let claudeRPC else { throw GhostermError.failed("原 Claude Runtime 已不可观察") }
        let result = try await claudeRPC.observe()
        try checkExtensionRunGeneration(generation)
        return result
    }

    /// Standard Native views retain declared failures and uncertainty as facts.
    func callPluginViewOutcome(_ method: String, inputBytes: Data, command: String) async throws -> Data {
        try await callExtensionExecutor(method, inputBytes: inputBytes, command: command, returnTerminalOutcome: true)
    }

    private func callExtensionExecutor(_ method: String, inputBytes: Data, command: String,
                                       returnTerminalOutcome: Bool = false) async throws -> Data {
        let generation = extensionRunGeneration
        try checkExtensionRunGeneration(generation)
        guard let extensionController, let run = extensionRun,
              canInteractWithRuntime, run.command == command, run.methods.contains(method) else {
            throw GhostermError.failed("插件 RPC 未在本次 Session allowlist 中")
        }
        let admission = try await extensionController.requestExecutorRPC(method: method, input: inputBytes)
        guard admission.status == .ok, let handle = admission.handle else {
            if returnTerminalOutcome {
                let codes: [ExtensionSession.RPCStatus: String] = [.wait: "BUSY", .invalid: "INVALID_INPUT", .denied: "DENIED", .unsupported: "UNSUPPORTED", .closed: "CANCELLED"]
                if let code = codes[admission.status] {
                    return try JSONSerialization.data(withJSONObject: ["kind": "failed", "error": ["code": code, "message": code, "effect": "none"]])
                }
            }
            throw GhostermError.failed("插件 RPC 未能进入发送队列（status \(admission.status.rawValue)）")
        }
        var completed = false
        defer { if !completed && generation == extensionRunGeneration { pendingExtensionRPCHandles.insert(handle) } }
        try checkExtensionRunGeneration(generation)
        let deadline = ProcessInfo.processInfo.systemUptime + 30
        do {
            while ProcessInfo.processInfo.systemUptime < deadline {
                try checkExtensionRunGeneration(generation)
                let (status, result) = try await extensionController.executorRPCResult(handle: handle)
                try checkExtensionRunGeneration(generation)
                if status == .ok, let result,
                   result.state != .queued, result.state != .pending {
                    let forget = try await extensionController.forgetExecutorRPC(handle: handle)
                    try checkExtensionRunGeneration(generation)
                    guard forget == .ok else { throw GhostermError.failed("插件 RPC 结果未能释放") }
                    completed = true
                    if returnTerminalOutcome {
                        guard result.state != .succeeded || result.confirmed else {
                            return Data(#"{"kind":"unknown","reason":"UNCONFIRMED_SUCCESS"}"#.utf8)
                        }
                        return result.outcome
                    }
                    guard result.state == .succeeded, result.confirmed else {
                        throw GhostermError.failed("插件 RPC 结果为 \(result.state)，执行效果未知或失败")
                    }
                    return result.outcome
                }
                try Task.checkCancellation()
                try await Task.sleep(nanoseconds: 20_000_000)
            }
            try checkExtensionRunGeneration(generation)
            _ = try await extensionController.cancelExecutorRPC(handle: handle)
            throw GhostermError.failed("插件 RPC 等待超过 30 秒，结果可能未知")
        } catch {
            // Do not retry or discard an unresolved handle: Core retains its
            // unknown outcome until the caller explicitly observes it.
            throw error
        }
    }

    private func checkExtensionRunGeneration(_ generation: UUID) throws {
        try Task.checkCancellation()
        guard generation == extensionRunGeneration else { throw CancellationError() }
        guard runtimeObservationIssue == nil else {
            throw GhostermError.failed("插件运行状态未知；未确认的结果保持未知，不会自动重试")
        }
    }

    public var canReadCodexChanges: Bool {
        canInteractWithRuntime && extensionRun?.command == "ai codex" &&
        extensionRun?.methods.contains("codex.change.read") == true
    }

    public func loadCodexChanges(_ reference: CodexChangeReference) {
        guard canReadCodexChanges, currentCodexChange(reference) else { return }
        beginCodexChangeRead(reference: reference, read: { [weak self] offset in
            guard let self else { throw CancellationError() }
            return try await self.callCodexExecutor("codex.change.read",
                input: ["snapshotID": reference.snapshotID, "offset": offset])
        }, isCurrent: { [weak self] in self?.currentCodexChange(reference) == true && self?.canReadCodexChanges == true })
    }

    private func currentCodexChange(_ reference: CodexChangeReference) -> Bool {
        codexThreadID?.utf8.elementsEqual(reference.threadID.utf8) == true &&
        codexTimeline.contains { $0.changeReference == reference }
    }

    public func cancelCodexChangeReview() {
        codexChangeReadGeneration = UUID()
        codexChangeReadTask?.cancel()
        codexChangeReadTask = nil
        codexChangeReview = nil
    }

    /// The injected read boundary also exercises asynchronous invalidation in tests.
    @discardableResult
    func beginCodexChangeRead(reference: CodexChangeReference,
                              read: @escaping @MainActor (Int) async throws -> CodexChangeChunk,
                              isCurrent: @escaping @MainActor () -> Bool) -> Task<Void, Never> {
        cancelCodexChangeReview()
        let token = codexChangeReadGeneration
        codexChangeReview = CodexChangeReviewState(reference: reference, receivedBytes: 0, document: nil, issue: nil)
        let task = Task { [weak self] in
            guard let self else { return }
            defer { if self.codexChangeReadGeneration == token { self.codexChangeReadTask = nil } }
            var assembler = CodexChangeAssembler(reference: reference)
            let deadline = ProcessInfo.processInfo.systemUptime + 120
            do {
                for _ in 0..<2048 {
                    try Task.checkCancellation()
                    guard self.codexChangeReadGeneration == token else { return }
                    guard isCurrent() else { self.cancelCodexChangeReview(); return }
                    guard ProcessInfo.processInfo.systemUptime < deadline else { throw CodexChangeReadError.incomplete }
                    let chunk = try await read(assembler.nextOffset)
                    try Task.checkCancellation()
                    guard ProcessInfo.processInfo.systemUptime < deadline else { throw CodexChangeReadError.incomplete }
                    guard self.codexChangeReadGeneration == token else { return }
                    guard isCurrent() else { self.cancelCodexChangeReview(); return }
                    try assembler.append(chunk)
                    self.codexChangeReview = CodexChangeReviewState(reference: reference,
                        receivedBytes: assembler.receivedBytes, document: nil, issue: nil)
                    if assembler.done {
                        let complete = assembler
                        let document = try await Task.detached(priority: .userInitiated) { try complete.document() }.value
                        try Task.checkCancellation()
                        guard ProcessInfo.processInfo.systemUptime < deadline else { throw CodexChangeReadError.incomplete }
                        guard self.codexChangeReadGeneration == token else { return }
                        guard isCurrent() else { self.cancelCodexChangeReview(); return }
                        self.codexChangeReview = CodexChangeReviewState(reference: reference,
                            receivedBytes: assembler.receivedBytes, document: document, issue: nil)
                        return
                    }
                }
                throw CodexChangeReadError.incomplete
            } catch {
                guard self.codexChangeReadGeneration == token else { return }
                guard !Task.isCancelled, isCurrent() else { self.cancelCodexChangeReview(); return }
                self.codexChangeReview = CodexChangeReviewState(reference: reference,
                    receivedBytes: assembler.receivedBytes, document: nil, issue: error.localizedDescription)
            }
        }
        codexChangeReadTask = task
        return task
    }

    private func advanceExtensionRuntime() async {
        guard let extensionController else { return }
        let generation = extensionRunGeneration
        do {
            while true {
                try Task.checkCancellation()
                let state = try await extensionController.stepRuntime()
                try checkExtensionRunGeneration(generation)
                updateExtensionRun(runtime: state)
                if !Self.runtimeIsRunning(state.phase) { return }
                try await reapPendingExtensionRPCs()
                try checkExtensionRunGeneration(generation)
                if state.phase == GhostermCore.ExtensionRuntime.Phase.active.rawValue {
                    try await pumpCodexHostEvent()
                }
                if extensionRun?.requiredCapabilities.contains("resource.receive") == true,
                   let resource = try await extensionController.takeSessionResource(maxBytes: 1048576) {
                    try checkExtensionRunGeneration(generation)
                    appendExtensionResource(resource)
                }
                try await Task.sleep(nanoseconds: 50_000_000)
            }
        } catch {
            if Task.isCancelled || generation != extensionRunGeneration { return }
            runtimeObservationFailed(String(describing: error))
        }
    }

    /// The last Native snapshot is retained as an observation, not authority
    /// for another request. Only an explicit stop may resume cleanup stepping.
    func runtimeObservationFailed(_ message: String) {
        guard runtimeObservationIssue == nil else { return }
        runtimeObservationIssue = message
        runtimeTask?.cancel()
        extensionRegisteredViews?.invalidate(); extensionTable?.invalidate()
        extensionDiff?.invalidate(); extensionTask?.invalidate()
        claudeRPC?.invalidate(); claudeConversation?.invalidate()
        cancelCodexChangeReview(); codexPendingApprovals = []
        extensionRunIssue = "插件运行观察已中断；已停用输入与批准，未确认的结果保持未知。可停止并释放。\(message)"
    }

    public func publishClientResource(relativePath: String, root: String = "client", instanceID: String? = nil) async {
        guard !extensionRunBusy, !closeRequested, let controller = extensionController, canInteractWithRuntime else { return }
        let generation = extensionRunGeneration
        do {
            let state = try await controller.publishClientResource(instanceID: instanceID ?? extensionRun?.runtime?.instanceID ?? "", root: root, relativePath: relativePath)
            guard generation == extensionRunGeneration else { return }
            updateExtensionRun(runtime: state)
        } catch {
            if generation == extensionRunGeneration { extensionRunIssue = "资源发布失败：\(error)" }
        }
    }
    public func cancelClientPublication(id: String) async {
        guard let controller = extensionController, extensionRun?.runtime?.publication?.id == id else { return }
        let generation = extensionRunGeneration
        do { try await controller.cancelClientPublication(id: id) }
        catch { if generation == extensionRunGeneration { extensionRunIssue = "无法取消资源发布：\(error)" } }
    }

    private func updateExtensionRun(session: ExtensionController.SessionState) {
        guard let run = extensionRun else { return }
        extensionRun = ExtensionRun(packageID: run.packageID, packageDigest: run.packageDigest,
            command: run.command, arguments: run.arguments, decodedArguments: run.decodedArguments,
            executorDigest: run.executorDigest,
            peerIdentity: run.peerIdentity, remoteCommand: run.remoteCommand, executorExecution: run.executorExecution, executorAttachment: run.executorAttachment, localLaunch: run.localLaunch, clientWorkspace: run.clientWorkspace,
            methods: run.methods, hostMethods: run.hostMethods,
            requiredCapabilities: run.requiredCapabilities,
            session: session, runtime: run.runtime, receivedResources: run.receivedResources,
            codexEvents: run.codexEvents)
    }

    private func updateExtensionRun(runtime: ExtensionController.RuntimeState) {
        guard let run = extensionRun else { return }
        if !Self.runtimeIsRunning(runtime.phase) {
            extensionRegisteredViews?.invalidate()
            extensionTable?.invalidate()
            extensionDiff?.invalidate()
            extensionTask?.invalidate()
            claudeRPC?.invalidate()
            claudeConversation?.invalidate()
            codexPendingApprovals = []
            cancelCodexChangeReview()
        }
        extensionRun = ExtensionRun(packageID: run.packageID, packageDigest: run.packageDigest,
            command: run.command, arguments: run.arguments, decodedArguments: run.decodedArguments,
            executorDigest: run.executorDigest,
            peerIdentity: run.peerIdentity, remoteCommand: run.remoteCommand, executorExecution: run.executorExecution, executorAttachment: run.executorAttachment, localLaunch: run.localLaunch, clientWorkspace: run.clientWorkspace,
            methods: run.methods, hostMethods: run.hostMethods,
            requiredCapabilities: run.requiredCapabilities,
            session: run.session, runtime: runtime, receivedResources: run.receivedResources,
            codexEvents: run.codexEvents)
        if let issue = runtime.viewIssue { extensionRunIssue = issue }
        if run.requiredCapabilities.contains("ui.views"), runtime.phase == ExtensionRuntime.Phase.active.rawValue {
            do {
                guard let data = runtime.views, let instance = runtime.viewSessionInstance, let epoch = runtime.viewSessionEpoch else {
                    throw PluginViewValue.Failure.invalidResponse
                }
                if extensionRegisteredViews == nil {
                    let generation = extensionRunGeneration, runtimeID = runtime.instanceID
                    extensionRegisteredViews = try PluginViewsModel(instance: instance, epoch: epoch,
                        kinds: Set(extensionViews.compactMap(PluginViewRegistration.Kind.init(rawValue:))),
                        capabilities: Set(run.requiredCapabilities), methods: Set(run.methods),
                        rpc: { [weak self] method, input in
                            guard let self else { throw CancellationError() }
                            try self.checkExtensionRunGeneration(generation)
                            return try await self.callPluginViewOutcome(method, inputBytes: input, command: run.command)
                        }, isCurrent: { [weak self] in
                            guard let self else { return false }
                            return generation == self.extensionRunGeneration && !self.extensionRunBusy &&
                                self.extensionRun?.runtime?.instanceID == runtimeID &&
                                self.canInteractWithRuntime
                        }, close: { [weak self] handle in
                            guard let self, let controller = self.extensionController else { throw CancellationError() }
                            try self.checkExtensionRunGeneration(generation)
                            let state = try await controller.closeRuntimeView(instanceID: runtimeID, handle: handle)
                            try self.checkExtensionRunGeneration(generation)
                            guard let data = state.views else { throw PluginViewValue.Failure.invalidResponse }
                            return data
                        })
                }
                try extensionRegisteredViews?.apply(data)
            } catch {
                extensionRegisteredViews?.invalidate()
                extensionRunIssue = "插件视图注册不可用，可停止并释放：\(error)"
            }
        }
        if !run.requiredCapabilities.contains("ui.views"), extensionViews.contains("table"), run.requiredCapabilities.contains("ui.table"),
           runtime.phase == ExtensionRuntime.Phase.active.rawValue, extensionTable == nil,
           PluginTableModel.methods.allSatisfy(run.methods.contains) {
            let generation = extensionRunGeneration
            extensionTable = PluginTableModel(rpc: { [weak self] method, input in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.callExtensionExecutor(method, inputBytes: input, command: run.command)
            }, isCurrent: { [weak self] in
                guard let self else { return false }
                return generation == self.extensionRunGeneration && !self.extensionRunBusy &&
                    self.canInteractWithRuntime
            })
        }
        if !run.requiredCapabilities.contains("ui.views"), extensionViews.contains("diff"), run.requiredCapabilities.contains("ui.diff"),
           runtime.phase == ExtensionRuntime.Phase.active.rawValue, extensionDiff == nil,
           PluginDiffModel.methods.allSatisfy(run.methods.contains) {
            let generation = extensionRunGeneration
            extensionDiff = PluginDiffModel(rpc: { [weak self] method, input in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.callExtensionExecutor(method, inputBytes: input, command: run.command)
            }, isCurrent: { [weak self] in
                guard let self else { return false }
                return generation == self.extensionRunGeneration && !self.extensionRunBusy &&
                    self.canInteractWithRuntime
            })
        }
        if !run.requiredCapabilities.contains("ui.views"), extensionViews.contains("task"), run.requiredCapabilities.contains("ui.task"),
           runtime.phase == ExtensionRuntime.Phase.active.rawValue, extensionTask == nil,
           PluginTaskModel.methods.allSatisfy(run.methods.contains) {
            let generation = extensionRunGeneration
            extensionTask = PluginTaskModel(rpc: { [weak self] method, input in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.callPluginViewOutcome(method, inputBytes: input, command: run.command)
            }, isCurrent: { [weak self] in
                guard let self else { return false }
                return generation == self.extensionRunGeneration && !self.extensionRunBusy &&
                    self.canInteractWithRuntime
            })
        }
        if run.command == "ai claude", runtime.phase == ExtensionRuntime.Phase.active.rawValue,
           ClaudeRPCClient.methods.allSatisfy(run.methods.contains),
           claudeConversation == nil {
            let generation = extensionRunGeneration
            claudeConversation = ClaudeConversation(request: { [weak self] method, parameters in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.requestClaudeAction(method: method, parameters: parameters)
            }, observe: { [weak self] in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.observeClaudeOutcome()
            }, isCurrent: { [weak self] in
                guard let self else { return false }
                return generation == self.extensionRunGeneration && !self.extensionRunBusy &&
                    self.canInteractWithRuntime
            })
        }
    }

    private func appendExtensionResource(_ resource: ExtensionController.ReceivedResource) {
        guard let run = extensionRun else { return }
        let total = run.receivedResources.reduce(UInt64(0)) { $0 + $1.size }
        guard run.receivedResources.count < 16, resource.size <= 1048576,
              total <= 8388608, resource.size <= 8388608 - total else {
            extensionRunIssue = "插件资源超过本次界面的数量或总大小上限"
            return
        }
        extensionRun = ExtensionRun(packageID: run.packageID, packageDigest: run.packageDigest,
            command: run.command, arguments: run.arguments, decodedArguments: run.decodedArguments,
            executorDigest: run.executorDigest, peerIdentity: run.peerIdentity,
            remoteCommand: run.remoteCommand, executorExecution: run.executorExecution, executorAttachment: run.executorAttachment, localLaunch: run.localLaunch, clientWorkspace: run.clientWorkspace, methods: run.methods, hostMethods: run.hostMethods,
            requiredCapabilities: run.requiredCapabilities,
            session: run.session, runtime: run.runtime,
            receivedResources: run.receivedResources + [resource], codexEvents: run.codexEvents)
    }

    private struct CodexHostEvent: Decodable {
        let sequence: UInt64
        let kind: String
        let notification: Notification?
        struct Notification: Decodable { let method: String }
    }

    private func pumpCodexHostEvent() async throws {
        guard let extensionController, let run = extensionRun, run.command == "ai codex" else { return }
        let generation = extensionRunGeneration
        let poll = try await extensionController.pollHostRPCRequest()
        try checkExtensionRunGeneration(generation)
        guard let request = poll.request else {
            guard poll.status == UInt32(GhostermCore.ExtensionSession.RPCStatus.wait.rawValue) else {
                throw GhostermError.failed("读取 Codex Executor Host RPC 失败（status \(poll.status)）")
            }
            return
        }
        guard request.method == "codex.event", run.hostMethods.contains("codex.event") else {
            let failure = Data(#"{"kind":"failed","error":{"code":"DENIED","message":"Host method is not registered","effect":"none"}}"#.utf8)
            guard try await extensionController.respondHostRPC(ticket: request.ticket, outcome: failure) == 0 else {
                throw GhostermError.failed("无法拒绝未注册的插件 Host RPC")
            }
            return
        }
        let encoded: String
        let event: CodexHostEvent
        do {
            struct EventInput: Decodable { let event: String }
            encoded = try JSONDecoder().decode(EventInput.self, from: request.input).event
            event = try JSONDecoder().decode(CodexHostEvent.self, from: Data(encoded.utf8))
        } catch {
            let failure = Data(#"{"kind":"failed","error":{"code":"DENIED","message":"Malformed Codex event","effect":"none"}}"#.utf8)
            guard try await extensionController.respondHostRPC(ticket: request.ticket, outcome: failure) == 0 else {
                throw GhostermError.failed("无法拒绝格式错误的 Codex 事件")
            }
            throw GhostermError.failed("Codex Executor 发送了格式错误的事件")
        }
        let rawEvent = (try? JSONSerialization.jsonObject(with: Data(encoded.utf8))) as? [String: Any]
        var approvalToAdd: CodexPendingApproval?
        var approvalToRemove: String?
        if event.kind == "approval-request" {
            guard let approval = Self.decodeCodexApproval(rawEvent?["request"]),
                  !codexPendingApprovals.contains(where: { $0.id.utf8.elementsEqual(approval.id.utf8) }),
                  codexPendingApprovals.count < 32 else {
                try await rejectCodexEvent(extensionController, ticket: request.ticket,
                                           message: "Invalid or excessive Codex approval request")
                throw GhostermError.failed("Codex Executor approval 请求无效")
            }
            approvalToAdd = approval
        } else if event.kind == "approval-resolved" {
            approvalToRemove = Self.codexRequestKey(rawEvent?["requestID"])
        }
        guard let history = CodexEventHistory.appending(encoded, kind: event.kind, sequence: event.sequence,
                                                       after: lastCodexEventSequence, to: run.codexEvents) else {
            let failure = Data(#"{"kind":"failed","error":{"code":"DENIED","message":"Invalid or excessive Codex event","effect":"none"}}"#.utf8)
            guard try await extensionController.respondHostRPC(ticket: request.ticket, outcome: failure) == 0 else {
                throw GhostermError.failed("无法拒绝无效的 Codex 事件")
            }
            throw GhostermError.failed("Codex Executor 事件序列无效或超过 Host 限额")
        }
        let success = Data(#"{"kind":"success","value":{"accepted":true}}"#.utf8)
        guard try await extensionController.respondHostRPC(ticket: request.ticket, outcome: success) == 0 else {
            throw GhostermError.failed("无法确认 Codex Executor 事件")
        }
        try checkExtensionRunGeneration(generation)
        lastCodexEventSequence = event.sequence
        applyCodexApprovalEvent(kind: event.kind, notificationMethod: event.notification?.method,
            added: approvalToAdd, removedID: approvalToRemove)
        extensionRun = ExtensionRun(packageID: run.packageID, packageDigest: run.packageDigest,
            command: run.command, arguments: run.arguments, decodedArguments: run.decodedArguments,
            executorDigest: run.executorDigest, peerIdentity: run.peerIdentity,
            remoteCommand: run.remoteCommand, executorExecution: run.executorExecution, executorAttachment: run.executorAttachment, localLaunch: run.localLaunch, clientWorkspace: run.clientWorkspace, methods: run.methods, hostMethods: run.hostMethods,
            requiredCapabilities: run.requiredCapabilities, session: run.session, runtime: run.runtime,
            receivedResources: run.receivedResources, codexEvents: history)
        if event.kind == "notification" {
            if event.notification?.method == "turn/started" { codexTurnActive = true }
            if event.notification?.method == "turn/completed" { codexTurnActive = false }
            if let notification = rawEvent?["notification"] as? [String: Any],
               let method = notification["method"] as? String,
               let params = notification["params"], JSONSerialization.isValidJSONObject(params),
               let data = try? JSONSerialization.data(withJSONObject: params, options: [.sortedKeys]) {
                codexTimeline = CodexTimelineReducer.apply(codexTimeline, sequence: event.sequence,
                    method: method, params: data)
            }
        } else if event.kind != "approval-request" && event.kind != "approval-resolved" {
            codexTimeline = CodexTimelineReducer.apply(codexTimeline, sequence: event.sequence,
                method: event.kind, params: Data(encoded.utf8))
        }
        if let review = codexChangeReview,
           !codexTimeline.contains(where: { $0.changeReference == review.reference }) {
            cancelCodexChangeReview()
        }
    }

    /// Applies only after the incoming Host RPC has been validated and acknowledged.
    func applyCodexApprovalEvent(kind: String, notificationMethod: String? = nil,
                                added: CodexPendingApproval? = nil, removedID: String? = nil) {
        if let added { codexPendingApprovals.append(added) }
        if let removedID { codexPendingApprovals.removeAll { $0.id.utf8.elementsEqual(removedID.utf8) } }
        if (kind == "notification" && notificationMethod == "turn/completed") ||
           ["protocol-error", "closed", "outcome-unknown"].contains(kind) {
            codexPendingApprovals = []
        }
        if ["protocol-error", "closed", "outcome-unknown"].contains(kind) { cancelCodexChangeReview() }
    }

    private func rejectCodexEvent(_ controller: ExtensionController, ticket: UInt64,
                                  message: String) async throws {
        let encodedMessage = String(data: try JSONEncoder().encode(message), encoding: .utf8)!
        let outcome = Data("{\"kind\":\"failed\",\"error\":{\"code\":\"DENIED\",\"message\":\(encodedMessage),\"effect\":\"none\"}}".utf8)
        guard try await controller.respondHostRPC(ticket: ticket, outcome: outcome) == 0 else {
            throw GhostermError.failed("无法拒绝无效的 Codex 事件")
        }
    }

    static func decodeCodexApproval(_ value: Any?) -> CodexPendingApproval? {
        guard let object = value as? [String: Any],
              let id = codexRequestKey(object["id"]),
              let method = object["method"] as? String,
              ["item/commandExecution/requestApproval", "item/fileChange/requestApproval", "item/permissions/requestApproval"].contains(method),
              let params = object["params"], JSONSerialization.isValidJSONObject(params),
              let data = try? JSONSerialization.data(withJSONObject: params, options: [.sortedKeys]),
              let paramsJSON = String(data: data, encoding: .utf8) else { return nil }
        let itemID = (object["params"] as? [String: Any])?["itemId"] as? String
        var identity: CodexApprovalIdentity?
        var permissionReviewID: String?
        if method == "item/fileChange/requestApproval" || method == "item/permissions/requestApproval" {
            let fields = object["params"] as? [String: Any]
            guard let thread = fields?["threadId"] as? String, !thread.isEmpty, thread.utf8.count <= 128,
                  let turn = fields?["turnId"] as? String, !turn.isEmpty, turn.utf8.count <= 128,
                  let itemID, !itemID.isEmpty, itemID.utf8.count <= 128 else { return nil }
            if method == "item/fileChange/requestApproval" {
                identity = CodexApprovalIdentity(threadID: thread, turnID: turn, itemID: itemID)
            } else {
                guard let fields, let profile = fields["permissions"] as? [String: Any],
                      Set(profile.keys) == ["network", "fileSystem"],
                      ["network", "fileSystem"].allSatisfy({ profile[$0] is NSNull || profile[$0] is [String: Any] }),
                      fields["cwd"] is String,
                      let token = object["permissionReviewID"] as? String,
                      token.hasPrefix("permissions."), (1...20).contains(token.dropFirst(12).count),
                      token.dropFirst(12).first != "0",
                      token.dropFirst(12).allSatisfy({ $0 >= "0" && $0 <= "9" }) else { return nil }
                permissionReviewID = token
            }
        }
        return CodexPendingApproval(id: id, method: method, paramsJSON: paramsJSON, itemID: itemID,
                                    identity: identity, permissionReviewID: permissionReviewID)
    }

    static func codexRequestKey(_ value: Any?) -> String? {
        if let value = value as? String, !value.isEmpty, value.utf8.count <= 128 { return "s:\(value)" }
        if let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
           number.doubleValue.isFinite, number.doubleValue.rounded(.towardZero) == number.doubleValue,
           abs(number.doubleValue) <= 9_007_199_254_740_991 {
            return "n:\(number.stringValue)"
        }
        return nil
    }

    private func reapPendingExtensionRPCs() async throws {
        guard let extensionController else { return }
        let generation = extensionRunGeneration
        for handle in pendingExtensionRPCHandles.sorted() {
            try checkExtensionRunGeneration(generation)
            let (status, result) = try await extensionController.executorRPCResult(handle: handle)
            try checkExtensionRunGeneration(generation)
            guard status == .ok, let result,
                  result.state != .queued, result.state != .pending else { continue }
            let forgotten = try await extensionController.forgetExecutorRPC(handle: handle)
            try checkExtensionRunGeneration(generation)
            guard forgotten == .ok else { throw GhostermError.failed("无法释放已结束的 Codex RPC 结果") }
            pendingExtensionRPCHandles.remove(handle)
        }
    }

    private static func runtimeIsRunning(_ phase: UInt32) -> Bool {
        phase == GhostermCore.ExtensionRuntime.Phase.created.rawValue ||
        phase == GhostermCore.ExtensionRuntime.Phase.starting.rawValue ||
        phase == GhostermCore.ExtensionRuntime.Phase.active.rawValue
    }

    func beginInspection() { extensionRunBusy = true; extensionRunIssue = nil }
    func endInspection() { extensionRunBusy = false }
    func recordInspectionIssue(_ issue: String) { cleanupPending = true; extensionRunIssue = issue }
    func captureLocalCleanup() async {
        extensionLastLocalProcess = try? await extensionController?.localProcessState()
        extensionLastLocalTemporary = try? await extensionController?.localTemporaryState()
    }
}
