import Foundation

/// Runtime-local task observations survive closing/reopening a Native view.
/// At most eight business scopes; never discard an unknown cancellation to make
/// room for another action. A new Runtime creates a fresh owner.
@MainActor public final class PluginTaskHistory {
    struct Record {
        let descriptor: PluginTaskDescriptor
        var snapshot: PluginTaskSnapshot?
        var requestID: String?
        var cancellation: PluginTaskModel.Cancellation = .idle
    }
    enum Failure: Error { case full, changedIdentity }
    private var records: [String: Record] = [:]
    public init() {}
    private func key(_ d: PluginTaskDescriptor) -> String { d.viewID + "\0" + d.revision }
    func restore(_ descriptor: PluginTaskDescriptor) throws -> Record {
        let key = key(descriptor)
        if let record = records[key] {
            guard record.descriptor == descriptor else { throw Failure.changedIdentity }
            return record
        }
        guard records.count < 8 else { throw Failure.full }
        let record = Record(descriptor: descriptor)
        records[key] = record
        return record
    }
    func observe(_ d: PluginTaskDescriptor, snapshot: PluginTaskSnapshot) {
        guard var record = records[key(d)] else { return }
        record.snapshot = snapshot; records[key(d)] = record
    }
    func action(_ d: PluginTaskDescriptor, requestID: String, state: PluginTaskModel.Cancellation) {
        guard var record = records[key(d)], record.requestID == nil || record.requestID == requestID else { return }
        record.requestID = requestID; record.cancellation = state; records[key(d)] = record
    }
    var retained: Int { records.count }
}
