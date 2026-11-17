import Foundation
import GhostermCore

/// Editable values are decoded by Core. Untouched fields never generate patches.
public struct HostEditorDraft {
    public struct Field {
        public let keyword: String
        public var enabled: Bool
        public var value: String
        public let source: String
        private let originalEnabled: Bool
        private let originalValue: Data
        public var changed: Bool { enabled != originalEnabled || (enabled && Data(value.utf8) != originalValue) }
        init(_ keyword: String, value: String, enabled: Bool, source: String) {
            self.keyword = keyword; self.value = value; self.enabled = enabled; self.source = source
            originalEnabled = enabled; originalValue = Data(value.utf8)
        }
    }
    public struct Extra: Identifiable {
        public var id: String { field.keyword }
        public var field: Field
        public let perLine: Bool
        public let readOnlyReason: String?
    }
    public let originalConfiguration: Data?
    public var alias: String
    public var hostname: Field
    public var user: Field
    public var port: Field
    public var identities: Field
    public var extras: [Extra]
    public let notice: String?
    public let readOnlyReason: String?

    public init(model: HostListModel?, alias: String, isNew: Bool) throws {
        originalConfiguration = model.map { Data($0.rawConfig.utf8) }
        self.alias = isNew ? "" : alias
        let rows: [ConfigDirectiveInfo]
        let info: ConfigHostInfo?
        let resolved: ResolvedHost?
        if isNew { rows = []; info = nil; resolved = nil }
        else {
            guard let model else { throw GhostermError.failed("配置未加载") }
            info = try model.inspect(alias: alias)
            rows = try model.directiveSnapshot(of: alias)
            resolved = try model.resolved(alias: alias)
        }
        readOnlyReason = info?.included == true ? "此块来自 Include 文件，请在来源文件中编辑：\(info!.sourcePath):\(info!.sourceLine)" : nil
        notice = (info?.patternCount ?? 0) > 1 ? "此块包含多个别名。字段修改也会影响同块的其他别名；改名只替换当前别名。" : nil
        func field(_ key: String, _ origin: ConfigFieldInfo?, effective: String) -> Field {
            let own = rows.first { $0.keyword == key.lowercased() }
            let source: String
            if let origin, origin.sourceLine > 0 {
                source = "当前生效：\(effective) · \(origin.sourcePath.isEmpty ? "当前配置" : origin.sourcePath):\(origin.sourceLine)\(origin.effectiveIsExplicit ? "（本块）" : "（继承）")"
            } else { source = "当前生效：\(effective)（默认值）" }
            return Field(key, value: own?.arguments.first ?? "", enabled: own != nil, source: source)
        }
        hostname = field("HostName", info?.hostname, effective: resolved?.hostname ?? alias)
        user = field("User", info?.user, effective: resolved?.user ?? "默认用户")
        port = field("Port", info?.port, effective: String(resolved?.port ?? 22))
        let ownIdentities = rows.filter { $0.keyword == "identityfile" }
        guard ownIdentities.allSatisfy({ $0.arguments.count == 1 }) else {
            throw GhostermError.failed("IdentityFile 必须每行只有一个引用，请在配置文件中修正")
        }
        let inherited = try (isNew ? [] : model!.identities(alias: alias)).filter { !$0.explicitValue }
        identities = Field("IdentityFile", value: ownIdentities.map { $0.arguments[0] }.joined(separator: "\n"),
            enabled: !ownIdentities.isEmpty,
            source: inherited.isEmpty ? "每行一个引用；逗号和空格属于路径。关闭本块设置后保留继承和默认候选。" :
                "继承引用：\n" + inherited.map { "\($0.value) · \($0.sourcePath.isEmpty ? "当前配置" : $0.sourcePath):\($0.sourceLine)" }.joined(separator: "\n"))
        let reserved: Set<String> = ["hostname", "user", "port", "identityfile"]
        var keys: [String] = []
        for row in rows where !reserved.contains(row.keyword) && !keys.contains(row.keyword) { keys.append(row.keyword) }
        guard keys.count <= 128 else { throw GhostermError.failed("其他指令超过表单预算，请使用配置文件编辑器") }
        extras = keys.map { key in
            let group = rows.filter { $0.keyword == key }
            let perLine = group[0].renderPerLine
            let reason = key == "include" ? "Include 路径由来源文件维护" : (!perLine && group.count > 1 ? "存在重复指令，保留原行；请在配置文件中编辑" : nil)
            return Extra(field: Field(key, value: group.map { $0.arguments.map(Self.quote).joined(separator: " ") }.joined(separator: "\n"), enabled: true,
                source: "当前配置第 \(group.map { String($0.sourceLine) }.joined(separator: "、")) 行"), perLine: perLine, readOnlyReason: reason)
        }
    }

    public func edits() throws -> [(String, [String]?)] {
        guard readOnlyReason == nil else { throw GhostermError.failed(readOnlyReason!) }
        guard !alias.isEmpty, !alias.hasPrefix("!"), !alias.utf8.contains(where: { $0 <= 32 || $0 == 127 }), !alias.contains("\""), !alias.contains("\\"), !alias.contains("#"), alias.utf8.count <= 128 else {
            throw GhostermError.failed("别名不能为空或包含空白、控制字符、引号或否定前缀")
        }
        var result: [(String, [String]?)] = []
        for field in [hostname, user, port] where field.changed {
            if !field.enabled { result.append((field.keyword, nil)); continue }
            try Self.validate(field.value)
            guard !field.value.isEmpty else { throw GhostermError.failed("\(field.keyword) 显式值不能为空；可关闭本块设置以继承") }
            if field.keyword == "Port" {
                guard let number = Int(field.value), (1...65535).contains(number) else { throw GhostermError.failed("Port 必须在 1–65535 之间") }
            }
            result.append((field.keyword, [Self.quote(field.value)]))
        }
        if identities.changed {
            if !identities.enabled { result.append(("IdentityFile", nil)) }
            else {
                let lines = identities.value.components(separatedBy: "\n").filter { !$0.isEmpty }
                guard !lines.isEmpty, lines.count <= 4096 else { throw GhostermError.failed("请逐行填写 1–4096 个身份引用") }
                for line in lines { try Self.validate(line) }
                result.append(("IdentityFile", lines.map(Self.quote)))
            }
        }
        for extra in extras where extra.field.changed {
            guard extra.readOnlyReason == nil else { throw GhostermError.failed(extra.readOnlyReason!) }
            if !extra.field.enabled { result.append((extra.field.keyword, nil)); continue }
            let lines = extra.field.value.components(separatedBy: "\n")
            guard extra.perLine || lines.count == 1 else { throw GhostermError.failed("此指令只接受一行") }
            var values: [String] = []
            for line in lines {
                try Self.validate(line)
                let parsed = try SSHConfig(contents: "Host __draft__\n \(extra.field.keyword) \(line)\n")
                let rows = try parsed.directives(alias: "__draft__")
                guard rows.count == 1, !rows[0].arguments.isEmpty else { throw GhostermError.failed("指令参数不能为空") }
                values.append(rows[0].arguments.map(Self.quote).joined(separator: " "))
            }
            result.append((extra.field.keyword, values))
        }
        return result
    }

    private static func validate(_ text: String) throws {
        guard text.utf8.count <= 65536, !text.utf8.contains(where: { $0 == 0 || $0 == 10 || $0 == 13 }) else {
            throw GhostermError.failed("字段过长或包含 NUL/换行")
        }
    }
    private static func quote(_ text: String) -> String {
        "\"" + text.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
    }
}
