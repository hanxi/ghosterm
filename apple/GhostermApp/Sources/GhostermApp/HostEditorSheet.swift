// HostEditorSheet — ssh_config 的图形编辑器（M3）。
//
// 编辑的是**文件真相**：初值取 Host 块内实际指令（不是 resolve 的求值结果）。
// 保存走稀疏补丁 → 冲突检测 → 备份 → 原子写回（AppModel.applyEdits），
// 任何一步失败都显式提示，不静默丢编辑。
import GhostermAppModel
import SwiftUI

/// 一个 Host 块的编辑器。保存 = 稀疏补丁 + 冲突检测 + 备份 + 原子写回。
/// 常用字段（HostName/User/Port/IdentityFile）给专门控件，其余指令
/// 走通用「关键字 + 值」表格——铁律 #4 的语义全在 core，UI 只做视图。
struct HostEditorSheet: View {
    @ObservedObject var model: AppModel
    @Environment(\.dismiss) private var dismiss

    let alias: String
    let isNew: Bool

    @State private var newAlias = ""
    @State private var hostname = ""
    @State private var user = ""
    @State private var port = ""
    @State private var identityFiles = ""
    @State private var extraDirectives: [(keyword: String, values: String)] = []
    @State private var errorText: String?
    @State private var loaded = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(isNew ? "新建 Host" : "编辑 \(alias)").font(DS.Font.titleLarge)

            if isNew {
                TextField("别名（Host 模式，如 web 或 web-*）", text: $newAlias)
                    .textFieldStyle(.roundedBorder)
            }

            Form {
                TextField("HostName", text: $hostname)
                TextField("User", text: $user)
                TextField("Port", text: $port)
                TextField("IdentityFile（逗号分隔多条）", text: $identityFiles)
            }

            if !extraDirectives.isEmpty {
                Text("其他指令").font(DS.Font.title)
                ForEach(extraDirectives.indices, id: \.self) { i in
                    HStack {
                        Text(extraDirectives[i].keyword)
                            .frame(width: 140, alignment: .leading)
                            .foregroundStyle(.secondary)
                        TextField("值", text: $extraDirectives[i].values)
                            .textFieldStyle(.roundedBorder)
                    }
                }
            }

            if let errorText {
                Text(errorText).foregroundStyle(.red).font(DS.Font.callout)
            }

            HStack {
                Spacer()
                Button("取消") { dismiss() }
                Button("保存") { save() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 460)
        .onAppear(perform: loadInitial)
    }

    /// 初值来自**块内实际指令**（directives），不是 resolve 的求值结果——
    /// 编辑器编辑的是文件真相。
    private func loadInitial() {
        guard !loaded else { return }
        loaded = true
        if !isNew {
            for d in model.directives(of: alias) {
                switch d.keyword.lowercased() {
                case "hostname": hostname = d.values.joined(separator: " ")
                case "user": user = d.values.joined(separator: " ")
                case "port": port = d.values.joined(separator: " ")
                case "identityfile": identityFiles = d.values.joined(separator: ",")
                default:
                    extraDirectives.append((d.keyword, d.values.joined(separator: " ")))
                }
            }
        }
    }

    private func save() {
        var edits: [(String, [String]?)] = []
        func push(_ keyword: String, _ text: String, multi: Bool = false) {
            let trimmed = text.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty {
                edits.append((keyword, nil)) // 空 = 删除该指令
            } else if multi {
                let values = trimmed.split(separator: ",")
                    .map { $0.trimmingCharacters(in: .whitespaces) }
                edits.append((keyword, values))
            } else {
                edits.append((keyword, [trimmed]))
            }
        }
        push("HostName", hostname)
        push("User", user)
        push("Port", port)
        push("IdentityFile", identityFiles, multi: true)
        for d in extraDirectives {
            let trimmed = d.values.trimmingCharacters(in: .whitespaces)
            edits.append((d.keyword, trimmed.isEmpty ? nil : [trimmed]))
        }

        do {
            let target = isNew ? newAlias.trimmingCharacters(in: .whitespaces) : alias
            if target.isEmpty {
                errorText = "别名不能为空"
                return
            }
            if isNew {
                try model.addHost(alias: target, edits)
            } else {
                try model.applyEdits(alias: target, edits)
            }
            dismiss()
        } catch {
            errorText = "\(error)"
        }
    }
}

