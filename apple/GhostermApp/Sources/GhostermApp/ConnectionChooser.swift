import GhostermAppModel
import SwiftUI

/// 窗口内的选择器只收集意图；关闭 sheet 后才启动连接或呈现 tmux。
struct ConnectionChooser: View {
    @ObservedObject var model: AppModel
    let choose: (ConnectionRequest) -> Void
    let manageHosts: () -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""
    @State private var target: AppModel.Target? = .localShell
    @State private var kind: ConnectionRequest.Kind = .terminal
    @FocusState private var searchFocused: Bool

    private var hosts: [HostEntry] { model.hosts.filter { ConnectionSearch.matches(query, text: $0.pattern + " " + $0.summary) } }
    private var showsLocal: Bool { ConnectionSearch.matches(query, text: "本机 本地 local localhost") }
    private var selected: AppModel.Target? {
        guard let target else { return nil }
        switch target {
        case .localShell: return showsLocal ? target : nil
        case .host(let pattern): return model.loadError == nil && hosts.contains { $0.pattern == pattern } ? target : nil
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("新建连接").font(DS.Font.titleLarge)
                Spacer()
                Button("管理 SSH 配置…", action: manageHosts)
            }
            TextField("搜索主机名称或地址", text: $query)
                .textFieldStyle(.roundedBorder).focused($searchFocused)
                .onSubmit(connect)
            Picker("打开为", selection: $kind) {
                ForEach(ConnectionRequest.Kind.allCases) { kind in Text(kind.title).tag(kind) }
            }.pickerStyle(.segmented)
            if let error = model.loadError {
                ConnectionLoadError(message: error, retry: model.reload)
            }
            List(selection: $target) {
                if showsLocal {
                    Label("本机", systemImage: "desktopcomputer").tag(AppModel.Target.localShell)
                }
                ForEach(hosts) { host in
                    VStack(alignment: .leading, spacing: 3) {
                        Text(host.pattern)
                        Text(host.summary).font(DS.Font.caption).foregroundStyle(.secondary)
                    }
                    .tag(AppModel.Target.host(host.pattern))
                }
                if !showsLocal && hosts.isEmpty && model.loadError == nil {
                    Text("没有匹配的主机").foregroundStyle(.secondary)
                }
            }
            .frame(height: 250)
            if model.hosts.isEmpty && query.isEmpty && model.loadError == nil {
                Text("尚未配置 SSH 主机，可在「管理 SSH 配置」中添加。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            }
            HStack {
                Text(kind == .tmux ? "后台会话由 tmux 管理，下一步选择会话。" : "每次打开终端都会建立新的会话。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
                Spacer()
                Button("取消") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("打开", action: connect).keyboardShortcut(.defaultAction).disabled(selected == nil)
            }
        }
        .padding(20).frame(width: 530)
        .onAppear { searchFocused = true }
        .onChange(of: query) { _ in
            if selected == nil { target = showsLocal ? .localShell : hosts.first.map { .host($0.pattern) } }
        }
    }

    private func connect() {
        guard let selected else { return }
        choose(ConnectionRequest(target: selected, kind: kind))
    }
}

struct ConnectionLoadError: View {
    let message: String
    let retry: () -> Void
    var body: some View {
        HStack(alignment: .top) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            Text("无法读取 SSH 配置：\(message)").font(DS.Font.callout).textSelection(.enabled)
            Spacer()
            Button("重试", action: retry)
        }
        .padding(10).background(Color.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 8))
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("connection-load-error")
    }
}

struct HostManagerView: View {
    @ObservedObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""
    @State private var editor: HostEditorRequest?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("管理 SSH 配置").font(DS.Font.titleLarge)
                Spacer()
                Button("刷新", action: model.reload)
                Button("新增主机…") { editor = HostEditorRequest(alias: nil) }
                    .disabled(model.loadError != nil)
            }
            Text(model.configPath).font(DS.Font.caption).foregroundStyle(.secondary).textSelection(.enabled)
            TextField("搜索主机或通配块", text: $query).textFieldStyle(.roundedBorder)
            if let error = model.loadError { ConnectionLoadError(message: error, retry: model.reload) }
            List {
                Section("主机") {
                    ForEach(model.hosts.filter { ConnectionSearch.matches(query, text: $0.pattern + " " + $0.summary) }) { host in
                        HStack {
                            VStack(alignment: .leading) {
                                Text(host.pattern)
                                Text(host.summary).font(DS.Font.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button("编辑…") { editor = HostEditorRequest(alias: host.pattern) }
                                .accessibilityLabel("编辑 \(host.pattern)")
                        }
                    }
                }
                Section("通配块 · 仅用于配置") {
                    ForEach(model.wildcards.filter { ConnectionSearch.matches(query, text: $0) }, id: \.self) { pattern in
                        HStack {
                            Label(pattern, systemImage: "asterisk")
                            Spacer()
                            Button("编辑…") { editor = HostEditorRequest(alias: pattern) }
                                .accessibilityLabel("编辑 \(pattern)")
                        }
                    }
                }
                if model.hosts.isEmpty && model.wildcards.isEmpty && model.loadError == nil {
                    Text("尚未配置主机").foregroundStyle(.secondary)
                }
            }.frame(height: 290)
            HStack {
                Text("保存时自动备份，保留原有注释与其他配置。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
                Spacer()
                Button("完成") { dismiss() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding(20).frame(width: 570)
        .sheet(item: $editor) { request in
            HostEditorSheet(model: model, alias: request.alias ?? "", isNew: request.alias == nil)
        }
    }
}

private struct HostEditorRequest: Identifiable {
    let id = UUID()
    let alias: String?
}
