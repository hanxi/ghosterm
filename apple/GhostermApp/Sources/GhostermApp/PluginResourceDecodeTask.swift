import Foundation

/// Runs bounded preview decoding away from the UI executor and discards a
/// completed value when its view task was cancelled while decoding.
enum PluginResourceDecodeTask {
    static func run<Value: Sendable>(
        _ operation: @escaping @Sendable () async -> Value
    ) async -> Value? {
        let worker = Task.detached(priority: .utility) { () -> Value? in
            let value = await operation()
            return Task.isCancelled ? nil : value
        }
        let value = await withTaskCancellationHandler {
            await worker.value
        } onCancel: {
            worker.cancel()
        }
        return Task.isCancelled ? nil : value
    }
}
