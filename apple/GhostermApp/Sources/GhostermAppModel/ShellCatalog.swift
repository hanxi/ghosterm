// ShellCatalog — 「默认命令行解释器」的选项来源（通用设置页的下拉列表）。
//
// 取法参照 iGhostVT 的 `ShellMenuItems`：**自动**（空值 = 用登录 shell）、
// 系统上发现的每个 shell、以及**自定义…**（手填可执行文件路径）。
//
// 为什么读 `/etc/shells` 而不是自己猜几个路径：那份文件就是系统对
// 「哪些是合法登录 shell」的登记（`chsh` 也认它），我们照着列不会漏掉
// 用户自己装的 zsh/bash。
import Foundation

public enum ShellCatalog {
    /// 系统登记的 shell（`/etc/shells`），按路径去重排序。
    /// 文件缺失/读不到时**显式返回空表**——UI 据此只给「自动」与「自定义…」，
    /// 不是一个看起来坏了的空下拉。
    public static func availableShells(
        etcShellsPath: String = "/etc/shells"
    ) -> [String] {
        guard let text = try? String(contentsOfFile: etcShellsPath,
                                     encoding: .utf8) else { return [] }
        return parse(text: text)
    }

    /// 解析 `/etc/shells` 文本（独立成纯函数，便于断言）。
    /// 规则：`#` 起头与空行是注释；同一路径只列一次；排序稳定。
    public static func parse(text: String) -> [String] {
        var seen = Set<String>()
        var out: [String] = []
        for raw in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            guard line.hasPrefix("/") else { continue }   // 只认绝对路径
            if seen.insert(line).inserted { out.append(line) }
        }
        return out.sorted()
    }

    /// 下拉按钮上显示的文字：正在编辑自定义路径 → 「自定义」；空 → 「自动」；
    /// 否则是那台 shell 的路径。
    public static func title(of shellPath: String, isCustom: Bool = false) -> String {
        if isCustom { return "自定义" }
        return shellPath.isEmpty ? "自动" : shellPath
    }

    /// 自定义路径输入框什么时候显示：正在编辑，或**存着一个列表里没有的路径**
    /// （用户手填过，或被升级换走了）。`available == nil` 表示列表还没到，
    /// 这时不显示——否则会因为「还没读到」而误报成自定义。
    public static func showsCustomPath(_ shellPath: String, available: [String]?,
                                       isEditing: Bool) -> Bool {
        if isEditing { return true }
        guard let available, !shellPath.isEmpty else { return false }
        return !available.contains(shellPath)
    }
}