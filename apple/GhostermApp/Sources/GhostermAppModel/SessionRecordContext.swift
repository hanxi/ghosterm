// Per logical session metadata actor. All disk work stays off the transport
// owner. Admission completion runs only after the atomic write has completed.
import Foundation
import GhostermCore

public final class SessionRecordContext {
    private let queue: DispatchQueue
    private let store: SessionRecordStore
    private var record: SessionRecord
    private var observationError: Swift.Error?
    public var onChange: ((SessionRecord) -> Void)?
    public var onError: ((String) -> Void)?
    public init(record: SessionRecord, directory: URL) {
        self.record = record
        queue = DispatchQueue(label: "dev.ghosterm.session-record." + record.id.uuidString)
        store = SessionRecordStore(directory: directory, owner: queue)
    }
    // Store locks are deliberately nonblocking. Absorb brief contention on the
    // dedicated metadata owner, preserving FIFO order without blocking IO/UI.
    private func retry<T>(_ operation: () throws -> T) throws -> T {
        let deadline = ProcessInfo.processInfo.systemUptime + 4
        while true {
            do { return try operation() }
            catch SessionRecordStore.Error.busy {
                guard ProcessInfo.processInfo.systemUptime < deadline else { throw SessionRecordStore.Error.busy }
                Thread.sleep(forTimeInterval: 0.005)
            }
        }
    }
    private func current() throws -> SessionRecord {
        guard let saved = try retry { try store.load() }.first(where: { $0.id == record.id }) else { return record }
        guard saved.nonce == record.nonce, saved.kind == record.kind,
              saved.endpoint.runtime == record.endpoint.runtime, saved.endpoint.contract == record.endpoint.contract,
              saved.target?.host == record.target?.host, saved.target?.user == record.target?.user,
              saved.target?.port == record.target?.port,
              record.instance.allSatisfy({ $0 == 0 }) || saved.instance == record.instance,
              record.session == 0 || saved.session == record.session else { throw SessionRecordStore.Error.invalid }
        return saved
    }
    private func save(_ next: SessionRecord) throws {
        try retry { try store.upsert(next) }; record = next
        DispatchQueue.main.async { [weak self] in self?.onChange?(next) }
    }
    public func persistIntent(_ completion: @escaping (Result<Void, Swift.Error>) -> Void) {
        queue.async { [self] in
            do { try save(current()); completion(.success(())) } catch { completion(.failure(error)) }
        }
    }
    private func merged(_ info: RecoverableSession.Info) throws -> SessionRecord {
        var next = try current()
        if info.instance.contains(where: { $0 != 0 }) {
            guard next.instance.allSatisfy({ $0 == 0 }) || next.instance == info.instance else { throw SessionRecordStore.Error.invalid }
            next.instance = info.instance
        }
        if info.session > 0 {
            guard next.session == 0 || next.session == info.session else { throw SessionRecordStore.Error.invalid }
            next.session = info.session
        }
        next.admitted = next.admitted || info.hasAdmittedCreate
        if info.hasExitCode { next.resultKnown = true; next.exitCode = info.exitCode }
        if info.stop.session == next.session, next.session > 0, next.disposition != .ended {
            switch info.stop.state {
            case .pending: next.disposition = .stopUnknown; next.stopStatus = nil; next.stopError = nil
            case .unknown: next.disposition = .stopUnknown; next.stopStatus = nil; next.stopError = info.stop.error
            case .known:
                next.stopStatus = info.stop.status; next.stopError = info.stop.error
                switch info.stop.status {
                case 0: next.disposition = .stopPending
                case 1: next.disposition = .ended
                default: next.disposition = .stopRejected
                }
            case .none: break
            }
        }
        if info.state == .ended || next.resultKnown { next.disposition = .ended }
        return next
    }
    /// Must complete successfully before the next CREATE-bearing session step.
    public func admit(_ info: RecoverableSession.Info, completion: @escaping (Result<Void, Swift.Error>) -> Void) {
        queue.async { [self] in
            do {
                guard info.hasAdmittedCreate, info.instance.contains(where: { $0 != 0 }) else { throw SessionRecordStore.Error.invalid }
                try save(merged(info)); completion(.success(()))
            } catch { completion(.failure(error)) }
        }
    }
    /// Low-frequency facts only. Closed/failed client state never means the
    /// authority process ended; only observed terminal/result facts do.
    public func observe(_ info: RecoverableSession.Info) {
        queue.async { [self] in
            do { let next = try merged(info); if next != record { try save(next) } }
            catch { observationError = error; let message = error.localizedDescription; DispatchQueue.main.async { [weak self] in self?.onError?(message) } }
        }
    }
    /// Persist uncertainty BEFORE submitting a side effect. A crash must never
    /// turn an unconfirmed TERMINATE into an automatic retry after restart.
    public func prepareStop(_ completion: @escaping (Result<Void, Swift.Error>) -> Void) {
        queue.async { [self] in
            do {
                if let observationError { throw observationError }
                var next = try current(); next.disposition = .stopUnknown; next.stopStatus = nil; next.stopError = nil
                try save(next); completion(.success(()))
            } catch { completion(.failure(error)) }
        }
    }
    public func observe(row: RecoverableSession.Row, instance: [UInt8]) {
        queue.async { [self] in
            do {
                var next = try current()
                guard next.session == row.session, next.instance == instance else { throw SessionRecordStore.Error.invalid }
                // ended 是终态：已结束的记录不能被 TERMINATE 回执再降级成停止中。
                if row.terminating, next.disposition != .ended { next.disposition = .stopPending; next.stopStatus = 0; next.stopError = 0 }
                if row.hasResult {
                    next.resultKnown = true; if row.hasExitCode { next.exitCode = row.exitCode }; next.disposition = .ended
                }
                if next.resultKnown { next.disposition = .ended }
                if next != record { try save(next) }
            } catch { observationError = error; let message = error.localizedDescription; DispatchQueue.main.async { [weak self] in self?.onError?(message) } }
        }
    }
    /// A complete catalog from the original instance proved this known ID is
    /// absent. This proves no active Source, but never fabricates an exit code.
    public func observeAbsent(instance: [UInt8], session: UInt64, completion: @escaping (Result<Void, Swift.Error>) -> Void) {
        queue.async { [self] in
            do {
                var next = try current()
                guard next.session == session, session > 0, next.instance == instance else { throw SessionRecordStore.Error.invalid }
                next.disposition = .ended
                try save(next); completion(.success(()))
            } catch { observationError = error; completion(.failure(error)) }
        }
    }
    public func snapshot(_ completion: @escaping (Result<SessionRecord, Swift.Error>) -> Void) {
        queue.async { [self] in do {
            if let observationError { throw observationError }
            completion(.success(try current()))
        } catch { completion(.failure(error)) } }
    }
    public func forget(_ completion: @escaping (Result<Void, Swift.Error>) -> Void) {
        queue.async { [self] in do { try retry { try store.forget(record.id) }; completion(.success(())) } catch { completion(.failure(error)) } }
    }
}
