import Foundation
import GhostermCore

/// Builds validated raw local argv or an explicit POSIX SSH command for the
/// reviewed Executor. Does not discover, install, or trust a gterm binary.
public enum ExtensionRelayBootstrap {
    public struct Attachment: Sendable, Equatable {
        public let directory, instance: String
        public let authorized, remoteProcesses: Bool
        public init(directory: String, instance: String, authorized: Bool, remoteProcesses: Bool = false) {
            self.directory = directory; self.instance = instance; self.authorized = authorized; self.remoteProcesses = remoteProcesses
        }
        public var reviewDescription: String {
            "连接已有前台实例：\(instance)\n私有目录：\(directory)\n运行档、根目录与程序映射沿用前台启动配置，连接不会修改。\n执行端业务进程服务：" + (remoteProcesses ? "明确允许沿用实例范围" : "未允许")
        }
    }
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
        public let prefixArguments: [String]

        public init(name: String, executable: String, version: String, sha256: String,
                    prefixArguments: [String] = []) {
            self.name = name; self.executable = executable
            self.version = version; self.sha256 = sha256
            self.prefixArguments = prefixArguments
        }
    }

    /// Explicit remote execution choice. The remote gterm verifies these claims
    /// against the real executable and captured Host SDK before activation.
    public struct NodeDependency: Sendable, Equatable {
        public let executable, version, sha256, sdkDirectory, sdkCatalogSHA256: String
        public init(executable: String, version: String, sha256: String,
                    sdkDirectory: String, sdkCatalogSHA256: String) {
            self.executable = executable; self.version = version; self.sha256 = sha256
            self.sdkDirectory = sdkDirectory; self.sdkCatalogSHA256 = sdkCatalogSHA256
        }
    }

    public struct Execution: Sendable, Equatable {
        public enum Runtime: Sendable, Equatable { case node(NodeDependency), exec }
        public let runtime: Runtime
        public let temporaryDirectory: String
        public let authorized: Bool
        public init(runtime: Runtime, temporaryDirectory: String, authorized: Bool) {
            self.runtime = runtime; self.temporaryDirectory = temporaryDirectory
            self.authorized = authorized
        }
        public var reviewDescription: String {
            reviewDescription(temporaryLabel: "执行端临时目录")
        }
        public func reviewDescription(temporaryLabel: String) -> String {
            let detail: String
            switch runtime {
            case .exec: detail = "Exec（使用已检查包内的原生入口）"
            case .node(let node):
                detail = "Node \(node.version) · \(node.executable)\n可执行 SHA-256：\(node.sha256)\nHost SDK：\(node.sdkDirectory)\nSDK catalog SHA-256：\(node.sdkCatalogSHA256)"
            }
            return detail + "\n\(temporaryLabel)：\(temporaryDirectory)\n进程执行：" + (authorized ? "本次已明确允许" : "尚未允许")
        }
    }

    public struct Request: Sendable {
        public let executable: String
        public let executorDirectory: String
        public let executorDigest: String
        public let clientDigest: String
        public let methods: [String]
        public let hostMethods: [String]
        public let clientCapabilities: [String]
        public let scopedRoots: [ScopedRoot]
        public let processPrograms: [ProcessProgram]
        public let processEnvironmentNames: [String]
        public let execution: Execution?
        public let attachment: Attachment?
        public let command: String
        public let arguments: [String]

        public init(executable: String, executorDirectory: String, executorDigest: String,
                    clientDigest: String, methods: [String], hostMethods: [String] = [],
                    clientCapabilities: [String] = [],
                    scopedRoots: [ScopedRoot] = [],
                    processPrograms: [ProcessProgram] = [],
                    processEnvironmentNames: [String] = [],
                    execution: Execution? = nil,
                    attachment: Attachment? = nil,
                    command: String, arguments: [String] = []) {
            self.executable = executable; self.executorDirectory = executorDirectory
            self.executorDigest = executorDigest; self.clientDigest = clientDigest
            self.methods = methods; self.hostMethods = hostMethods
            self.clientCapabilities = clientCapabilities
            self.scopedRoots = scopedRoots
            self.processPrograms = processPrograms
            self.processEnvironmentNames = processEnvironmentNames
            self.execution = execution
            self.attachment = attachment
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
            var scanner = JSONDuplicateKeyScanner(data: data)
            try scanner.validate()
            guard let values = try JSONSerialization.jsonObject(with: data) as? [[String: Any]],
                  values.count <= 16 else { throw invalid("进程映射必须是最多 16 项的 JSON 对象数组") }
            let expected: Set<String> = ["name", "executable", "version", "sha256"]
            let programs = try values.map { value -> ProcessProgram in
                guard Set(value.keys).subtracting(["prefixArguments"]) == expected,
                      let name = value["name"] as? String,
                      let executable = value["executable"] as? String,
                      let version = value["version"] as? String,
                      let sha256 = value["sha256"] as? String else {
                    throw invalid("每个进程映射必须且只能包含 name、executable、version、sha256")
                }
                let prefix = value["prefixArguments"] == nil ? [] : value["prefixArguments"] as? [String]
                guard let prefix else { throw invalid("prefixArguments 必须是字符串数组") }
                return ProcessProgram(name: name, executable: executable, version: version, sha256: sha256,
                                      prefixArguments: prefix)
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
        let argv = try arguments(request, local: false)
        let environment = request.execution.map { "TMPDIR=" + quote($0.temporaryDirectory) + " " } ?? ""
        let command = environment + "exec " + argv.map(quote).joined(separator: " ")
        guard command.utf8.count <= 1024 else { throw invalid("远端启动命令超过 SSH 插件路由上限") }
        return command
    }

    /// Exact argv for Native process transport; no shell rendering or SSH
    /// command-length limit. Core's explicit process argv budget still applies.
    public static func buildLocal(_ request: Request) throws -> [String] {
        let argv = try arguments(request, local: true)
        guard request.executable.hasPrefix("/"), argv.count <= 64,
              argv.reduce(0, { $0 + $1.utf8.count + 1 }) <= 16384 else {
            throw invalid("本机 gterm 需要绝对路径，启动参数最多 64 项 / 16 KiB")
        }
        return argv
    }

    private static func arguments(_ request: Request, local: Bool) throws -> [String] {
        guard request.clientCapabilities.count <= 2, Set(request.clientCapabilities).count == request.clientCapabilities.count,
              request.clientCapabilities.allSatisfy({ ["ui.table", "ui.diff", "ui.task"].contains($0) }) else {
            throw invalid("此 Host 仅支持显式声明的标准 Client 服务")
        }
        guard !request.executable.isEmpty, (request.attachment != nil || !request.executorDirectory.isEmpty),
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
        try validateEnvironmentNames(request.processEnvironmentNames)
        guard request.processEnvironmentNames.isEmpty || !request.processPrograms.isEmpty || request.execution != nil else {
            throw invalid("显式进程环境需要配置进程映射或 Node / Exec 运行档")
        }
        if let attachment = request.attachment {
            guard attachment.authorized, attachment.instance.utf8.count == 32,
                  attachment.instance.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }),
                  canonicalDirectory(attachment.directory), request.execution == nil,
                  request.executorDirectory.isEmpty, request.scopedRoots.isEmpty,
                  request.processPrograms.isEmpty, request.processEnvironmentNames.isEmpty else {
                throw invalid("已有前台实例需要明确允许、规范绝对目录与 32 位小写实例标识；不能同时重配运行档、包目录、根或进程")
            }
            let argv = [request.executable, "relay", "--attach", attachment.directory, "--instance", attachment.instance, "--", request.command] + request.arguments
            guard argv.allSatisfy({ !$0.utf8.contains(0) && (local || (!$0.contains("\n") && !$0.contains("\r"))) && $0.utf8.count <= 4096 }) else { throw invalid("连接参数包含不支持的字符或过长") }
            return argv
        }
        // Keep each expression small for the Swift 6.1 compiler used by CI.
        var argv: [String] = [request.executable, "relay", "--stdio", "--trust-development-digest",
                              request.executorDigest, "--client-digest", request.clientDigest]
        if let execution = request.execution {
            guard execution.authorized else { throw invalid("Node / Exec 需要明确允许本次进程执行") }
            guard absolutePath(execution.temporaryDirectory) else { throw invalid("请指定远端已存在的绝对临时目录") }
            switch execution.runtime {
            case .exec:
                argv.append(contentsOf: ["--runtime", "exec", "--allow-process-execution"])
            case .node(let node):
                guard absolutePath(node.executable), absolutePath(node.sdkDirectory),
                      !node.version.isEmpty, node.version.utf8.count <= 128,
                      node.version.utf8.first.map({ (48...57).contains($0) }) == true,
                      node.version.utf8.allSatisfy({
                          (48...57).contains($0) || (65...90).contains($0) ||
                          (97...122).contains($0) || $0 == 46 || $0 == 45 || $0 == 43
                      }), isDigest(node.sha256), isDigest(node.sdkCatalogSHA256) else {
                    throw invalid("Node 需要绝对路径、无 v 前缀的版本、可执行摘要和 Host SDK catalog 摘要")
                }
                argv.append(contentsOf: ["--runtime", "node", "--allow-process-execution",
                    "--node-runtime", node.executable, node.version, node.sha256,
                    "--node-sdk", node.sdkDirectory, node.sdkCatalogSHA256])
            }
        }
        for method in request.methods {
            argv.append(contentsOf: ["--method", method])
        }
        for capability in request.clientCapabilities {
            argv.append(contentsOf: ["--client-capability", capability])
        }
        for method in request.hostMethods {
            argv.append(contentsOf: ["--host-method", method])
        }
        for root in request.scopedRoots {
            argv.append(contentsOf: ["--root", root.id, root.directory])
        }
        for program in request.processPrograms {
            argv.append(contentsOf: ["--process-program", program.name, program.executable,
                                      program.version, program.sha256])
            for argument in program.prefixArguments {
                argv.append(contentsOf: ["--process-argument", program.name, argument])
            }
        }
        for name in request.processEnvironmentNames {
            argv.append(contentsOf: ["--process-environment", name])
        }
        argv.append(contentsOf: ["--", request.executorDirectory, request.command])
        argv.append(contentsOf: request.arguments)
        guard argv.allSatisfy({
            !$0.utf8.contains(0) && (local || (!$0.contains("\n") && !$0.contains("\r"))) && $0.utf8.count <= 4096
        }) else { throw invalid("远端启动参数包含不支持的字符或过长") }
        return argv
    }

    private static func canonicalDirectory(_ value: String) -> Bool {
        guard absolutePath(value), !value.hasSuffix("/") else { return false }
        return value.dropFirst().split(separator: "/", omittingEmptySubsequences: false).allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." }
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

    private static func absolutePath(_ value: String) -> Bool {
        value.hasPrefix("/") && value.utf8.count <= 4096 &&
            !value.utf8.contains(0) && !value.contains("\n") && !value.contains("\r")
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
        let prefixValid = program.prefixArguments.count <= 16 && program.prefixArguments.allSatisfy {
            $0.utf8.count <= 4096 && !$0.utf8.contains(0) && !$0.contains("\n") && !$0.contains("\r")
        } && program.prefixArguments.reduce(0, { $0 + $1.utf8.count + 1 }) <= 16384
        return prefixValid && validName && program.executable.hasPrefix("/") &&
            program.executable.utf8.count <= 4096 && !program.executable.contains("\n") &&
            !program.executable.contains("\r") && !program.executable.utf8.contains(0) &&
            !program.version.isEmpty && program.version.utf8.count <= 128 &&
            !program.version.contains("\n") && !program.version.contains("\r") &&
            !program.version.utf8.contains(0) && isDigest(program.sha256)
    }

    public static func parseProcessEnvironmentJSON(_ source: String) throws -> [String] {
        let names = try parseStringArray(source, limit: 27, label: "执行端环境变量名称")
        try validateEnvironmentNames(names)
        return names
    }

    private static func validateEnvironmentNames(_ names: [String]) throws {
        let defaults: Set<String> = ["HOME", "PATH", "XDG_CONFIG_HOME", "CODEX_HOME", "TMPDIR"]
        guard names.count <= 27, Set(names).count == names.count,
              names.allSatisfy({ name in
                  let bytes = Array(name.utf8)
                  guard let first = bytes.first, bytes.count <= 128, !defaults.contains(name),
                        (65...90).contains(first) || (97...122).contains(first) || first == 95 else { return false }
                  return bytes.dropFirst().allSatisfy {
                      (65...90).contains($0) || (97...122).contains($0) || (48...57).contains($0) || $0 == 95
                  }
              }) else { throw invalid("执行端环境变量名称非法、重复或与默认候选重复") }
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

    /// JSONSerialization keeps only one value when an object repeats a key.
    /// Scan the original bytes first so the macOS form matches Android's strict
    /// JsonReader behavior instead of silently accepting an ambiguous mapping.
    static func validateJSON(_ data: Data) throws {
        var scanner = JSONDuplicateKeyScanner(data: data)
        try scanner.validate()
        _ = try JSONSerialization.jsonObject(with: data)
    }

    private struct JSONDuplicateKeyScanner {
        private let bytes: [UInt8]
        private var index = 0

        init(data: Data) { bytes = Array(data) }

        mutating func validate() throws {
            try value(depth: 0)
            whitespace()
            guard index == bytes.count else { throw invalid("JSON 后存在额外内容") }
        }

        private mutating func value(depth: Int) throws {
            guard depth <= 64 else { throw invalid("JSON 嵌套过深") }
            whitespace()
            guard index < bytes.count else { throw invalid("JSON 值缺失") }
            switch bytes[index] {
            case 0x7b: try object(depth: depth + 1)
            case 0x5b: try array(depth: depth + 1)
            case 0x22: _ = try string()
            default:
                let start = index
                while index < bytes.count,
                      ![0x2c, 0x5d, 0x7d, 0x20, 0x09, 0x0a, 0x0d].contains(bytes[index]) {
                    index += 1
                }
                guard index > start else { throw invalid("JSON 值无效") }
            }
        }

        private mutating func object(depth: Int) throws {
            index += 1
            whitespace()
            if consume(0x7d) { return }
            var keys = Set<Data>()
            while true {
                whitespace()
                guard index < bytes.count, bytes[index] == 0x22 else {
                    throw invalid("JSON 对象键无效")
                }
                let key = try string()
                guard keys.insert(Data(key.utf8)).inserted else { throw invalid("JSON 对象包含重复键") }
                whitespace()
                guard consume(0x3a) else { throw invalid("JSON 对象缺少冒号") }
                try value(depth: depth)
                whitespace()
                if consume(0x7d) { return }
                guard consume(0x2c) else { throw invalid("JSON 对象分隔符无效") }
            }
        }

        private mutating func array(depth: Int) throws {
            index += 1
            whitespace()
            if consume(0x5d) { return }
            while true {
                try value(depth: depth)
                whitespace()
                if consume(0x5d) { return }
                guard consume(0x2c) else { throw invalid("JSON 数组分隔符无效") }
            }
        }

        private mutating func string() throws -> String {
            let start = index
            index += 1
            while index < bytes.count {
                if bytes[index] == 0x22 {
                    index += 1
                    let encoded = Data(bytes[start..<index])
                    guard let result = try? JSONDecoder().decode(String.self, from: encoded) else {
                        throw invalid("JSON 字符串无效")
                    }
                    return result
                }
                if bytes[index] == 0x5c {
                    index += 1
                    guard index < bytes.count else { break }
                }
                index += 1
            }
            throw invalid("JSON 字符串未结束")
        }

        private mutating func whitespace() {
            while index < bytes.count, [0x20, 0x09, 0x0a, 0x0d].contains(bytes[index]) { index += 1 }
        }

        private mutating func consume(_ byte: UInt8) -> Bool {
            guard index < bytes.count, bytes[index] == byte else { return false }
            index += 1
            return true
        }
    }
}
