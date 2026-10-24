// Product session lifecycle. Platform prompts and metadata live here; all
// terminal bytes continue directly from Transport into TerminalSurface.
import Foundation
import GhostermCore
import GhostermTerminal

@MainActor final class NativeSessionCoordinator {
    let directory: URL
    private let runtimeRoot: URL
    private let remoteInstallRoot, remoteRuntimeDirectory: String?
    private let io = DispatchQueue(label: "dev.ghosterm.session-catalog")
    private let loadCatalog: () throws -> SessionRuntimeCatalog
    private let credentials: (SessionRecord.Target) -> SessionCredentialSelector
    private let changed: ([SessionRecord]) -> Void
    private let failed: (String) -> Void
    private var preparing: [UUID: UUID] = [:]
    private var installers: [UUID: SessionSetupTransport] = [:]
    private var controls: [UUID: SessionControlTransport] = [:]
    private var managing = Set<UUID>()
    private var records: [SessionRecord] = []
    private var attached: [UUID: UUID] = [:] // pane -> durable record

    init(directory: URL = URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support/Ghosterm/sessions"),
         runtimeRoot: URL = URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent(".local/state/ghosterm"),
         loadCatalog: @escaping () throws -> SessionRuntimeCatalog = SessionRuntimeCatalog.loadDefault,
         remoteInstallRoot: String? = nil, remoteRuntimeDirectory: String? = nil,
         credentials: @escaping (SessionRecord.Target) -> SessionCredentialSelector,
         changed: @escaping ([SessionRecord]) -> Void, failed: @escaping (String) -> Void) {
        self.directory = directory; self.runtimeRoot = runtimeRoot; self.loadCatalog = loadCatalog
        self.remoteInstallRoot = remoteInstallRoot; self.remoteRuntimeDirectory = remoteRuntimeDirectory
        self.credentials = credentials; self.changed = changed; self.failed = failed
    }
    func refresh(completion: ((Result<[SessionRecord], Swift.Error>) -> Void)? = nil) {
        let directory = directory, owner = io
        // 还挂在窗格上的记录不碰：它的 context 还活着，删了会被下一次 save 写回来。
        let pinned = Set(attached.values)
        owner.async { [weak self] in
            let result = Result { try Self.storeOperation {
                let store = SessionRecordStore(directory: directory, owner: owner)
                let rows = try store.load()
                var survivors: [SessionRecord] = []
                for row in rows {
                    // 已结束的会话在最后一个客户端脱离后就被 daemon 回收，恢复只会
                    // 得到「找不到」——留着就是侧栏里一排点不动的死行。读到即删。
                    if row.disposition == .ended, !pinned.contains(row.id) { try store.forget(row.id) }
                    else { survivors.append(row) }
                }
                return survivors
            } }
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                switch result { case .success(let rows): self.records = rows; self.publish()
                case .failure(let error): self.failed(sessionMessage(error)) }
                completion?(result)
            }
        }
    }
    nonisolated private static func storeOperation<T>(_ operation: () throws -> T) throws -> T {
        precondition(!Thread.isMainThread)
        let deadline = ProcessInfo.processInfo.systemUptime + 4
        while true {
            do { return try operation() }
            catch SessionRecordStore.Error.busy {
                guard ProcessInfo.processInfo.systemUptime < deadline else { throw SessionRecordStore.Error.busy }
                Thread.sleep(forTimeInterval: 0.005)
            }
        }
    }
    /// 交给 UI 的清单：**只列还打得开的**。已结束的记录在 `refresh` 里就被从
    /// 存储上删掉了，但终止回执到最终刷新之间窗格里还留着一条 `.ended`，
    /// 不在这里挡一下就会闪出一条点不动的死行。
    private func publish() {
        changed(records.filter { !attached.values.contains($0.id) && $0.disposition != .ended })
    }
    private func observe(_ record: SessionRecord) {
        if let i = records.firstIndex(where: { $0.id == record.id }) { records[i] = record } else { records.append(record) }
        publish()
    }
    func cancelPreparation(_ pane: TerminalPane) {
        preparing.removeValue(forKey: pane.id); installers.removeValue(forKey: pane.id)?.cancel()
    }
    private func catalog(for pane: TerminalPane, then: @escaping (SessionRuntimeCatalog) throws -> Void) {
        cancelPreparation(pane)
        pane.onStop = { [weak self, weak pane] in if let pane { self?.cancelPreparation(pane) } }
        pane.onExplicitClose = { [weak self] in self?.close($0) }
        let ticket = UUID(); preparing[pane.id] = ticket; let loader = loadCatalog
        io.async { [weak self, weak pane] in
            let result = Result { try loader() }
            DispatchQueue.main.async { [weak self, weak pane] in
                guard let self, let pane, self.preparing[pane.id] == ticket else { return }
                do { try then(result.get()) }
                catch { self.preparing.removeValue(forKey: pane.id); pane.markFailed(sessionMessage(error)) }
            }
        }
    }
    private func options(_ record: SessionRecord, _ catalog: SessionRuntimeCatalog, create: RecoverableSession.Create? = nil) throws -> RecoverableSession.Options {
        try catalog.requireCompatible(record)
        var options = RecoverableSession.Options(kind: record.kind == .local ? .local : .ssh,
            proxy: record.endpoint.proxy, daemon: record.endpoint.daemon, runtime: record.endpoint.runtime,
            compatibility: catalog.compatibility, format: 0x47545643, create: create)
        if create == nil {
            guard record.instance.contains(where: { $0 != 0 }), record.session > 0 || record.admitted else {
                throw GhostermError.failed("创建未获确认，不能自动重建；请检查后显式新建会话")
            }
            options.instance = record.instance; options.session = record.session
            if record.session == 0 { guard let nonce = record.nonce else { throw GhostermError.failed("缺少原创建查询标识") }; options.pendingNonce = nonce }
        }
        return options
    }
    private func attach(_ record: SessionRecord, pane: TerminalPane, app: TerminalApp, catalog: SessionRuntimeCatalog,
                        create: RecoverableSession.Create? = nil) throws {
        let options = try options(record, catalog, create: create)
        let selector = record.target.map(credentials)
        pane.onSessionRecord = { [weak self] in self?.observe($0) }
        pane.onExplicitClose = { [weak self] in self?.close($0) }
        try pane.attachRecoverable(app: app, record: record, directory: directory, options: options,
            authentication: selector.map { selector in { transport in
                let broker = SessionAuthenticationBroker(owner: transport.owner, options: Self.ssh(record.target!), prompts: selector.prompts,
                    endpoint: { [weak transport] body in transport?.withSession(body) },
                    failure: { [weak pane] in pane?.acceptRecoverableFailure($0) })
                return broker.callbacks
            } })
        preparing.removeValue(forKey: pane.id); attached[pane.id] = record.id; observe(record)
    }
    private static func ssh(_ target: SessionRecord.Target) -> SSHOptions {
        SSHOptions(host: target.host, port: target.port, username: target.user, knownHostsPath: target.knownHosts)
    }
    func local(pane: TerminalPane, app: TerminalApp, shell: String?, cwd: String) {
        catalog(for: pane) { [weak self, weak pane] catalog in
            guard let self, let pane else { return }
            let runtime = self.runtimeRoot.appendingPathComponent(String(catalog.contract.prefix(16))).path
            let endpoint = SessionRecord.Endpoint(proxy: catalog.localDirectory.appendingPathComponent("ghosterm-session-proxy").path,
                daemon: catalog.localDirectory.appendingPathComponent("ghosterm-sessiond").path, runtime: runtime, contract: catalog.contract)
            let nonce = withUnsafeBytes(of: UUID().uuid) { Array($0) }
            let record = SessionRecord(kind: .local, endpoint: endpoint, nonce: nonce)
            try self.attach(record, pane: pane, app: app, catalog: catalog, create: .init(nonce: nonce, executable: shell ?? "/bin/zsh", cwd: cwd, size: .init(cols: 80, rows: 24)))
        }
    }
    private static let remoteShellCommand = "exec \"$SHELL\" -l"
    func remote(pane: TerminalPane, app: TerminalApp, target: SessionRecord.Target, cwd: String? = nil) {
        catalog(for: pane) { [weak self, weak pane] catalog in
            guard let self, let pane else { return }
            self.install(pane: pane, target: target, catalog: catalog) { [weak self, weak pane] result, selected in
                guard let self, let pane else { return }
                do {
                    let installed = try result.get(); let nonce = withUnsafeBytes(of: UUID().uuid) { Array($0) }
                    var target = target; if let selected { target.identityPaths = selected }
                    let record = SessionRecord(kind: .ssh, endpoint: installed.endpoint, target: target, nonce: nonce)
                    try self.attach(record, pane: pane, app: app, catalog: catalog,
                        create: .init(nonce: nonce, executable: "/bin/sh", cwd: cwd ?? installed.linux.home,
                            arguments: ["-c", Self.remoteShellCommand], size: .init(cols: 80, rows: 24), direct: true))
                } catch { self.preparing.removeValue(forKey: pane.id); pane.markFailed(sessionMessage(error)) }
            }
        }
    }
    private func install(pane: TerminalPane, target: SessionRecord.Target, catalog: SessionRuntimeCatalog, runtime: String? = nil,
                         completion: @escaping (Result<SessionSetupTransport.Installed, Swift.Error>, [String]?) -> Void) {
        let selector = credentials(target), ticket = preparing[pane.id]
        let task = SessionSetupTransport(options: Self.ssh(target), bundles: catalog.linuxBundles, contract: catalog.contract,
            installRoot: remoteInstallRoot, runtimeDirectory: runtime ?? remoteRuntimeDirectory, prompts: selector.prompts, progress: { [weak self, weak pane] message in
                guard let self, let pane, self.preparing[pane.id] == ticket else { return }; pane.acceptRecoverableNotice(message)
            }) { [weak self, weak pane] result in
                guard let self, let pane, self.preparing[pane.id] == ticket else { return }
                self.installers.removeValue(forKey: pane.id)
                // Progress notices are not terminal state: the failure card or the
                // attached surface must not keep a stale "uploading…" line.
                pane.dismissSessionNotice()
                completion(result, selector.acceptedIdentityPaths)
            }
        installers[pane.id] = task; task.start()
    }
    func resume(_ record: SessionRecord, pane: TerminalPane, app: TerminalApp) {
        guard !attached.values.contains(record.id) else { pane.markFailed("此会话已经打开"); return }
        catalog(for: pane) { [weak self, weak pane] catalog in
            guard let self, let pane else { return }; try catalog.requireCompatible(record)
            _ = try self.options(record, catalog) // never install or CREATE an unadmitted intent
            if let target = record.target {
                self.install(pane: pane, target: target, catalog: catalog, runtime: record.endpoint.runtime) { [weak self, weak pane] result, selected in
                    guard let self, let pane else { return }
                    do {
                        let installed = try result.get(); var next = record; next.endpoint = installed.endpoint
                        if let selected { next.target?.identityPaths = selected }
                        self.remap(next, pane: pane, app: app, catalog: catalog)
                    } catch { self.preparing.removeValue(forKey: pane.id); pane.markFailed(sessionMessage(error)) }
                }
            } else {
                var next = record
                next.endpoint.proxy = catalog.localDirectory.appendingPathComponent("ghosterm-session-proxy").path
                next.endpoint.daemon = catalog.localDirectory.appendingPathComponent("ghosterm-sessiond").path
                self.remap(next, pane: pane, app: app, catalog: catalog)
            }
        }
    }
    private func remap(_ record: SessionRecord, pane: TerminalPane, app: TerminalApp, catalog: SessionRuntimeCatalog) {
        let directory = directory, owner = io, ticket = preparing[pane.id]
        owner.async { [weak self, weak pane] in
            let result = Result { try Self.storeOperation { try SessionRecordStore(directory: directory, owner: owner).upsert(record) } }
            DispatchQueue.main.async { [weak self, weak pane] in
                guard let self, let pane, self.preparing[pane.id] == ticket else { return }
                do { try result.get(); try self.attach(record, pane: pane, app: app, catalog: catalog) }
                catch { self.preparing.removeValue(forKey: pane.id); pane.markFailed(sessionMessage(error)) }
            }
        }
    }
    // Explicit close differs from quit/deinit: drain the Transport owner before
    // reading durable admission, which may be newer than the main-thread view.
    func close(_ pane: TerminalPane) {
        cancelPreparation(pane); attached.removeValue(forKey: pane.id)
        guard pane.sessionRecord != nil else { publish(); return }
        pane.detachThenRecord { [self] result in
            DispatchQueue.main.async { [self] in
                switch result {
                case .failure(let error): self.failed(sessionMessage(error)); self.refresh()
                case .success(let record):
                    self.observe(record)
                    if record.admitted || record.session > 0 { self.control(record, operation: .terminate) }
                }
            }
        }
    }
    func detached(_ pane: TerminalPane) {
        cancelPreparation(pane); attached.removeValue(forKey: pane.id)
        if pane.sessionRecord != nil {
            pane.detachThenRecord { [weak self] result in DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                switch result { case .success(let record): self.observe(record)
                case .failure(let error): self.failed(sessionMessage(error)) }
                self.refresh()
            } }
        } else { pane.stop(); refresh() }
    }
    func control(_ record: SessionRecord, operation: SessionControlTransport.Operation) {
        guard !managing.contains(record.id) else { return }
        managing.insert(record.id); publish()
        let loader = loadCatalog
        io.async { [self] in
            let result = Result { let catalog = try loader(); try catalog.requireCompatible(record) }
            DispatchQueue.main.async { [self] in
                switch result {
                case .success: self.startControl(record, operation: operation)
                case .failure(let error): self.managing.remove(record.id); self.failed(sessionMessage(error)); self.publish()
                }
            }
        }
    }
    private func startControl(_ record: SessionRecord, operation: SessionControlTransport.Operation) {
        let task = SessionControlTransport(record: record, directory: directory, operation: operation,
            recordChanged: { [weak self] in self?.observe($0) }) { [self] result in
                self.controls.removeValue(forKey: record.id); self.managing.remove(record.id)
                if case .failure(let error) = result { self.failed(sessionMessage(error)) }; self.refresh()
            }
        controls[record.id] = task
        if let target = record.target {
            let selector = credentials(target)
            let broker = SessionAuthenticationBroker(owner: task.owner, options: Self.ssh(target), prompts: selector.prompts,
                endpoint: { [weak task] body in task?.withSession(body) }, failure: { [weak self] in self?.failed($0) })
            task.start(authentication: broker.callbacks)
        } else { task.start() }
    }
    func isManaging(_ id: UUID) -> Bool { managing.contains(id) }
    func forget(_ record: SessionRecord) {
        guard !attached.values.contains(record.id), !managing.contains(record.id) else { failed("请先断开会话并等待管理操作完成"); return }
        let directory = directory, owner = io
        owner.async { [weak self] in
            let result = Result { try Self.storeOperation { try SessionRecordStore(directory: directory, owner: owner).forget(record.id) } }
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                if case .failure(let error) = result { self.failed(sessionMessage(error)) }; self.refresh()
            }
        }
    }
}

func sessionMessage(_ error: Swift.Error) -> String {
    if let localized = error as? LocalizedError, let message = localized.errorDescription { return message }
    return String(describing: error)
}
