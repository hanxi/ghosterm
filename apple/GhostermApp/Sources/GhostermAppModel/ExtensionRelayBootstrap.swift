import Foundation
import GhostermCore

/// Builds the explicit POSIX command used to start the reviewed Executor over
/// SSH. This does not discover, install, or trust a remote gterm binary.
public enum ExtensionRelayBootstrap {
    public struct Request: Sendable {
        public let executable: String
        public let executorDirectory: String
        public let executorDigest: String
        public let clientDigest: String
        public let methods: [String]
        public let hostMethods: [String]
        public let command: String
        public let arguments: [String]

        public init(executable: String, executorDirectory: String, executorDigest: String,
                    clientDigest: String, methods: [String], hostMethods: [String] = [],
                    command: String, arguments: [String] = []) {
            self.executable = executable; self.executorDirectory = executorDirectory
            self.executorDigest = executorDigest; self.clientDigest = clientDigest
            self.methods = methods; self.hostMethods = hostMethods
            self.command = command; self.arguments = arguments
        }
    }

    public static func parseArgumentsJSON(_ source: String) throws -> [String] {
        try parseStringArray(source, limit: 32, label: "命令参数")
    }

    public static func parseMethodsJSON(_ source: String) throws -> [String] {
        let methods = try parseStringArray(source, limit: 16, label: "RPC 方法")
        guard Set(methods).count == methods.count, methods.allSatisfy(validMethod) else {
            throw invalid("RPC 方法名无效或重复")
        }
        return methods
    }

    public static func parseHostMethodsJSON(_ source: String) throws -> [String] {
        let methods = try parseStringArray(source, limit: 16, label: "Host RPC 方法")
        guard Set(methods).count == methods.count, methods.allSatisfy(validMethod) else {
            throw invalid("Host RPC 方法名无效或重复")
        }
        return methods
    }

    public static func build(_ request: Request) throws -> String {
        guard !request.executable.isEmpty, !request.executorDirectory.isEmpty,
              !request.command.isEmpty else { throw invalid("请指定 gterm、Executor 目录和完整插件命令") }
        guard isDigest(request.executorDigest), isDigest(request.clientDigest) else {
            throw invalid("插件摘要必须是 64 位小写 SHA-256")
        }
        guard request.methods.count <= 16, request.methods.allSatisfy(validMethod),
              Set(request.methods).count == request.methods.count,
              request.hostMethods.count + request.methods.count <= 16,
              request.hostMethods.allSatisfy(validMethod),
              Set(request.hostMethods).count == request.hostMethods.count,
              Set(request.methods).isDisjoint(with: request.hostMethods) else {
            throw invalid("RPC 方法无效、重复或超过 16 项")
        }
        guard request.arguments.count <= 32 else { throw invalid("命令参数最多 32 项") }
        let argv = [request.executable, "relay", "--stdio", "--trust-development-digest", request.executorDigest,
                    "--client-digest", request.clientDigest]
            + request.methods.flatMap { ["--method", $0] }
            + request.hostMethods.flatMap { ["--host-method", $0] }
            + ["--", request.executorDirectory, request.command] + request.arguments
        guard argv.allSatisfy({
            !$0.utf8.contains(0) && !$0.contains("\n") && !$0.contains("\r") && $0.utf8.count <= 4096
        }) else { throw invalid("远端启动参数包含不支持的字符或过长") }
        let command = "exec " + argv.map(quote).joined(separator: " ")
        guard command.utf8.count <= 1024 else { throw invalid("远端启动命令超过 SSH 插件路由上限") }
        return command
    }

    private static func parseStringArray(_ source: String, limit: Int, label: String) throws -> [String] {
        guard source.utf8.count <= 65536, let data = source.data(using: .utf8) else {
            throw invalid("\(label) JSON 超过 64 KiB 或不是 UTF-8")
        }
        do {
            let values = try JSONDecoder().decode([String].self, from: data)
            guard values.count <= limit else { throw invalid("\(label) 最多 \(limit) 项") }
            return values
        } catch let error as GhostermError { throw error }
        catch { throw invalid("\(label) 必须是严格 JSON 字符串数组") }
    }

    private static func validMethod(_ value: String) -> Bool {
        guard value.utf8.count <= 128, let first = value.utf8.first,
              (97...122).contains(first) else { return false }
        var previousSeparator = false
        for byte in value.utf8.dropFirst() {
            let alphanumeric = (65...90).contains(byte) || (97...122).contains(byte) || (48...57).contains(byte)
            if alphanumeric { previousSeparator = false; continue }
            if byte == 46 || byte == 95 || byte == 45, !previousSeparator {
                previousSeparator = true; continue
            }
            return false
        }
        return !previousSeparator
    }

    private static func isDigest(_ value: String) -> Bool {
        value.utf8.count == 64 && value.utf8.allSatisfy {
            (48...57).contains($0) || (97...102).contains($0)
        }
    }

    private static func quote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private static func invalid(_ message: String) -> GhostermError { .failed(message) }
}
