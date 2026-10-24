// One IO owner joins the shared session coordinator to the native replica.
// Checkpoint/output bytes never cross the main queue. Only construction policy
// and surface identity cross it; the coordinator retains its borrowed snapshot.
import Foundation
import GhostermCore
import GhostermTerminal

public final class RecoverableSurfaceTransport {
    public struct Callbacks {
        public var surface: (TerminalSurface) -> Void
        public var state: (RecoverableSession.Info) -> Void
        public var failure: (String) -> Void
        public var receipt: ((RecoverableSession.Receipt) -> Void)?
        public var notice: ((String) -> Void)?
        /// Metadata only. Completion resumes CREATE after durable admission.
        public var admission: ((RecoverableSession.Info, @escaping (Result<Void, Swift.Error>) -> Void) -> Void)?
        public var identity: ((RecoverableSession.Info) -> Void)?
        public init(surface: @escaping (TerminalSurface) -> Void,
                    state: @escaping (RecoverableSession.Info) -> Void,
                    failure: @escaping (String) -> Void, receipt: ((RecoverableSession.Receipt) -> Void)? = nil, notice: ((String) -> Void)? = nil,
                    admission: ((RecoverableSession.Info, @escaping (Result<Void, Swift.Error>) -> Void) -> Void)? = nil,
                    identity: ((RecoverableSession.Info) -> Void)? = nil) {
            self.surface = surface; self.state = state; self.failure = failure; self.receipt = receipt; self.notice = notice; self.admission = admission; self.identity = identity
        }
    }
    public let owner: DispatchQueue
    private let app: TerminalApp
    private let callbacks: Callbacks
    private var session: RecoverableSession?
    private var timer: DispatchSourceTimer?
    private var surface: TerminalSurface?
    private var creating: UInt64?
    private var generation: UInt64 = 0
    private var token: UInt64 = 0
    private var imported = false
    private var enabled = false
    private var stopped = false
    private var presented: UInt64 = 0
    private struct MetadataKey: Equatable {
        let state: RecoverableSession.State
        let instance: [UInt8]
        let session, generation: UInt64
        let attempts: UInt32
        let admitted, readonly, terminal, exitKnown: Bool
        let exitCode, error, stateError: Int32
        let stop: RecoverableSession.StopState
        let stopTarget, stopRequest: UInt64
        let stopStatus: UInt32
        let stopError: Int32
        let hasProcess: Bool
        let child: Transport.ChildState
        let cwd: String?
        init(_ i: RecoverableSession.Info) {
            hasProcess = i.hasProcess; child = i.childState; cwd = i.cwd
            state = i.state; instance = i.instance; session = i.session; generation = i.generation; attempts = i.attempts
            admitted = i.hasAdmittedCreate; readonly = i.readonly; terminal = i.hasTerminal; exitKnown = i.hasExitCode
            exitCode = i.exitCode; error = i.error; stateError = i.stateError
            stop = i.stop.state; stopTarget = i.stop.session; stopRequest = i.stop.request; stopStatus = i.stop.status; stopError = i.stop.error
        }
    }
    private var lastMetadata: MetadataKey?
    private var lastReportedMetadata: MetadataKey?
    private var needsAdmission = false
    private var admissionPending: UInt64?
    private var admissionSerial: UInt64 = 0
    private var admissionDeadline: UInt64 = 0
    private var constructionError: String?
    private var lastFailure: String?
    // This lock bounds admission on the engine callback thread too. Dispatching
    // every keystroke first would merely move an unbounded queue into GCD.
    private let inputLock = NSLock()
    private var input: [(UInt64, UInt64, Data)] = []
    private var inputBytes = 0
    private var admittingGeneration: UInt64 = 0
    private var size: RecoverableSession.Size?
    private var desiredDark: Bool?
    private var latestStyle: RecoverableSession.Style?
    private var desiredStyle: RecoverableSession.Style?
    private var styleRevision: UInt64 = 0
    private var sentStyle: RecoverableSession.Style?
    private var styleSequence: UInt64?


    public init(app: TerminalApp, owner: DispatchQueue = DispatchQueue(label: "dev.ghosterm.recoverable-transport"),
                callbacks: Callbacks) {
        precondition(Thread.isMainThread)
        self.app = app; self.owner = owner; self.callbacks = callbacks
    }
    /// Call once on owner. Authentication completions must also use owner.
    public func start(options: RecoverableSession.Options, authentication: RecoverableSession.Authentication? = nil) throws {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard session == nil, !stopped else { throw GhostermError.failed("会话已启动或关闭") }
        let sink = RecoverableSession.Sink(
            snapshot: { [weak self] g, s, _, bytes, _ in self?.snapshot(g, s, bytes) ?? .failed(3) },
            event: { [weak self] g, s, event in self?.event(g, s, event) ?? .failed(3) },
            applied: { [weak self] in self?.applied() },
            invalidated: { [weak self] g, _ in self?.invalidate(g) }, receipt: { [weak self] receipt in
                guard let self else { return }
                DispatchQueue.main.async { [weak self] in self?.callbacks.receipt?(receipt) }
                if self.stopped { return }
                if receipt.kind == RecoverableSession.Command.style.rawValue,
                   receipt.rejected || (receipt.status != 0 && receipt.delivery != 2) {
                    self.constructionError = "会话不支持或未接受终端配置更新：\(receipt.status)/\(receipt.rejection)"
                }
                let notice: String?
                if receipt.delivery == 2 { notice = "输入操作结果未知，请在恢复后检查；未自动重发" }
                else if receipt.rejected || receipt.status != 0 { notice = "输入操作未被接受：\(receipt.status)/\(receipt.rejection)" }
                else { notice = nil }
                if let notice { DispatchQueue.main.async { [weak self] in self?.callbacks.notice?(notice) } }
            })
        needsAdmission = options.create != nil && callbacks.admission != nil
        session = try RecoverableSession(options: options, sink: sink, authentication: authentication, owner: owner)
        let timer = DispatchSource.makeTimerSource(queue: owner)
        timer.schedule(deadline: .now(), repeating: .milliseconds(10), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in self?.tick() }
        self.timer = timer; timer.resume()
    }
    /// Usable only on owner, for asynchronous resolver/credential/hostkey results.
    public func withSession(_ body: (RecoverableSession) -> Void) {
        dispatchPrecondition(condition: .onQueue(owner))
        if let session, !stopped { body(session) }
    }
    private func snapshot(_ g: UInt64, _ s: UInt64, _ bytes: UnsafeRawBufferPointer) -> RecoverableSession.Admission {
        if constructionError != nil { return .failed(9) }
        if creating != g {
            do {
                let policy = try TerminalSurface.ReplicaPolicy.decode(bytes)
                token &+= 1; let request = token
                creating = g; generation = g; imported = false; enabled = false
                DispatchQueue.main.async { [weak self] in
                    guard let self else { return }
                    do {
                        let made = try TerminalSurface(replica: self.app, generation: g, policy: policy)
                        self.wire(made, generation: g)
                        self.owner.async { [weak self] in
                            guard let self, !self.stopped, self.token == request, self.creating == g else { return }
                            self.surface = made
                        }
                    } catch {
                        let message = String(describing: error)
                        self.owner.async { [weak self] in
                            guard let self, self.token == request, !self.stopped else { return }
                            self.constructionError = message; self.reportFailure(message)
                        }
                    }
                }
            } catch { reportFailure(String(describing: error)); return .failed(6) }
            return .busy
        }
        guard let surface, surface.replicaGeneration == g else { return .busy }
        guard !imported else { return .failed(6) }
        guard surface.importReplica(generation: g, sequence: s, bytes: bytes) else { return .failed(6) }
        imported = true
        return .accepted
    }
    private func event(_ g: UInt64, _ s: UInt64, _ event: RecoverableSession.Event) -> RecoverableSession.Admission {
        guard g == generation, imported, let surface else { return .failed(6) }
        let accepted: Bool
        switch event {
        case .output(let bytes): accepted = surface.applyReplicaOutput(generation: g, sequence: s, bytes: bytes)
        case .resize(let size): accepted = surface.applyReplicaResize(generation: g, sequence: s, cols: size.cols, rows: size.rows)
        case .style(let style):
            accepted = surface.beginReplicaScheme(generation: g, sequence: s, dark: style.dark)
            if accepted {
                if sentStyle == style { styleSequence = s }
                let request = token
                DispatchQueue.main.async { [weak self] in
                    let ok = surface.applyReplicaStyle(generation: g, sequence: s, dark: style.dark, colors: style.colors)
                    self?.owner.async { [weak self] in
                        guard let self, self.token == request, !self.stopped else { return }
                        if !ok { self.constructionError = "终端配置状态应用失败" }
                    }
                }
            }
        case .color(let dark):
            accepted = surface.beginReplicaScheme(generation: g, sequence: s, dark: dark)
            if accepted {
                let request = token
                DispatchQueue.main.async { [weak self] in
                    // The surface is immutable per attachment. A stale operation
                    // can only touch its old readonly surface, never its successor.
                    let ok = surface.applyReplicaScheme(generation: g, sequence: s, dark: dark)
                    self?.owner.async { [weak self] in
                        guard let self, self.token == request, !self.stopped else { return }
                        if !ok { self.constructionError = "终端配色状态应用失败" }
                    }
                }
            }
        }
        return accepted ? .accepted : .failed(6)
    }
    private func applied() -> RecoverableSession.Applied? {
        guard imported, let surface, constructionError == nil else { return nil }
        do {
            guard let s = try surface.replicaApplied(generation: generation) else { return nil }
            return .init(generation: generation, sequence: s)
        } catch { constructionError = String(describing: error); return nil }
    }
    private func wire(_ surface: TerminalSurface, generation: UInt64) {
        surface.onReplicaInput = { [weak self] g, s, bytes in
            guard let self else { return }
            self.inputLock.lock(); defer { self.inputLock.unlock() }
            guard self.admittingGeneration == g, g == generation, !bytes.isEmpty else { return }
            guard bytes.count <= 262144 - self.inputBytes,
                  self.input.count + (bytes.count + 32767) / 32768 <= 128 else {
                self.admittingGeneration = 0
                self.owner.async { [weak self] in
                    guard let self, !self.stopped else { return }
                    self.reportFailure("终端输入队列已满；会话已脱离，请恢复后继续输入")
                    self.invalidate(g); self.session?.cancel()
                }
                return
            }
            // Keep every frame below the shared connection's payload limit.
            // Reserve the entire callback before appending any fragment.
            for offset in stride(from: 0, to: bytes.count, by: 32768) {
                self.input.append((g, s, bytes.subdata(in: offset..<min(offset + 32768, bytes.count))))
            }
            self.inputBytes += bytes.count
        }
        surface.onResize = { [weak self] cols, rows in
            guard let self else { return }
            self.inputLock.lock(); self.size = .init(cols: cols, rows: rows); self.inputLock.unlock()
        }
    }
    public func requestStyle(_ style: TerminalApp.Style) {
        inputLock.lock(); styleRevision &+= 1; latestStyle = .init(dark: style.dark, colors: style.colors); desiredStyle = latestStyle; inputLock.unlock()
    }
    public func requestColor(dark: Bool) {
        inputLock.lock(); desiredDark = dark; inputLock.unlock()
    }
    private func revokeInput() {
        if let surface { _ = surface.setReplicaInput(generation: generation, sequence: 0, enabled: false) }
        inputLock.lock(); admittingGeneration = 0; input.removeAll(); inputBytes = 0; inputLock.unlock()
        enabled = false
    }
    private func invalidate(_ g: UInt64) {
        guard g == generation else { return }
        revokeInput(); imported = false; creating = nil; token &+= 1
        sentStyle = nil; styleSequence = nil
        inputLock.lock(); desiredStyle = latestStyle; inputLock.unlock()
    }
    private func tick() {
        guard let session, !stopped else { return }
        if let error = constructionError { reportFailure(error); detachOnOwner(); return }
        if admissionPending != nil {
            if DispatchTime.now().uptimeNanoseconds >= admissionDeadline {
                admissionPending = nil; reportFailure("无法及时保存会话记录，本次连接已取消")
                detachOnOwner()
            }
            return
        }
        let state = session.step()
        guard let info = session.info else { return }
        if needsAdmission, info.hasAdmittedCreate, let admit = callbacks.admission {
            admissionSerial &+= 1; let serial = admissionSerial
            admissionPending = serial; admissionDeadline = DispatchTime.now().uptimeNanoseconds + 5_000_000_000
            admit(info) { [weak self] result in
                self?.owner.async { [weak self] in
                    guard let self, !self.stopped, self.admissionPending == serial else { return }
                    self.admissionPending = nil
                    switch result {
                    case .success: self.needsAdmission = false
                    case .failure(let error): self.reportFailure("无法保存会话记录：\(error.localizedDescription)"); self.detachOnOwner()
                    }
                }
            }
            return
        }
        if state == .live, !info.readonly, let applied = applied(), let surface {
            let styleReady = prepareStyle(session, applied: applied.sequence)
            if !enabled && styleReady {
                enabled = surface.setReplicaInput(generation: generation, sequence: applied.sequence, enabled: true)
                inputLock.lock(); admittingGeneration = enabled ? generation : 0; inputLock.unlock()
            }
            if enabled && styleReady { drain(session, sequence: applied.sequence) }
            if enabled && presented != generation {
                presented = generation
                DispatchQueue.main.async { [weak self] in self?.callbacks.surface(surface) }
            }
        } else if state == .readonly || state == .ended || (state == .live && info.readonly) {
            if enabled { revokeInput() }
            if imported, presented != generation, let surface, applied() != nil {
                presented = generation
                DispatchQueue.main.async { [weak self] in self?.callbacks.surface(surface) }
            }
        } else if enabled { invalidate(generation) }
        let metadata = MetadataKey(info)
        if lastMetadata != metadata {
            lastMetadata = metadata; callbacks.identity?(info)
        }
        if lastReportedMetadata != metadata, state != .live || info.readonly || enabled {
            lastReportedMetadata = metadata
            DispatchQueue.main.async { [weak self] in self?.callbacks.state(info) }
        }
        if state == .failed { reportFailure("可恢复会话失败：\(info.error)") }
    }
    private func prepareStyle(_ session: RecoverableSession, applied: UInt64) -> Bool {
        if sentStyle != nil {
            guard let sequence = styleSequence, applied >= sequence else { return false }
            sentStyle = nil; styleSequence = nil
        }
        inputLock.lock(); let desired = desiredStyle, revision = styleRevision; inputLock.unlock()
        guard let desired else { return true }
        let epoch = token
        // Core submission can synchronously invalidate on a clock/deadline
        // failure. Never hold the callback's inputLock across any Core call.
        let result = session.style(generation: generation, sequence: applied, style: desired).0
        guard !stopped, epoch == token else { return false }
        if result == .queued {
            sentStyle = desired
            inputLock.lock(); if styleRevision == revision { desiredStyle = nil }; inputLock.unlock()
        } else if result != .backpressure && result != .locked { constructionError = "终端配置未被会话接受：\(result)"; reportFailure(constructionError!) }
        return false
    }
    private func drain(_ session: RecoverableSession, sequence: UInt64) {
        while true {
            inputLock.lock(); let first = input.first; inputLock.unlock()
            guard let first else { break }
            let epoch = token
            let result = first.2.withUnsafeBytes { session.input(generation: first.0, sequence: first.1, bytes: $0).0 }
            guard !stopped, epoch == token else { return }
            if result == .backpressure { return }
            inputLock.lock(); inputBytes -= first.2.count; input.removeFirst(); inputLock.unlock()
            if result != .queued {
                inputLock.lock(); input.removeAll(); inputBytes = 0; inputLock.unlock()
                if result == .invalid || result == .unavailable { reportFailure("终端输入未被接受：\(result)") }
                return
            }
        }
        inputLock.lock(); let size = self.size, dark = desiredDark; inputLock.unlock()
        let epoch = token
        if let size {
            let result = session.resize(generation: generation, sequence: sequence, size: size).0
            guard !stopped, epoch == token else { return }
            if result == .queued { inputLock.lock(); if self.size == size { self.size = nil }; inputLock.unlock() }
        }
        if let dark {
            let result = session.color(generation: generation, sequence: sequence, dark: dark).0
            guard !stopped, epoch == token else { return }
            if result == .queued { inputLock.lock(); if desiredDark == dark { desiredDark = nil }; inputLock.unlock() }
        }
    }
    private func reportFailure(_ message: String) {
        guard lastFailure != message else { return }; lastFailure = message
        DispatchQueue.main.async { [weak self] in self?.callbacks.failure(message) }
    }
    /// Detach only. Explicit source termination is a separate control operation.
    public func detach(completion: (() -> Void)? = nil) {
        owner.async { [self] in detachOnOwner(); completion?() }
    }
    private func detachOnOwner() {
        dispatchPrecondition(condition: .onQueue(owner))
        guard !stopped else { return }
        // Stop synchronously: another already-enqueued timer event must never
        // flush CREATE after a failed or expired persistence admission.
        stopped = true; timer?.cancel(); timer = nil; admissionPending = nil
        invalidate(generation); session?.cancel(); reap()
    }
    private func reap() {
        guard let session else { return }
        if session.close() { self.session = nil; surface = nil }
        else { owner.asyncAfter(deadline: .now() + .milliseconds(10)) { [self] in reap() } }
    }
    deinit { timer?.cancel() }
}
