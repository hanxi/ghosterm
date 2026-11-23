import Combine
import Foundation
import GhostermCore

/// Native launch snapshot. Scope registration never comes from peer frames.
public struct ExtensionGroupInvocation: Sendable, Identifiable {
    public let id = UUID()
    public let packageID: String
    public let request: ExtensionRelayBootstrap.Request
    public let sshAlias: String
    public let localLaunch: ExtensionLocalLaunch?
    public let clientWorkspace: ExtensionController.ClientWorkspace?
    public let temporaryDirectory: String
    public static func validateGroup(_ launches: [Self]) throws -> Self {
        guard let first = launches.first, (1...8).contains(launches.count), launches.allSatisfy({
            $0.sshAlias == first.sshAlias && $0.localLaunch == first.localLaunch &&
            $0.temporaryDirectory == first.temporaryDirectory && $0.request.executable == first.request.executable &&
            $0.request.attachment == nil
        }) else {
            throw GhostermError.failed("启动组须有一至八项，使用同一目标、gterm、临时目录和本机授权，且不能包含已有前台附着")
        }
        try ExtensionRelayBootstrap.validateMultiplexTemporaryDirectory(first.temporaryDirectory)
        if let local = first.localLaunch, local.temporaryDirectory != first.temporaryDirectory {
            throw GhostermError.failed("本机启动与连接临时目录不一致")
        }
        try first.localLaunch?.validateConfiguration(executable: first.request.executable)
        return first
    }

    public init(packageID: String, request: ExtensionRelayBootstrap.Request, sshAlias: String,
                localLaunch: ExtensionLocalLaunch?, clientWorkspace: ExtensionController.ClientWorkspace?, temporaryDirectory: String) {
        self.packageID = packageID; self.request = request; self.sshAlias = sshAlias
        self.localLaunch = localLaunch; self.clientWorkspace = clientWorkspace; self.temporaryDirectory = temporaryDirectory
    }
}

@MainActor public final class ExtensionConnectionModel: ObservableObject, Identifiable {
    public let id = UUID()
    public let title: String
    public let controller: ExtensionController
    @Published public private(set) var instances: [ExtensionInstanceModel] = []
    @Published public private(set) var busy = false
    @Published public private(set) var cleanupPending = false
    @Published public private(set) var closed = false
    @Published public private(set) var issue: String?
    @Published public private(set) var targetDescription = ""
    @Published public private(set) var state: ExtensionController.MultiplexState?
    @Published public private(set) var process: ExtensionController.ProcessState?
    @Published public private(set) var temporary: ExtensionLocalTemporaryState?
    public var canDiscard: Bool { closed && !busy && !cleanupPending }
    public var closeNeedsConfirmation: Bool { !closed && !closing && (busy || instances.contains { $0.closeNeedsConfirmation }) }
    public var canAdd: Bool { nativeReady && !closed && !closing && !busy && !cleanupPending && (state?.error ?? 0) == 0 && instances.filter { !$0.canDiscard }.count < 8 }
    private var launchTask: Task<Bool, Never>?
    private var monitor: Task<Void, Never>?
    private var observations: [AnyCancellable] = []
    private var origin: ExtensionGroupInvocation?
    private var environmentNames = Set<String>()
    private var scopes: [UUID: ExtensionController.MultiplexScope] = [:]
    private var nextEpoch: UInt64 = 1
    private var nativeReady = false
    private var closing = false
    init(controller: ExtensionController, title: String) { self.controller = controller; self.title = title }
    private static func hex(_ value: UUID) -> String { value.uuidString.replacingOccurrences(of: "-", with: "").lowercased() }
    static func nativeBoolean(_ value: Any?) -> Bool? {
        guard let number = value as? NSNumber, CFGetTypeID(number) == CFBooleanGetTypeID() else { return nil }
        return number.boolValue
    }
    private func object(_ data: Data) throws -> [String: Any] {
        guard let value = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { throw GhostermError.failed("连接管理回执无效") }
        return value
    }
    private static func prepare(_ launch: ExtensionGroupInvocation, controller: ExtensionController) async throws -> ExtensionRelayBootstrap.Request {
        let request = launch.request
        let selected = try await controller.resolve(command: request.command, selectedID: launch.packageID, selectedDigest: request.clientDigest)
        guard selected.status == 0, selected.packageID == launch.packageID, selected.packageDigest == request.clientDigest,
              selected.command?.path == request.command else { throw GhostermError.failed("Client 摘要或命令尚未批准") }
        _ = try await controller.decodeArguments(packageID: launch.packageID, packageDigest: request.clientDigest, command: request.command, arguments: request.arguments)
        try launch.clientWorkspace?.validate(requiredCapabilities: selected.requiredCapabilities)
        let plan = try ExtensionInstanceModel.clientPlan(selected: selected, methods: request.methods, hostMethods: request.hostMethods)
        return .init(executable: request.executable, executorDirectory: request.executorDirectory, executorDigest: request.executorDigest,
            clientDigest: request.clientDigest, methods: plan.methods, hostMethods: plan.hostMethods, clientCapabilities: plan.services,
            scopedRoots: request.scopedRoots, processPrograms: request.processPrograms, processEnvironmentNames: request.processEnvironmentNames,
            attachment: request.attachment, releaseEvidence: request.releaseEvidence, variantID: request.variantID, command: request.command, arguments: request.arguments)
    }
    static func prepareGroup(_ launches: [ExtensionGroupInvocation], controller: ExtensionController, connectionID: UUID)
        async throws -> (first: ExtensionGroupInvocation, scopes: [ExtensionController.MultiplexScope], argv: [String], command: String) {
        let first = try ExtensionGroupInvocation.validateGroup(launches)
        var requests: [ExtensionRelayBootstrap.Request] = []
        for selection in launches {
            try Task.checkCancellation()
            requests.append(try await prepare(selection, controller: controller))
        }
        let scopes = launches.enumerated().map {
            ExtensionController.MultiplexScope(instance: hex($0.element.id), epoch: UInt64($0.offset + 1))
        }
        let bootstrap = try ExtensionRelayBootstrap.buildMultiplex(connection: hex(connectionID), scopes: scopes,
            requests: requests, local: first.localLaunch != nil, temporaryDirectory: first.temporaryDirectory, live: true)
        return (first, scopes, bootstrap.argv, bootstrap.command)
    }

    private func launch(_ model: ExtensionInstanceModel, _ selection: ExtensionGroupInvocation, command: String) async {
        let request = selection.request
        await model.startExtensionClient(packageID: selection.packageID, packageDigest: request.clientDigest,
            command: request.command, arguments: request.arguments, sshAlias: selection.sshAlias,
            executable: request.executable, executorDirectory: request.executorDirectory, executorDigest: request.executorDigest,
            methods: request.methods, hostMethods: request.hostMethods, scopedRoots: request.scopedRoots,
            processPrograms: request.processPrograms, processEnvironmentNames: request.processEnvironmentNames,
            executorReleaseEvidence: request.releaseEvidence, executorVariantID: request.variantID, clientWorkspace: selection.clientWorkspace, localLaunch: selection.localLaunch,
            multiplexedConnectionCommand: command)
    }
    private func observeModels() {
        observations = instances.map { $0.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() } }
    }
    func start(_ launches: [ExtensionGroupInvocation], app: AppModel) async -> Bool {
        guard launchTask == nil, !busy, !closed else { return false }
        busy = true
        let task = Task { await self.startOnMain(launches, app: app) }
        launchTask = task
        defer { launchTask = nil; busy = false }
        return await withTaskCancellationHandler(operation: { await task.value }, onCancel: { task.cancel() })
    }
    private func startOnMain(_ launches: [ExtensionGroupInvocation], app: AppModel) async -> Bool {
        do {
            let command = try await Self.prepareGroup(launches, controller: controller, connectionID: id)
            let first = command.first, admitted = command.scopes
            origin = first; environmentNames = Set(launches.flatMap(\.request.processEnvironmentNames))
            nextEpoch = UInt64(launches.count + 1)
            if let local = first.localLaunch {
                _ = try await controller.openLocalChannel(executable: first.request.executable, arguments: command.argv, launch: local, environmentNames: environmentNames.sorted())
                targetDescription = "本机 · " + first.request.executable + " · " + local.workingDirectory
            } else {
                _ = try await app.authenticatePluginSSHChannel(alias: first.sshAlias, bootstrap: command.command, controller: controller)
                targetDescription = try await controller.multiplexTargetDescription()
            }
            let children = try await controller.adoptMultiplexConnection(connection: Self.hex(id), scopes: admitted, live: true)
            let hello = try object(await controller.manageMultiplex("hello"))
            guard let version = hello["nativeControl"] as? NSNumber, CFGetTypeID(version) != CFBooleanGetTypeID(), version.intValue == 1 else {
                throw GhostermError.failed("执行端未确认在线管理能力")
            }
            nativeReady = true
            guard children.count == launches.count else { throw GhostermError.failed("共享连接未返回完整实例集合") }
            instances = children.map { ExtensionInstanceModel(controller: $0) }
            for (model, scope) in zip(instances, admitted) { scopes[model.id] = scope }
            observeModels()
            await withTaskGroup(of: Void.self) { group in
                for (model, selection) in zip(instances, launches) { group.addTask { @MainActor in await self.launch(model, selection, command: command.command) } }
            }
            try Task.checkCancellation()
            for (model, selection) in zip(instances, launches) where model.extensionRun == nil {
                let reason = model.extensionRunIssue ?? "未进入待授权状态"
                throw GhostermError.failed("\(selection.request.command)：\(reason)")
            }
            for model in instances { app.showExtensionWorkspace(model) }
            monitor = Task { await self.observeConnection() }
            return true
        } catch {
            issue = "连接组启动失败：\(error)"
            await controller.stopMultiplexConnection()
            for instance in instances { _ = await instance.requestClose() }
            cleanupPending = !(await controller.close()); closed = !cleanupPending
            return false
        }
    }
    private func observeConnection() async {
        while !Task.isCancelled && !closed {
            do { state = try await controller.stepMultiplexConnection() }
            catch { issue = "共享连接状态查不出来：\(error)"; return }
            if state?.error != 0 { issue = "共享连接结束或发生错误，请检查各实例结果" }
            do { try await Task.sleep(for: .milliseconds(100)) } catch { return }
        }
    }
    public func acceptsConfiguration(_ selection: ExtensionGroupInvocation) -> Bool {
        guard let first = origin else { return false }
        return selection.sshAlias == first.sshAlias && selection.localLaunch == first.localLaunch &&
            selection.request.executable == first.request.executable && selection.temporaryDirectory == first.temporaryDirectory &&
            selection.request.attachment == nil &&
            (first.localLaunch == nil || Set(selection.request.processEnvironmentNames).isSubset(of: environmentNames))
    }
    @discardableResult public func add(_ selection: ExtensionGroupInvocation, app: AppModel) async -> Bool {
        guard canAdd else { issue = "连接正在处理请求、已结束或八个实例名额已满"; return false }
        busy = true; issue = nil
        let task = Task { await self.addOnMain(selection, app: app) }
        launchTask = task
        defer { launchTask = nil; busy = false }
        return await withTaskCancellationHandler(operation: { await task.value }, onCancel: { task.cancel() })
    }
    private func addOnMain(_ selection: ExtensionGroupInvocation, app: AppModel) async -> Bool {
        var child: ExtensionController?
        var added: ExtensionInstanceModel?
        do {
            guard let first = origin, acceptsConfiguration(selection) else {
                throw GhostermError.failed("新增实例须沿用此连接的目标、gterm、临时目录和原本机环境授权；更换范围请新建连接")
            }
            let request = try await Self.prepare(selection, controller: controller)
            let profile = try ExtensionRelayBootstrap.multiplexProfile(request, local: first.localLaunch != nil)
            let deadline = ProcessInfo.processInfo.systemUptime + 3
            for model in instances where model.canDiscard {
                if let scope = scopes[model.id] {
                    let input = try JSONSerialization.data(withJSONObject: ["instance": scope.instance, "epoch": String(scope.epoch)])
                    while Self.nativeBoolean(try object(await controller.manageMultiplex("status", input: input))["present"]) != false {
                        guard ProcessInfo.processInfo.systemUptime < deadline else { throw GhostermError.failed("上一实例仍在清理，请稍后重试") }
                        try await Task.sleep(for: .milliseconds(10))
                    }
                }
                scopes.removeValue(forKey: model.id)
            }
            instances.removeAll { $0.canDiscard }; observeModels()
            guard instances.count < 8, nextEpoch != UInt64.max else { throw GhostermError.failed("连接实例或代次已达上限") }
            let scope = ExtensionController.MultiplexScope(instance: Self.hex(selection.id), epoch: nextEpoch); nextEpoch += 1
            let owner = try await controller.openMultiplexInstance(scope); child = owner
            let input = try JSONSerialization.data(withJSONObject: ["instance": scope.instance, "epoch": String(scope.epoch), "profile": profile])
            let result = try object(await controller.manageMultiplex("open", input: input))
            guard Self.nativeBoolean(result["admitted"]) == true else { throw GhostermError.failed("新增实例准入未确认") }
            try Task.checkCancellation()
            let model = ExtensionInstanceModel(controller: owner)
            added = model
            instances.append(model); scopes[model.id] = scope; observeModels()
            await launch(model, selection, command: "此连接的在线实例 · " + targetDescription)
            try Task.checkCancellation()
            guard model.extensionRun != nil else {
                throw GhostermError.failed(model.extensionRunIssue ?? "新增实例未进入待授权状态")
            }
            app.showExtensionWorkspace(model)
            return true
        } catch {
            issue = "新增实例失败：\(error)"
            if let added {
                if !(await added.requestClose()) { cleanupPending = true }
            } else if let child, !(await child.close()) { cleanupPending = true }
            return false
        }
    }
    @discardableResult public func close() async -> Bool {
        guard !closed else { return !cleanupPending }
        guard !closing else { return false }; closing = true
        monitor?.cancel(); monitor = nil
        defer { closing = false }
        launchTask?.cancel()
        if let launchTask { _ = await launchTask.value }
        var released = true
        for instance in instances { if !(await instance.requestClose()) { released = false } }
        if released && nativeReady {
            do {
                let result = try object(await controller.manageMultiplex("finish"))
                guard Self.nativeBoolean(result["released"]) == true else { throw GhostermError.failed("执行端释放未确认") }
            } catch { issue = "连接关闭结果未确认：\(error)"; await controller.stopMultiplexConnection() }
        } else { await controller.stopMultiplexConnection() }
        for instance in instances where !instance.canDiscard { _ = await instance.requestClose() }
        let done = await controller.close()
        state = try? await controller.multiplexObservation(); process = try? await controller.localProcessState()
        if done && state?.peerEOF != true && process?.reaped != true { issue = "连接已释放；执行端退出未确认，请检查各实例回执" }
        temporary = try? await controller.localTemporaryState(); cleanupPending = !done; closed = done; nativeReady = !done && nativeReady
        if done { monitor?.cancel(); monitor = nil }
        return done
    }
}

@MainActor public extension AppModel {
    func validateQueuedExtensionGroup(_ launches: [ExtensionGroupInvocation]) async throws {
        guard let extensionController else { throw GhostermError.failed("此构建未启用插件运行时") }
        _ = try await ExtensionConnectionModel.prepareGroup(launches, controller: extensionController, connectionID: UUID())
    }

    @discardableResult
    func startExtensionGroup(_ invocations: [ExtensionGroupInvocation]) async -> Bool {
        guard let extensionController else { extensionConnectionIssue = "此构建未启用插件运行时"; return false }
        do {
            guard extensionConnections.filter({ !$0.canDiscard }).count < 16, let first = invocations.first else {
                throw GhostermError.failed("插件连接数量已达上限或启动组为空")
            }
            let owner = try await extensionController.makeInvocationController()
            // Creating the owner suspends on the Core worker. Recheck before
            // publishing the connection, while admission is on the main actor.
            guard !Task.isCancelled else {
                _ = await owner.close()
                return false
            }
            guard extensionConnections.filter({ !$0.canDiscard }).count < 16 else {
                _ = await owner.close()
                throw GhostermError.failed("插件连接数量已达上限")
            }
            let connection = ExtensionConnectionModel(controller: owner, title: first.localLaunch == nil ? first.sshAlias : "本机")
            extensionConnections.append(connection)
            extensionConnectionObservations[connection.id] = connection.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }
            extensionConnectionIssue = nil
            let started = await connection.start(invocations, app: self)
            if !started { extensionConnectionIssue = connection.issue }
            return started
        } catch {
            extensionConnectionIssue = "连接组启动失败：\(error)"
            return false
        }
    }
}
