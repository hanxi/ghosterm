import Foundation
import GhostermCore

/// An observation from an explicitly selected, live foreground endpoint. It
/// declares configuration only; package matching and Session consent follow.
public struct PluginForegroundDeclaration: Sendable, Equatable, Decodable {
    public let schemaVersion: Int
    public let instance, runtime, executorDigest, clientDigest, command: String
    public let arguments, methods, hostMethods, clientCapabilities: [String]
    public let authorizationGranted: Bool
    public static let maximumBytes = 153600

    public static func parse(_ data: Data, expectedInstance: String) throws -> Self {
        func invalid() -> GhostermError { .failed("前台实例声明无效、超限或标识不匹配") }
        guard data.count <= maximumBytes, String(data: data, encoding: .utf8) != nil else { throw invalid() }
        try ExtensionRelayBootstrap.validateJSON(data)
        let keys: Set<String> = ["schemaVersion", "instance", "runtime", "executorDigest", "clientDigest",
                                 "command", "arguments", "methods", "hostMethods", "clientCapabilities", "authorizationGranted"]
        guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              Set(object.keys) == keys else { throw invalid() }
        let value = try JSONDecoder().decode(Self.self, from: data)
        func hexadecimal(_ s: String, count: Int) -> Bool {
            s.utf8.count == count && s.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
        }
        guard value.schemaVersion == 1, !value.authorizationGranted,
              hexadecimal(value.instance, count: 32), value.instance == expectedInstance,
              hexadecimal(value.executorDigest, count: 64), hexadecimal(value.clientDigest, count: 64),
              ["portable-js", "node", "exec"].contains(value.runtime),
              !value.command.isEmpty, value.command.utf8.count <= 519,
              !value.command.utf8.contains(0), value.arguments.count <= 32,
              value.arguments.allSatisfy({ $0.utf8.count <= 4096 && !$0.utf8.contains(0) }),
              value.arguments.reduce(value.command.utf8.count + 1, { $0 + $1.utf8.count + 1 }) <= 16384,
              value.methods.count + value.hostMethods.count <= 16,
              Set(value.methods).isDisjoint(with: value.hostMethods),
              value.clientCapabilities.count <= 4,
              Set(value.clientCapabilities).count == value.clientCapabilities.count,
              value.clientCapabilities.allSatisfy({ ["ui.table", "ui.diff", "ui.task", "ui.views"].contains($0) }) else { throw invalid() }
        let encoder = JSONEncoder()
        _ = try ExtensionRelayBootstrap.parseMethodsJSON(String(decoding: encoder.encode(value.methods), as: UTF8.self))
        _ = try ExtensionRelayBootstrap.parseHostMethodsJSON(String(decoding: encoder.encode(value.hostMethods), as: UTF8.self))
        return value
    }

    public static func inspectionArguments(executable: String, directory: String, instance: String, local: Bool) throws -> [String] {
        let parts = directory.split(separator: "/", omittingEmptySubsequences: false)
        guard !executable.isEmpty, !local || executable.hasPrefix("/"),
              directory.hasPrefix("/"), parts.count > 1, parts.dropFirst().allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." }),
              instance.utf8.count == 32, instance.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }),
              [executable, directory].allSatisfy({ $0.utf8.count <= 4096 && !$0.utf8.contains(0) && !$0.contains("\n") && !$0.contains("\r") }) else {
            throw GhostermError.failed("请指定 gterm、绝对私有目录与 32 位小写实例标识")
        }
        return [executable, "relay", "--inspect", directory, "--instance", instance]
    }

    public static func inspectionBootstrap(executable: String, directory: String, instance: String) throws -> String {
        let argv = try inspectionArguments(executable: executable, directory: directory, instance: instance, local: false)
        let command = "exec " + argv.map { "'" + $0.replacingOccurrences(of: "'", with: "'\\''") + "'" }.joined(separator: " ")
        guard command.utf8.count <= 1024 else { throw GhostermError.failed("检查命令超过 SSH 启动上限") }
        return command
    }
}
