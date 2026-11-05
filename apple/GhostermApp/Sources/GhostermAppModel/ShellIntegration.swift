import Foundation
import Darwin
import GhostermCore

/// Startup-only integration. The shell owns prompt detection; no transport
/// polling, output inspection, or commands injected into a running session.
enum ShellIntegration {
    struct Launch {
        let executable: String?
        let arguments: [String]
        let environment: [String]
    }

    static func launch(executable: String?, arguments: [String], environment: [String],
                       kind: Transport.PTYCommandKind) throws -> Launch {
        let unchanged = Launch(executable: executable, arguments: arguments, environment: environment)
        guard kind == .shell, arguments.isEmpty || arguments == ["-i"] else { return unchanged }
        var selected = executable ?? ""
        if selected.isEmpty { selected = ProcessInfo.processInfo.environment["SHELL"] ?? "" }
        if selected.isEmpty, let account = getpwuid(getuid()), let shell = account.pointee.pw_shell {
            selected = String(cString: shell)
        }
        if selected.isEmpty { selected = "/bin/sh" }
        let name = URL(fileURLWithPath: selected).lastPathComponent
        guard name == "zsh" else { return unchanged }
        var values = ProcessInfo.processInfo.environment
        for entry in environment {
            guard let equal = entry.firstIndex(of: "=") else { continue }
            values[String(entry[..<equal])] = String(entry[entry.index(after: equal)...])
        }
        let resources: Bundle
        if Bundle.main.bundleURL.pathExtension == "app" {
            // SwiftPM's generated accessor can fall back to the developer's
            // build directory. An installed app must use only its own bundle.
            guard let directory = Bundle.main.resourceURL,
                  let packaged = Bundle(url: directory.appendingPathComponent("GhostermApp_GhostermAppModel.bundle")) else {
                throw GhostermError.failed("缺少随包 shell 鼠标恢复资源")
            }
            resources = packaged
        } else {
            resources = Bundle.module
        }
        guard let root = resources.url(forResource: "ShellIntegration", withExtension: nil) else {
            throw GhostermError.failed("缺少随包 shell 鼠标恢复资源")
        }
        let required = ["zsh/.zshenv", "mouse-recovery.zsh"]
        for relative in required {
            let path = root.appendingPathComponent(relative).path
            var directory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: path, isDirectory: &directory), !directory.boolValue,
                  FileManager.default.isReadableFile(atPath: path) else {
                throw GhostermError.failed("缺少随包 shell 鼠标恢复资源：\(relative)")
            }
        }
        var overrides = environment
        func put(_ key: String, _ value: String) {
            overrides.removeAll { $0.hasPrefix(key + "=") }
            overrides.append(key + "=" + value)
        }
        put("GHOSTERM_ORIGINAL_ZDOTDIR_SET", values["ZDOTDIR"] == nil ? "0" : "1")
        put("GHOSTERM_ORIGINAL_ZDOTDIR", values["ZDOTDIR"] ?? "")
        put("ZDOTDIR", root.appendingPathComponent("zsh").path)
        return Launch(executable: selected, arguments: arguments, environment: overrides)
    }
}
