import GhostermAppModel
import SwiftUI

struct ExecutorExecutionDraft {
    enum Choice: String, CaseIterable { case portable = "Portable JS", node = "Node", exec = "Exec" }
    var choice: Choice = .portable { didSet { authorized = false } }
    var temporaryDirectory = "" { didSet { authorized = false } }
    var executable = "" { didSet { authorized = false } }
    var version = "" { didSet { authorized = false } }
    var sha256 = "" { didSet { authorized = false } }
    var sdkDirectory = "" { didSet { authorized = false } }
    var sdkCatalogSHA256 = "" { didSet { authorized = false } }
    var authorized = false

    var selection: ExtensionRelayBootstrap.Execution? {
        switch choice {
        case .portable: return nil
        case .exec: return .init(runtime: .exec, temporaryDirectory: temporaryDirectory, authorized: authorized)
        case .node:
            let node = ExtensionRelayBootstrap.NodeDependency(executable: executable, version: version,
                sha256: sha256, sdkDirectory: sdkDirectory, sdkCatalogSHA256: sdkCatalogSHA256)
            return .init(runtime: .node(node), temporaryDirectory: temporaryDirectory, authorized: authorized)
        }
    }
}

struct ExecutorRuntimeFields: View {
    @Binding var draft: ExecutorExecutionDraft
    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            Picker("Executor 运行档", selection: $draft.choice) {
                ForEach(ExecutorExecutionDraft.Choice.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }.accessibilityIdentifier("plugin-executor-runtime")
            if draft.choice != .portable {
                TextField("远端已存在的绝对临时目录", text: $draft.temporaryDirectory)
                    .accessibilityIdentifier("plugin-execution-tmpdir")
                if draft.choice == .node {
                    TextField("远端 Node 规范绝对路径", text: $draft.executable)
                        .accessibilityIdentifier("plugin-node-executable")
                    TextField("Node 版本（不带 v）", text: $draft.version)
                        .accessibilityIdentifier("plugin-node-version")
                    TextField("Node 可执行 SHA-256", text: $draft.sha256)
                        .accessibilityIdentifier("plugin-node-sha256")
                    TextField("远端 Host SDK 目录", text: $draft.sdkDirectory)
                        .accessibilityIdentifier("plugin-node-sdk-directory")
                    TextField("Host SDK catalog SHA-256", text: $draft.sdkCatalogSHA256)
                        .accessibilityIdentifier("plugin-node-sdk-sha256")
                }
                Toggle("允许本次 Executor 以远端登录用户身份运行进程", isOn: $draft.authorized)
                    .accessibilityIdentifier("plugin-execution-allow")
                Text("请只运行可信包。Node 和 Host SDK 须预先准备，远端将核对版本及摘要；Exec 使用包内匹配系统与架构的入口。修改运行配置须重新允许；不会自动安装或切换运行档。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            }
        }
    }
}
