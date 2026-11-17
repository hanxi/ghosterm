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
    @Published public private(set) var state: ExtensionController.MultiplexState?
    @Published public private(set) var process: ExtensionController.ProcessState?
    @Published public private(set) var temporary: ExtensionLocalTemporaryState?
    public var canDiscard: Bool { closed && !busy && !cleanupPending }
    private var launchTask: Task<Void, Never>?
    private var monitor: Task<Void, Never>?
    private var observations: [AnyCancellable] = []
    init(controller: ExtensionController, title: String) { self.controller = controller; self.title = title }

    func start(_ launches: [ExtensionGroupInvocation], app: AppModel) async {
        guard launchTask == nil, !busy, !closed else { return }
        busy = true
        let task = Task { await self.startOnMain(launches, app: app) }
        launchTask = task
        await withTaskCancellationHandler(operation: { await task.value }, onCancel: { task.cancel() })
        launchTask = nil; busy = false
    }

    private func startOnMain(_ launches: [ExtensionGroupInvocation], app: AppModel) async {
        do {
            guard let first = launches.first, (1...8).contains(launches.count), launches.allSatisfy({
                $0.sshAlias == first.sshAlias && $0.localLaunch == first.localLaunch &&
                $0.temporaryDirectory == first.temporaryDirectory && $0.request.executable == first.request.executable
            }) else { throw GhostermError.failed("同一启动组必须使用同一明确目标、gterm、临时目录和本机启动授权") }
            var requests: [ExtensionRelayBootstrap.Request] = []
            for launch in launches {
                try Task.checkCancellation()
                let request = launch.request
                let selected = try await controller.resolve(command: request.command, selectedID: launch.packageID, selectedDigest: request.clientDigest)
                guard selected.status == 0, selected.packageID == launch.packageID,
                      selected.packageDigest == request.clientDigest, selected.command?.path == request.command else {
                    throw GhostermError.failed("启动组中有 Client 摘要或命令尚未批准")
                }
                _ = try await controller.decodeArguments(packageID: launch.packageID, packageDigest: request.clientDigest,
                    command: request.command, arguments: request.arguments)
                try launch.clientWorkspace?.validate(requiredCapabilities: selected.requiredCapabilities)
                let plan = try ExtensionInstanceModel.clientPlan(selected: selected, methods: request.methods, hostMethods: request.hostMethods)
                requests.append(.init(executable: request.executable, executorDirectory: request.executorDirectory,
                    executorDigest: request.executorDigest, clientDigest: request.clientDigest, methods: plan.methods,
                    hostMethods: request.hostMethods, clientCapabilities: plan.services, scopedRoots: request.scopedRoots,
                    processPrograms: request.processPrograms, processEnvironmentNames: request.processEnvironmentNames,
                    execution: request.execution, attachment: request.attachment, command: request.command, arguments: request.arguments))
            }
            let scopes = launches.enumerated().map { index, launch in
                ExtensionController.MultiplexScope(instance: launch.id.uuidString.replacingOccurrences(of: "-", with: "").lowercased(), epoch: UInt64(index + 1))
            }
            let command = try ExtensionRelayBootstrap.buildMultiplex(connection: id.uuidString.replacingOccurrences(of: "-", with: "").lowercased(),
                scopes: scopes, requests: requests, local: first.localLaunch != nil, temporaryDirectory: first.temporaryDirectory)
            try Task.checkCancellation()
            if let local = first.localLaunch {
                guard local.temporaryDirectory == first.temporaryDirectory else { throw GhostermError.failed("连接组与本机启动必须使用同一临时目录") }
                let names = Array(Set(requests.flatMap(\.processEnvironmentNames))).sorted()
                _ = try await controller.openLocalChannel(executable: first.request.executable, arguments: command.argv, launch: local, environmentNames: names)
            } else {
                _ = try await app.authenticatePluginSSHChannel(alias: first.sshAlias, bootstrap: command.command, controller: controller)
            }
            try Task.checkCancellation()
            let children = try await controller.adoptMultiplexConnection(connection: id.uuidString.replacingOccurrences(of: "-", with: "").lowercased(), scopes: scopes)
            instances = children.map { ExtensionInstanceModel(controller: $0) }
            observations = instances.map { $0.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() } }
            instances.forEach { app.showExtensionWorkspace($0) }
            // Register every Session promptly; one awaiting consent cannot hold
            // the other seven behind its five-second Executor handshake window.
            await withTaskGroup(of: Void.self) { group in
                for (instance, launch) in zip(instances, launches) {
                    group.addTask { @MainActor in
                        let request = launch.request
                        await instance.startExtensionClient(packageID: launch.packageID, packageDigest: request.clientDigest,
                            command: request.command, arguments: request.arguments, sshAlias: launch.sshAlias,
                            executable: request.executable, executorDirectory: request.executorDirectory, executorDigest: request.executorDigest,
                            methods: request.methods, hostMethods: request.hostMethods, scopedRoots: request.scopedRoots,
                            processPrograms: request.processPrograms, processEnvironmentNames: request.processEnvironmentNames,
                            executorExecution: request.execution, clientWorkspace: launch.clientWorkspace, localLaunch: launch.localLaunch,
                            multiplexedConnectionCommand: command.command)
                    }
                }
            }
            try Task.checkCancellation()
            monitor = Task { await self.observeConnection() }
        } catch {
            issue = "连接组启动失败：\(error)"
            for instance in instances { _ = await instance.requestClose() }
            cleanupPending = !(await controller.close())
            closed = !cleanupPending
            process = try? await controller.localProcessState()
            temporary = try? await controller.localTemporaryState()
        }
    }

    private func observeConnection() async {
        while !Task.isCancelled && !closed {
            do { state = try await controller.stepMultiplexConnection() }
            catch { issue = "共享连接状态查不出来：\(error)"; return }
            if state?.error != 0 && instances.contains(where: { !$0.canDiscard }) { issue = "共享连接已结束或发生错误（\(state?.error ?? -1)），请检查各实例结果" }
            if !busy && instances.allSatisfy(\.canDiscard) { _ = await close(); return }
            do { try await Task.sleep(for: .milliseconds(100)) } catch { return }
        }
    }

    @discardableResult public func close() async -> Bool {
        guard !closed else { return !cleanupPending }
        launchTask?.cancel()
        if let launchTask { await launchTask.value }
        for instance in instances { if !(await instance.requestClose()) { cleanupPending = true; return false } }
        let done = await controller.close()
        state = try? await controller.multiplexObservation()
        process = try? await controller.localProcessState()
        if done && state?.peerEOF != true && process?.reaped != true { issue = "连接已释放；执行端退出未确认，具体业务结果请检查实例回执" }
        temporary = try? await controller.localTemporaryState()
        cleanupPending = !done; closed = done
        if done { monitor?.cancel(); monitor = nil }
        return done
    }
}

@MainActor public extension AppModel {
    func startExtensionGroup(_ invocations: [ExtensionGroupInvocation]) async {
        guard let extensionController else { extensionConnectionIssue = "此构建未启用插件运行时"; return }
        do {
            guard extensionConnections.filter({ !$0.canDiscard }).count < 16, let first = invocations.first else {
                throw GhostermError.failed("插件连接数量已达上限或启动组为空")
            }
            let owner = try await extensionController.makeInvocationController()
            let connection = ExtensionConnectionModel(controller: owner, title: first.localLaunch == nil ? first.sshAlias : "本机")
            extensionConnections.append(connection)
            extensionConnectionObservations[connection.id] = connection.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }
            extensionConnectionIssue = nil
            await connection.start(invocations, app: self)
        } catch { extensionConnectionIssue = "连接组启动失败：\(error)" }
    }
}
