import GhostermAppModel
import SwiftUI

struct PluginCommandPalette: View {
    @ObservedObject var model: AppModel
    let workspaceID: UUID?
    let dismiss: () -> Void
    @State private var query = ""
    @State private var starting = false
    @State private var issue: String?
    @FocusState private var searchFocused: Bool

    private var commands: [AppModel.PreparedExtensionCommand] {
        model.preparedExtensionCommands.filter { entry in
            entry.workspaceID == workspaceID && (query.isEmpty || entry.title.localizedCaseInsensitiveContains(query) ||
                entry.launch.request.command.localizedCaseInsensitiveContains(query))
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            TextField("搜索插件命令", text: $query)
                .focused($searchFocused)
                .disabled(starting)
                .accessibilityIdentifier("plugin-command-search")
            if commands.isEmpty {
                Text(query.isEmpty ? "可在设置的插件页添加当前工作区的命令。" : "没有匹配的命令。")
                    .foregroundStyle(.secondary)
            } else {
                List(commands) { entry in
                    HStack {
                        VStack(alignment: .leading) {
                            Text(verbatim: entry.title)
                            Text(verbatim: entry.launch.localLaunch == nil ? entry.launch.sshAlias : "本机")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button("运行") { run(entry) }
                            .disabled(starting || !model.canInvokePreparedExtensionCommand(entry))
                            .accessibilityIdentifier("plugin-command-run-\(entry.launch.request.command)")
                        Button(model.pluginCommandHasShortcut(entry) ? "清除 ⇧⌘Y" : "绑定 ⇧⌘Y") {
                            model.togglePluginCommandShortcut(id: entry.id)
                        }
                        .disabled(starting)
                        Button("移除") { model.forgetExtensionCommand(id: entry.id) }
                            .disabled(starting)
                    }
                    .buttonStyle(.borderless)
                }
                .frame(minHeight: 144, maxHeight: 300)
            }
            if let issue { Text(verbatim: issue).foregroundStyle(.red) }
            Button("关闭", action: dismiss).keyboardShortcut(.cancelAction)
        }
        .padding(16)
        .frame(width: 460)
        .onAppear { searchFocused = true }
    }

    private func run(_ entry: AppModel.PreparedExtensionCommand) {
        starting = true
        issue = nil
        Task { @MainActor in
            defer { starting = false }
            let found = await model.invokePreparedExtensionCommand(id: entry.id, workspaceID: workspaceID, onValidated: dismiss)
            issue = found ? model.preparedExtensionCommandIssues[entry.id] : "此工作区的命令已失效。"
        }
    }
}
