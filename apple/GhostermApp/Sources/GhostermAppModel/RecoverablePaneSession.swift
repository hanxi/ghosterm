// Native pane lifecycle. Bytes remain inside RecoverableSurfaceTransport;
// this owner only handles construction, record metadata and presentation.
import Foundation
import GhostermCore
import GhostermTerminal

@MainActor final class RecoverablePaneSession {
    private weak var pane: TerminalPane?
    private let context: SessionRecordContext
    private let options: RecoverableSession.Options
    private var transport: RecoverableSurfaceTransport!
    private var authentication: RecoverableSession.Authentication?
    private var closed = false
    private let app: TerminalApp
    private let configurationObserver = UUID()

    init(pane: TerminalPane, app: TerminalApp, record: SessionRecord, directory: URL,
         options: RecoverableSession.Options,
         authentication: ((RecoverableSurfaceTransport) throws -> RecoverableSession.Authentication?)?) throws {
        let contract = options.compatibility.map { String(format: "%02x", $0) }.joined()
        guard record.endpoint.proxy == options.proxy, record.endpoint.daemon == options.daemon,
              record.endpoint.runtime == options.runtime, record.endpoint.contract == contract,
              (record.kind == .local) == (options.kind == .local), !options.catalogOnly else {
            throw GhostermError.failed("会话记录与连接目标不一致")
        }
        if let create = options.create {
            guard !record.admitted, record.session == 0, record.instance.allSatisfy({ $0 == 0 }),
                  record.nonce == create.nonce else { throw GhostermError.failed("已发送的创建请求不得重建") }
        } else {
            guard record.instance == options.instance, record.instance.contains(where: { $0 != 0 }),
                  record.session > 0 ? options.session == record.session :
                    (record.admitted && record.nonce == options.pendingNonce) else {
                throw GhostermError.failed("恢复缺少原会话身份或创建查询信息")
            }
        }
        self.pane = pane; self.options = options; self.app = app
        context = SessionRecordContext(record: record, directory: directory)
        let metadata = context
        transport = RecoverableSurfaceTransport(app: app, callbacks: .init(
            surface: { [weak self] surface in guard let self, !self.closed else { return }; self.pane?.acceptRecoverableSurface(surface) },
            state: { [weak self] info in guard let self, !self.closed else { return }; self.pane?.acceptRecoverableState(info) },
            failure: { [weak self] message in guard let self, !self.closed else { return }; self.pane?.acceptRecoverableFailure(message) },
            notice: { [weak self] message in guard let self, !self.closed else { return }; self.pane?.acceptRecoverableNotice(message) },
            admission: { info, completion in metadata.admit(info, completion: completion) },
            identity: { metadata.observe($0) }))
        self.authentication = try authentication?(transport)
        if let style = app.resolvedStyle() { transport.requestStyle(style) }
        else { throw GhostermError.failed("无法读取终端主题配置") }
        app.observeConfiguration(configurationObserver) { [weak self] in
            guard let self, !self.closed else { return }
            if let style = self.app.resolvedStyle() { self.transport.requestStyle(style) }
            else { self.pane?.acceptRecoverableFailure("无法读取终端主题配置") }
        }
        context.onChange = { [weak self] record in guard let self, !self.closed else { return }; self.pane?.acceptRecoverableRecord(record) }
        context.onError = { [weak self] message in guard let self, !self.closed else { return }; self.transport.detach(); self.pane?.acceptRecoverableFailure(message) }
    }
    func start() {
        let transport = transport!, options = options, authentication = authentication
        context.persistIntent { [weak self] result in
            DispatchQueue.main.async { [weak self] in
                guard let self, !self.closed else { return }
                switch result {
                case .failure(let error): self.pane?.acceptRecoverableFailure(error.localizedDescription)
                case .success:
                    transport.owner.async {
                        do { try transport.start(options: options, authentication: authentication) }
                        catch { let message = error.localizedDescription; DispatchQueue.main.async { [weak self] in
                            guard let self, !self.closed else { return }; self.pane?.acceptRecoverableFailure(message)
                        } }
                    }
                }
            }
        }
    }
    func detachThenRecord(_ completion: @escaping (Result<SessionRecord, Swift.Error>) -> Void) {
        closed = true; app.removeConfigurationObserver(configurationObserver)
        let metadata = context
        transport.detach { metadata.snapshot(completion) }
    }
    func detach() { guard !closed else { return }; closed = true; app.removeConfigurationObserver(configurationObserver); transport.detach() }
    deinit { app.removeConfigurationObserver(configurationObserver); transport?.detach() }
}
