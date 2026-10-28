import GhostermAppModel
import GhostermCore
import SwiftUI

struct TmuxSheet: View {
    @ObservedObject var chooser: TmuxChooser
    let close: () -> Void
    @State private var name = ""
    @State private var confirmsTerminate = false
    @State private var terminationTarget: TmuxSession?
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("后台会话 · tmux").font(.title2)
            Text(chooser.targetLabel).foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 6) {
                Text("tmux 程序").font(.headline)
                Picker("程序", selection: Binding(get: { chooser.executablePath }, set: { chooser.selectExecutable($0) })) {
                    Text("自动：使用 PATH 中第一个").tag("")
                    ForEach(chooser.candidates) { candidate in
                        Text(candidate.description).tag(candidate.path)
                    }
                    if !chooser.executablePath.isEmpty && !chooser.candidates.contains(where: { $0.path == chooser.executablePath }) {
                        Text("指定路径 · \(chooser.executablePath)").tag(chooser.executablePath)
                    }
                }.labelsHidden()
                Text("候选来自目标登录环境的 PATH；未列出的程序可手动指定。")
                    .font(.caption).foregroundStyle(.secondary)
                if let warning = chooser.discoveryWarning {
                    Text("PATH 探测失败：\(warning)").font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                }
                if let candidate = chooser.executablePath.isEmpty ? chooser.candidates.first : chooser.candidates.first(where: { $0.path == chooser.executablePath }) {
                    Text(chooser.executablePath.isEmpty ? "默认：\(candidate.description)" : candidate.description)
                        .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                } else if chooser.candidates.isEmpty {
                    Text("未发现 PATH 中的程序；可展开下方手动输入路径。")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }.disabled(chooser.busy)
            DisclosureGroup("手动路径与 socket（可选）") {
                TextField("tmux 绝对路径；留空自动选择", text: $chooser.executablePath)
                TextField("socket 绝对路径；留空使用 tmux 默认实例", text: $chooser.socketPath)
                Button("使用此路径") { chooser.selectExecutable(chooser.executablePath) }
                Text("更改后点刷新。尊重用户配置，不安装程序或抢占已有客户端。")
                    .font(.caption).foregroundStyle(.secondary)
            }.disabled(chooser.busy)
            HStack {
                Text(chooser.message).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                Spacer()
                if chooser.busy { ProgressView().controlSize(.small) }
                Button("刷新") { chooser.refresh() }.disabled(chooser.busy)
            }
            List(selection: $chooser.selectedID) {
                ForEach(chooser.sessions) { session in
                    VStack(alignment: .leading, spacing: 3) {
                        Text(session.name).fontWeight(.medium)
                        Text("\(session.windows) 个窗口 · \(session.attachedClients) 个客户端").font(.caption).foregroundStyle(.secondary)
                    }.tag(session.id)
                }
            }.frame(minHeight: 150)
            if let selected = chooser.selected {
                Text(selected.retentionWarning).font(.callout).foregroundStyle(.secondary)
                Text("socket：\(selected.socketPath)").font(.caption).textSelection(.enabled)
            }
            HStack {
                TextField("新建或重命名的名称", text: $name).frame(minWidth: 180)
                Button("新建") { chooser.change(.create, name: name); name = "" }
                    .disabled(!chooser.canOperate || name.isEmpty)
                Button("重命名") { chooser.change(.rename, name: name); name = "" }
                    .disabled(!chooser.canOperate || chooser.selected == nil || name.isEmpty)
                Button("结束后台会话", role: .destructive) { terminationTarget = chooser.selected; confirmsTerminate = true }
                    .disabled(!chooser.canOperate || chooser.selected == nil)
            }
            Text("新建后从列表选择连接。关闭格子只断开客户端；实际保留行为由 tmux 与系统策略决定。")
                .font(.caption).foregroundStyle(.secondary)
            HStack {
                Button("取消", role: .cancel, action: close).keyboardShortcut(.cancelAction)
                Spacer()
                Button(chooser.replacingPaneID == nil ? "在新标签页连接" : "在原格子连接所选会话") { chooser.connectSelected() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!chooser.canOperate || chooser.selected == nil)
            }
        }
        .padding(20).frame(width: 620)
        .alert("结束所选后台会话？", isPresented: $confirmsTerminate) {
            Button("结束", role: .destructive) {
                if chooser.selected == terminationTarget { chooser.change(.terminate) }
            }
            Button("取消", role: .cancel) {}
        } message: {
            Text("\(chooser.targetLabel)\n\(terminationTarget?.name ?? "")\n其中的程序会结束，其他客户端也可能受影响。")
        }
    }
}
