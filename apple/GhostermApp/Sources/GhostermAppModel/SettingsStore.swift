// SettingsStore — 应用设置（设置页的数据面）。
//
// 两件事分开存，各有各的真相来源：
//   1. **终端配置**（字体 / 主题 / 光标…）→ 落成一份 **Ghostty 语法**的
//      config 文件，交给 libghostty 自己解析。**不在这里实现一份配置解析**：
//      Ghostty 的配置键有一百多个且随版本演进，我们维护一份子集必然漂移。
//      （铁律 #1/#8 的同一种精神：真相在有闸门的那一侧。）
//   2. **应用偏好**（启动时是否新建会话、默认 shell…）→ UserDefaults。
//
// 写入纪律（沿用 ConfigStore 的写侧三件套，铁律 #4 的精神）：
//   先备份 → 冲突检测 → 原子写（同目录临时文件 + rename）。
// 用户手改过这个文件就得被尊重，不能被设置页静默覆盖。
import Foundation

public enum SettingsStoreError: Error, CustomStringConvertible {
    case conflict(String)
    case invalid(String)
    case io(String)

    public var description: String {
        switch self {
        case .conflict(let d):
            return "终端配置文件已被外部修改（\(d)）——请重新加载后再保存"
        case .invalid(let d):
            return "配置无效：\(d)"
        case .io(let d):
            return "写入失败：\(d)"
        }
    }
}

/// 终端配置里我们**暴露成控件**的一组键（其余键仍生效，只是不进 UI）。
/// 这是 UI 的展示清单，不是「支持的键」的白名单——写回时文件里的
/// 其他行原样保留。
public struct TerminalSettingKey {
    public let key: String
    public let label: String
    public let hint: String
    public let kind: Kind

    public enum Kind {
        case number(range: ClosedRange<Double>, step: Double)
        case text
        case toggle
        case choice([String])
    }
}

public final class SettingsStore {
    /// 一个键在 Ghostty 配置里的 C 类型（决定用哪个强类型入口去读它）。
    /// libghostty 的 `ghostty_config_get` 按键的**真实类型**写 out，类型不符
    /// 只会得到 0/false——所以这张表是回显正确性的前提。
    public enum CValueKind { case float, double, bool, text }

    /// 我们暴露的键各自的 C 类型（来自 vendor/ghostty 的 Config.zig）。
    public static func cValueKind(of key: String) -> CValueKind {
        switch key.lowercased() {
        case "font-size", "font-size-adjust", "line-height": return .float
        case "background-opacity", "background-image-opacity": return .double
        case "cursor-style-blink", "mouse-hide-while-typing",
             "copy-on-select", "confirm-close-surface": return .bool
        default: return .text
        }
    }

    public static let terminalSettingKeys: [TerminalSettingKey] = [
        .init(key: "font-size", label: "字号", hint: "终端字体大小（磅）",
              kind: .number(range: 6...48, step: 0.5)),
        .init(key: "font-family", label: "字体", hint: "留空用终端默认字体",
              kind: .text),
        .init(key: "background-opacity", label: "背景不透明度",
              hint: "0 全透明，1 不透明",
              kind: .number(range: 0...1, step: 0.05)),
        .init(key: "cursor-style", label: "光标形状", hint: "终端光标样式",
              kind: .choice(["block", "bar", "underline"])),
        .init(key: "cursor-style-blink", label: "光标闪烁", hint: "",
              kind: .toggle),
        .init(key: "mouse-hide-while-typing", label: "输入时隐藏鼠标",
              hint: "", kind: .toggle),
        .init(key: "copy-on-select", label: "选中即复制", hint: "", kind: .toggle),
        .init(key: "confirm-close-surface", label: "关闭前确认",
              hint: "有前台进程时关闭窗口先确认", kind: .toggle),
    ]

    public let terminalConfigPath: String
    /// Ghostty 资源目录（`<...>/Resources/ghostty`）。主题等资源在这里；
    /// nil = 当前进程没有资源目录（开发态直接跑可执行文件时会出现）。
    public let resourcesPath: String?
    private var transaction: ConfigStore
    private var content: Result<String, Error>
    private let operations: ConfigStore.FileOperations

    /// Text for presentation only. Failed reads remain a separate error and
    /// cannot be saved until an explicit reload succeeds.
    public var loadedText: String? { try? content.get() }
    public var loadError: String? {
        if case .failure(let error) = content { return String(describing: error) }
        return nil
    }

    public convenience init(terminalConfigPath: String, resourcesPath: String? = nil) {
        self.init(terminalConfigPath: terminalConfigPath, resourcesPath: resourcesPath, operations: ConfigStore.FileOperations())
    }

    init(terminalConfigPath: String, resourcesPath: String? = nil, operations: ConfigStore.FileOperations) {
        self.terminalConfigPath = terminalConfigPath
        self.resourcesPath = resourcesPath ?? SettingsStore.detectResourcesPath()
        self.operations = operations
        let transaction = ConfigStore(path: terminalConfigPath, operations: operations, backupName: URL(fileURLWithPath: terminalConfigPath).lastPathComponent + ".bak")
        self.transaction = transaction
        content = Result { try Self.text(in: transaction) }
    }

    private static func text(in transaction: ConfigStore) throws -> String {
        guard let bytes = try transaction.loadedBytes() else { return "" }
        guard let text = String(data: bytes, encoding: .utf8), !bytes.contains(0) else {
            throw SettingsStoreError.invalid("终端配置必须是无 NUL 的 UTF-8 文本")
        }
        return text
    }

    /// Reload is explicit: a previous failed read must never become permission
    /// to overwrite a subsequently replaced file with an empty editor buffer.
    @discardableResult public func reload() throws -> String {
        transaction = ConfigStore(path: terminalConfigPath, operations: operations, backupName: URL(fileURLWithPath: terminalConfigPath).lastPathComponent + ".bak")
        content = Result { try Self.text(in: transaction) }
        return try rawConfig()
    }

    /// 推导资源目录：与 CGhostty shim 同一套逻辑（沿可执行文件向上找
    /// `Contents/Resources/ghostty`）。两处都实现是**故意**的取舍：
    /// 主题列表要给 UI 看（Swift 侧），而 libghostty 认的是环境变量
    /// （C 侧）——用一个共享的 C 入口导出路径会更绕，且这条推导很短；
    /// 两边不一致时**以 C 侧为准**（那样主题能真的被引擎加载，
    /// 只是列表可能为空，属于可诊断的降级）。
    public static func detectResourcesPath() -> String? {
        var size: UInt32 = 4096
        var buf = [CChar](repeating: 0, count: Int(size))
        guard _NSGetExecutablePath(&buf, &size) == 0 else { return nil }
        guard let real = realpath(buf, nil) else { return nil }
        defer { free(real) }
        var url = URL(fileURLWithPath: String(cString: real))
        url.deleteLastPathComponent()   // .../Contents/MacOS
        url.deleteLastPathComponent()   // .../Contents
        let res = url.appendingPathComponent("Resources/ghostty").path
        return FileManager.default.fileExists(atPath: res) ? res : nil
    }

    /// 可用的主题名（Ghostty 配色集，按名字排序）。
    /// 资源目录缺失时**显式返回空表**，UI 据此显示「未随包提供」而不是
    /// 一个看起来像坏了的空下拉。
    public func availableThemes() -> [String] {
        guard let dir = themesDirectory else { return [] }
        let names = (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []
        return names.filter { !$0.hasPrefix(".") }.sorted()
    }

    /// 配色集目录（`<资源目录>/themes`）。nil = 没有资源目录
    /// （开发态直接跑可执行文件时的正常情况）。
    public var themesDirectory: String? {
        guard let res = resourcesPath else { return nil }
        return (res as NSString).appendingPathComponent("themes")
    }

    /// 读一个配色文件的原文（色块预览用）。找不到返回 nil（**不猜**：
    /// UI 据此画一个空色块，而不是拿默认色假装是那套配色）。
    public func themeFileText(named name: String) -> String? {
        guard let dir = themesDirectory else { return nil }
        let path = (dir as NSString).appendingPathComponent(name)
        return try? String(contentsOfFile: path, encoding: .utf8)
    }

    /// 默认位置：`~/Library/Application Support/Ghosterm/ghostty.conf`。
    public static func defaultTerminalConfigPath() -> String {
        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory())
                .appendingPathComponent("Library/Application Support")
        return base.appendingPathComponent("Ghosterm/ghostty.conf").path
    }

    public func isExternallyModified() -> Bool { transaction.isExternallyModified() }

    /// The exact loaded snapshot. Missing is empty; failed reads throw.
    public func rawConfig() throws -> String { try content.get() }

    /// Mandatory backup, exact byte/identity conflict checks, stable writer lock
    /// and same-directory atomic replacement are shared with SSH configuration.
    public func saveTerminalConfig(_ text: String) throws {
        _ = try rawConfig()
        guard !text.utf8.contains(0) else { throw SettingsStoreError.invalid("配置不能包含 NUL") }
        let directory = URL(fileURLWithPath: terminalConfigPath).deletingLastPathComponent()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                               attributes: [.posixPermissions: 0o700])
        do {
            try transaction.save(text)
            content = .success(try Self.text(in: transaction))
        } catch {
            if case ConfigStoreError.committed = error { content = .failure(error) }
            throw error
        }
    }

    /// 把一组「键 = 值」合并进现有配置文本（**逐行保真**：未知键、注释、
    /// 缩进、顺序都不动；只替换/追加目标键的那一行）。
    ///
    /// 键不存在 → 追加到末尾；值传 nil → 删除该行。这是唯一改配置文本的
    /// 入口，UI 不自己拼字符串。
    public static func merging(_ text: String, edits: [(String, String?)]) -> String {
        var lines = text.components(separatedBy: "\n")
        let trailing = text.isEmpty || text.utf8.last == 10
        if lines.last == "" { lines.removeLast() }
        for (key, value) in edits {
            let matching = lines.indices.filter { index in
                let line = lines[index].trimmingCharacters(in: .whitespacesAndNewlines)
                guard !line.hasPrefix("#"), let equal = line.firstIndex(of: "=") else { return false }
                return line[..<equal].trimmingCharacters(in: .whitespaces).lowercased() == key.lowercased()
            }
            if let value {
                if let last = matching.last {
                    let ending = lines[last].hasSuffix("\r") ? "\r" : ""
                    lines[last] = "\(key) = \(value)" + ending
                } else {
                    let ending = text.contains("\r\n") ? "\r" : ""
                    lines.append("\(key) = \(value)" + ending)
                }
            } else {
                let removed = Set(matching)
                lines = lines.enumerated().filter { !removed.contains($0.offset) }.map(\.element)
            }
        }
        guard !lines.isEmpty else { return "" }
        return lines.joined(separator: "\n") + (trailing ? "\n" : "")
    }

    /// 从配置文本里读一个键的值（最后一次出现的胜出——与 libghostty 的
    /// 覆盖语义一致）。
    public static func value(of key: String, in text: String) -> String? {
        let lowerKey = key.lowercased()
        var result: String?
        for line in text.components(separatedBy: "\n") {
            let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
            if trimmed.isEmpty || trimmed.hasPrefix("#") { continue }
            let parts = trimmed.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            guard parts.count == 2 else { continue }
            if parts[0].trimmingCharacters(in: .whitespaces).lowercased() == lowerKey {
                result = parts[1].trimmingCharacters(in: .whitespaces)
            }
        }
        return result
    }
}

// MARK: - 应用偏好（UserDefaults）

/// 启动行为 / 默认 shell 这类偏好的读写口。与终端配置分开：这些**不进**
/// Ghostty 配置（libghostty 不认识它们），由宿主自己实现。
public struct AppPreferences {
    public enum Key: String {
        case shellOnLaunch = "Ghosterm.shellOnLaunch"
        case defaultShell = "Ghosterm.defaultShell"
        case rememberDirectories = "Ghosterm.rememberDirectories"
        case lockTabsOnLaunch = "Ghosterm.lockTabsOnLaunch"
        case tabPlacement = "Ghosterm.tabPlacement"
    }

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) { self.defaults = defaults }

    public var tabPlacement: TabPlacement {
        get { defaults.string(forKey: Key.tabPlacement.rawValue).flatMap(TabPlacement.init(rawValue:)) ?? .sidebar }
        set { defaults.set(newValue.rawValue, forKey: Key.tabPlacement.rawValue) }
    }

    /// 启动时是否新建一个本地 shell（关掉就是空窗口等用户操作，
    /// 对应 iGhostVT 的「启动时新建会话」）。
    public var shellOnLaunch: Bool {
        get { bool(Key.shellOnLaunch.rawValue, default: true) }
        set { defaults.set(newValue, forKey: Key.shellOnLaunch.rawValue) }
    }

    /// 默认 shell 可执行文件（空 = 系统默认 / $SHELL）。
    public var defaultShell: String {
        get { defaults.string(forKey: Key.defaultShell.rawValue) ?? "" }
        set { defaults.set(newValue, forKey: Key.defaultShell.rawValue) }
    }

    /// 记不记「最近用过的目录」（本地 shell 的 cwd 起点列表）。
    public var rememberDirectories: Bool {
        get { bool(Key.rememberDirectories.rawValue, default: true) }
        set { defaults.set(newValue, forKey: Key.rememberDirectories.rawValue) }
    }

    /// 启动时把恢复出来的标签页锁上（防止刚开就误触）。
    public var lockTabsOnLaunch: Bool {
        get { bool(Key.lockTabsOnLaunch.rawValue, default: false) }
        set { defaults.set(newValue, forKey: Key.lockTabsOnLaunch.rawValue) }
    }

    /// 有显式设置则是它；没有就用 `$SHELL`；再没有就 nil（由 core 的
    /// forkpty 走默认）。
    public var resolvedShell: String? {
        if !defaultShell.isEmpty { return defaultShell }
        let env = ProcessInfo.processInfo.environment["SHELL"]
        return (env?.isEmpty == false) ? env : nil
    }

    private func bool(_ key: String, default fallback: Bool) -> Bool {
        guard defaults.object(forKey: key) != nil else { return fallback }
        return defaults.bool(forKey: key)
    }
}
