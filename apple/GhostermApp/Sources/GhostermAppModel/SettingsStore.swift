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
    private var loadedStat: (mtime: TimeInterval, size: Int)?

    public init(terminalConfigPath: String, resourcesPath: String? = nil) {
        self.terminalConfigPath = terminalConfigPath
        self.resourcesPath = resourcesPath
            ?? SettingsStore.detectResourcesPath()
        loadedStat = SettingsStore.statOf(terminalConfigPath)
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

    private static func statOf(_ path: String) -> (mtime: TimeInterval, size: Int)? {
        guard let a = try? FileManager.default.attributesOfItem(atPath: path),
              let m = a[.modificationDate] as? Date,
              let s = a[.size] as? Int else { return nil }
        return (m.timeIntervalSince1970, s)
    }

    /// 文件自加载后是否被外部改过。文件**不存在**时不算冲突（首次保存）。
    public func isExternallyModified() -> Bool {
        guard let loaded = loadedStat else { return false }
        guard let now = SettingsStore.statOf(terminalConfigPath) else { return true }
        return now.mtime != loaded.mtime || now.size != loaded.size
    }

    /// 当前配置原文（文件不存在 = 空）。
    public func rawConfig() -> String {
        (try? String(contentsOfFile: terminalConfigPath, encoding: .utf8)) ?? ""
    }

    /// 保存配置：备份 → 冲突检测 → 原子写。
    ///
    /// `text` 是**整份**终端配置文本（UI 改一行也要给全文——保持
    /// 「文件即真相」，不做增量补丁：用户手写的注释与未知键必须原样留着）。
    public func saveTerminalConfig(_ text: String) throws {
        if isExternallyModified() {
            throw SettingsStoreError.conflict("mtime/size 与加载时不一致")
        }
        let fm = FileManager.default
        let dir = (terminalConfigPath as NSString).deletingLastPathComponent
        try? fm.createDirectory(atPath: dir, withIntermediateDirectories: true)

        // 1) 备份（仅在原文件存在时；首次保存没什么可备份的）。
        if fm.fileExists(atPath: terminalConfigPath) {
            let backup = terminalConfigPath + ".bak"
            try? fm.removeItem(atPath: backup)
            do { try fm.copyItem(atPath: terminalConfigPath, toPath: backup) }
            catch { throw SettingsStoreError.io("备份失败：\(error)") }
        }

        // 2) 原子写：同目录临时文件 + rename（半份配置永不落盘）。
        let tmp = dir + "/.ghostty.conf.\(UUID().uuidString).tmp"
        do {
            try text.write(toFile: tmp, atomically: false, encoding: .utf8)
            _ = try fm.replaceItemAt(URL(fileURLWithPath: terminalConfigPath),
                                     withItemAt: URL(fileURLWithPath: tmp))
        } catch {
            try? fm.removeItem(atPath: tmp)
            throw SettingsStoreError.io("\(error)")
        }
        loadedStat = SettingsStore.statOf(terminalConfigPath)
    }

    /// 把一组「键 = 值」合并进现有配置文本（**逐行保真**：未知键、注释、
    /// 缩进、顺序都不动；只替换/追加目标键的那一行）。
    ///
    /// 键不存在 → 追加到末尾；值传 nil → 删除该行。这是唯一改配置文本的
    /// 入口，UI 不自己拼字符串。
    public static func merging(_ text: String, edits: [(String, String?)]) -> String {
        // 保留原样的换行结构：只摘掉**末尾**那一个空串（它是「以 \n 结尾」
        // 的痕迹，不是一行内容）。中途的空行是用户排版，必须留着。
        var lines = text.split(separator: "\n", omittingEmptySubsequences: false)
            .map(String.init)
        var hadTrailingNewline = false
        if lines.last == "" {
            hadTrailingNewline = true
            lines.removeLast()
        }

        for (key, value) in edits {
            let lowerKey = key.lowercased()
            var found = false
            var out: [String] = []
            for line in lines {
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                // 注释行不动（哪怕是注释掉的同名键——那是用户的笔记）。
                if trimmed.hasPrefix("#") || trimmed.isEmpty {
                    out.append(line); continue
                }
                let parts = trimmed.split(separator: "=", maxSplits: 1)
                let name = parts[0].trimmingCharacters(in: .whitespaces).lowercased()
                if name == lowerKey && !found {
                    found = true
                    if let v = value { out.append("\(key) = \(v)") }
                    // value == nil 且 found：这一行被丢掉（删除语义）。
                    continue
                }
                out.append(line)
            }
            lines = out
            if !found, let v = value { lines.append("\(key) = \(v)") }
        }

        guard !lines.isEmpty else { return "" }
        return lines.joined(separator: "\n") + (hadTrailingNewline ? "\n" : "")
    }

    /// 从配置文本里读一个键的值（最后一次出现的胜出——与 libghostty 的
    /// 覆盖语义一致）。
    public static func value(of key: String, in text: String) -> String? {
        let lowerKey = key.lowercased()
        var result: String?
        for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty || trimmed.hasPrefix("#") { continue }
            let parts = trimmed.split(separator: "=", maxSplits: 1)
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
        case compactSidebar = "Ghosterm.compactSidebar"
    }

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) { self.defaults = defaults }

    public var tabPlacement: TabPlacement {
        get { defaults.string(forKey: Key.tabPlacement.rawValue).flatMap(TabPlacement.init(rawValue:)) ?? .sidebar }
        set { defaults.set(newValue.rawValue, forKey: Key.tabPlacement.rawValue) }
    }

    /// 顶部布局不使用此偏好，但切回侧边时保留用户的选择。
    public var compactSidebar: Bool {
        get { bool(Key.compactSidebar.rawValue, default: false) }
        set { defaults.set(newValue, forKey: Key.compactSidebar.rawValue) }
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
