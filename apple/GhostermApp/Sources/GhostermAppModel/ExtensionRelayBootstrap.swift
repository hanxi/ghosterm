import Foundation
import GhostermCore

/// Builds the explicit POSIX command used to start the reviewed Executor over
/// SSH. This does not discover, install, or trust a remote gterm binary.
public enum ExtensionRelayBootstrap {
    public struct ScopedRoot: Sendable, Equatable {
        public let id: String
        public let directory: String

        public init(id: String, directory: String) {
            self.id = id; self.directory = directory
        }
    }

    public struct ProcessProgram: Sendable, Equatable {
        public let name: String
        public let executable: String
        public let version: String
        public let sha256: String

        public init(name: String, executable: String, version: String, sha256: String) {
            self.name = name; self.executable = executable
            self.version = version; self.sha256 = sha256
        }
    }

    public struct Request: Sendable {
        public let executable: String
        public let executorDirectory: String
        public let executorDigest: String
        public let clientDigest: String
        public let methods: [String]
        public let hostMethods: [String]
        public let scopedRoots: [ScopedRoot]
        public let processPrograms: [ProcessProgram]
        public let command: String
        public let arguments: [String]

        public init(executable: String, executorDirectory: String, executorDigest: String,
                    clientDigest: String, methods: [String], hostMethods: [String] = [],
                    scopedRoots: [ScopedRoot] = [],
                    processPrograms: [ProcessProgram] = [],
                    command: String, arguments: [String] = []) {
            self.executable = executable; self.executorDirectory = executorDirectory
            self.executorDigest = executorDigest; self.clientDigest = clientDigest
            self.methods = methods; self.hostMethods = hostMethods
            self.scopedRoots = scopedRoots
            self.processPrograms = processPrograms
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

    public static func parseProcessProgramsJSON(_ source: String) throws -> [ProcessProgram] {
        guard source.utf8.count <= 65536, let data = source.data(using: .utf8) else {
            throw invalid("进程映射 JSON 超过 64 KiB 或不是 UTF-8")
        }
        do {
            guard let values = try JSONSerialization.jsonObject(with: data) as? [[String: Any]],
                  values.count <= 16 else { throw invalid("进程映射必须是最多 16 项的 JSON 对象数组") }
            let expected: Set<String> = ["name", "executable", "version", "sha256"]
            let programs = try values.map { value -> ProcessProgram in
                guard Set(value.keys) == expected,
                      let name = value["name"] as? String,
                      let executable = value["executable"] as? String,
                      let version = value["version"] as? String,
                      let sha256 = value["sha256"] as? String else {
                    throw invalid("每个进程映射必须且只能包含 name、executable、version、sha256")
                }
                return ProcessProgram(name: name, executable: executable, version: version, sha256: sha256)
            }
            guard Set(programs.map(\.name)).count == programs.count,
                  programs.allSatisfy(validProcessProgram) else {
                throw invalid("进程映射字段无效、重复或超过限制")
            }
            return programs
        } catch let error as GhostermError { throw error }
        catch { throw invalid("进程映射必须是严格 JSON 对象数组") }
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
        guard request.scopedRoots.count <= 8,
              Set(request.scopedRoots.map(\.id)).count == request.scopedRoots.count,
              request.scopedRoots.allSatisfy({ validRootID($0.id) && !$0.directory.isEmpty }) else {
            throw invalid("受控目录必须有唯一的 ASCII ID 和非空远端路径，最多 8 项")
        }
        guard request.processPrograms.count <= 16,
              Set(request.processPrograms.map(\.name)).count == request.processPrograms.count,
              request.processPrograms.allSatisfy(validProcessProgram),
              request.processPrograms.isEmpty || !request.scopedRoots.isEmpty else {
            throw invalid("进程映射必须唯一有效，并且需要至少一个显式 workspace root")
        }
        let argv = [request.executable, "relay", "--stdio", "--trust-development-digest", request.executorDigest,
                    "--client-digest", request.clientDigest]
            + request.methods.flatMap { ["--method", $0] }
            + request.hostMethods.flatMap { ["--host-method", $0] }
            + request.scopedRoots.flatMap { ["--root", $0.id, $0.directory] }
            + request.processPrograms.flatMap {
                ["--process-program", $0.name, $0.executable, $0.version, $0.sha256]
            }
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

    private static func validRootID(_ value: String) -> Bool {
        let bytes = Array(value.utf8)
        guard let first = bytes.first, bytes.count <= 64, (97...122).contains(first) else { return false }
        return bytes.dropFirst().allSatisfy {
            (97...122).contains($0) || (48...57).contains($0) || $0 == 45 || $0 == 95 || $0 == 46
        }
    }

    private static func validProcessProgram(_ program: ProcessProgram) -> Bool {
        let name = program.name.utf8
        let validName = !name.isEmpty && name.count <= 128 && name.allSatisfy {
            (65...90).contains($0) || (97...122).contains($0) || (48...57).contains($0) ||
                $0 == 46 || $0 == 95 || $0 == 45
        }
        return validName && program.executable.hasPrefix("/") &&
            program.executable.utf8.count <= 4096 && !program.executable.contains("\n") &&
            !program.executable.contains("\r") && !program.executable.utf8.contains(0) &&
            !program.version.isEmpty && program.version.utf8.count <= 128 &&
            !program.version.contains("\n") && !program.version.contains("\r") &&
            !program.version.utf8.contains(0) && isDigest(program.sha256)
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
