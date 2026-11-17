import GhostermAppModel
import GhostermCore
import SwiftUI
import UniformTypeIdentifiers
import CoreGraphics
import WebKit

/// Package review, installation, source decisions and explicit launch settings.
/// Runtime content is owned by the generic workspace leaf.
struct ExtensionsPane: View {
    @ObservedObject var model: AppModel
    // A caller can supply a directory chooser (e.g. an embedded host). The
    // default remains SwiftUI's system fileImporter. Both use the same review
    // completion; selecting a directory never approves or activates a package.
    var packagePicker: ((@escaping (Result<[URL], Error>) -> Void) -> Void)? = nil
    @State private var importing = false
    @State private var entries: [Entry] = []
    @State private var issue: String?
    @State private var pendingConnections: [ExtensionGroupInvocation] = []
    @State private var reservationToRelease: ExtensionLocalJournalEntry?
    @State private var inspectionTask: Task<Void, Never>?

    struct Entry: Identifiable {
        let review: ExtensionController.PackageReview
        var signatureDraft = PluginSignatureDraft()
        var installation: ExtensionController.InstalledRelease?
        var decision: String = "尚未批准"
        var localConnection = false { didSet { localAllowed = false; executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; scopedFiles = false; remoteProcesses = false } }
        var localDigest = "" { didSet { localAllowed = false; clientAuthorized = false; attachmentAllowed = false } }
        var localWorkingDirectory = "" { didSet { localAllowed = false; clientAuthorized = false; attachmentAllowed = false } }
        var localTemporaryDirectory = "" { didSet { localAllowed = false; clientAuthorized = false; attachmentAllowed = false } }
        var localAllowed = false
        var localLaunch: ExtensionLocalLaunch? {
            localConnection ? .init(executableSHA256: localDigest, workingDirectory: localWorkingDirectory,
                temporaryDirectory: attachExisting ? localTemporaryDirectory : executionDraft.selection?.temporaryDirectory ?? localTemporaryDirectory,
                authorized: localAllowed) : nil
        }

        var multiplexTemporaryDirectory = ""
        var sshAlias = "" { didSet { executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; localAllowed = false; scopedFiles = false; remoteProcesses = false } }
        var executable = "gterm" { didSet { executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; localAllowed = false; scopedFiles = false; remoteProcesses = false } }
        var executorDirectory = "" { didSet { executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; localAllowed = false } }
        var executorDigest = "" { didSet { executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; localAllowed = false } }
        var command = "" { didSet { executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; localAllowed = false } }
        var argumentsJSON = "[]" { didSet { executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; localAllowed = false } }
        var methodsJSON = "[]" { didSet { executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; localAllowed = false } }
        var hostMethodsJSON = "[]" { didSet { executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; localAllowed = false } }
        var resourceRoot = "" { didSet { executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; localAllowed = false } }
        var processProgramsJSON = "[]" { didSet { executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; localAllowed = false } }
        var processEnvironmentJSON = "[]" { didSet { executionDraft.authorized = false; clientAuthorized = false; attachmentAllowed = false; localAllowed = false } }
        var clientDirectory = "" { didSet { clientAuthorized = false; localAllowed = false } }
        var clientAuthorized = false
        var clientWorkspace: ExtensionController.ClientWorkspace? {
            clientDirectory.isEmpty ? nil : .init(directory: clientDirectory, authorized: clientAuthorized)
        }
        var executionDraft = ExecutorExecutionDraft() { didSet { localAllowed = false } }
        var attachExisting = false { didSet { attachmentAllowed = false; executionDraft.authorized = false; clientAuthorized = false } }
        var ipcDirectory = "" { didSet { attachmentAllowed = false; clientAuthorized = false; localAllowed = false; scopedFiles = false; remoteProcesses = false } }
        var ipcInstance = "" { didSet { attachmentAllowed = false; clientAuthorized = false; localAllowed = false; scopedFiles = false; remoteProcesses = false } }
        var remoteProcesses = false { didSet { attachmentAllowed = false; clientAuthorized = false; localAllowed = false } }
        var scopedFiles = false { didSet { attachmentAllowed = false; clientAuthorized = false; localAllowed = false } }
        var attachmentAllowed = false
        var foregroundDeclaration: PluginForegroundDeclaration?
        var foregroundKey: [Data]?
        var foregroundIssue: String?
        var inspectionKey: [Data] {
            [localConnection ? "local" : "ssh", sshAlias, executable, ipcDirectory, ipcInstance,
             localDigest, localWorkingDirectory, localTemporaryDirectory].map { Data($0.utf8) }
        }
        mutating func adoptForegroundDeclaration() throws {
            guard foregroundKey == inspectionKey, let value = foregroundDeclaration,
                  value.clientDigest == review.digest, review.commands.contains(where: { $0.path == value.command }) else {
                throw GhostermError.failed("声明已过期，或与所选 Client 摘要和命令不匹配")
            }
            executorDigest = value.executorDigest; command = value.command
            let encoder = JSONEncoder()
            argumentsJSON = String(decoding: try encoder.encode(value.arguments), as: UTF8.self)
            methodsJSON = String(decoding: try encoder.encode(value.methods), as: UTF8.self)
            hostMethodsJSON = String(decoding: try encoder.encode(value.hostMethods), as: UTF8.self)
            remoteProcesses = false; scopedFiles = false; attachmentAllowed = false; localAllowed = false
            executionDraft.authorized = false; clientAuthorized = false
        }
        var attachment: ExtensionRelayBootstrap.Attachment? {
            attachExisting ? .init(directory: ipcDirectory, instance: ipcInstance, authorized: attachmentAllowed, remoteProcesses: remoteProcesses, scopedFiles: scopedFiles) : nil
        }
        init(review: ExtensionController.PackageReview) {
            self.review = review
            self.command = review.commands.first?.path ?? ""
        }
        var id: String { review.id + ":" + review.digest }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.m) {
            HStack {
                Text("插件包").font(DS.Font.labelEmphasis)
                Spacer()
                Button("选择文件夹…") {
                    if let packagePicker { packagePicker(selectedPackage) }
                    else { importing = true }
                }
                    .disabled(model.extensionController == nil)
                    .accessibilityIdentifier("plugin-package-import")
            }

            PluginReleaseInstallView(controller: model.extensionController) { installed in
                let entryID = installed.review.id + ":" + installed.review.digest
                if let index = entries.firstIndex(where: { $0.id == entryID }) { entries[index].installation = installed }
                else {
                    var entry = Entry(review: installed.review); entry.installation = installed; entries.append(entry)
                }
            }
            PluginInstalledVersionsView(controller: model.extensionController, didRemove: { releaseID in
                entries.removeAll { $0.installation?.signature.descriptorSHA256 == releaseID }
            }) { pluginID, result in
                for index in entries.indices where entries[index].review.id == pluginID { entries[index].decision = "尚未批准" }
                if let installed = result.release {
                    let entryID = installed.review.id + ":" + installed.review.digest
                    if let index = entries.firstIndex(where: { $0.id == entryID }) { entries[index].installation = installed }
                    else { var entry = Entry(review: installed.review); entry.installation = installed; entries.append(entry) }
                }
            }

            if model.extensionController == nil {
                Label("此构建未启用 Portable JS 插件运行时。", systemImage: "exclamationmark.circle")
                    .foregroundStyle(.secondary)
                SettingsNote("普通 Ghosterm 构建默认关闭插件运行时；当前不能审查或启动 Client 插件。")
            } else if entries.isEmpty {
                SettingsNote("选择插件包文件夹后，Ghosterm 会检查固定内容摘要、兼容性和命令。审查不会自动批准或启动插件。")
            }

            if let issue {
                Text(issue).foregroundStyle(.red).textSelection(.enabled)
            }
            if !model.frontendCandidates.isEmpty {
                Text("终端候选尚未核实。请审查对应 Client 包，并明确选择实际执行端后检查；来源终端和外层 SSH 不证明执行端身份。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
                ForEach(model.frontendCandidates) { candidate in
                    HStack(alignment: .top) {
                        Text("\(candidate.sourceDescription)\n\(candidate.offer.directory)\nClient：\(candidate.offer.clientDigest)")
                            .font(DS.Font.caption).textSelection(.enabled)
                        Spacer()
                        Button("移除此候选") { model.dismissFrontendCandidate(candidate.id) }
                    }
                }
            }
            if let issue = model.frontendDiscoveryIssue { Text(issue).font(DS.Font.caption).foregroundStyle(.secondary) }

            ForEach($entries) { $entry in
                VStack(alignment: .leading, spacing: DS.Padding.s) {
                    HStack(alignment: .firstTextBaseline) {
                        Text(entry.review.id).font(DS.Font.labelEmphasis)
                        Spacer()
                        Text(entry.decision).foregroundStyle(.secondary)
                    }
                    Text("版本 \(entry.review.version) · 发布者 \(entry.review.publisher) · \(entry.review.license)")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    if let installed = entry.installation {
                        Text("已安装签名版本 · 渠道 \(installed.signature.channel)\n\(installed.directory.path)")
                            .font(DS.Font.caption).textSelection(.enabled)
                    }
                    PluginSignatureReviewView(controller: model.extensionController, review: entry.review,
                                              draft: $entry.signatureDraft)
                    Text("Client 运行时：\(entry.review.runtime) · 结构审查身份：\(entry.review.publisherAuthenticated ? "已认证" : "未认证")")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    Text("声明的必需能力：\(entry.review.requiredCapabilities.isEmpty ? "无" : entry.review.requiredCapabilities.joined(separator: ", "))")
                        .font(DS.Font.caption)
                    Text("缺失的可选能力：\(entry.review.optionalMissingCapabilities.isEmpty ? "无" : entry.review.optionalMissingCapabilities.joined(separator: ", "))")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    Text("摘要：\(entry.review.digest)").font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                    Text("命令：\(entry.review.commands.map(\.path).joined(separator: ", "))")
                        .font(DS.Font.caption)
                    Text("能力授权：\(entry.review.authorizationGranted ? "已授予" : "未授予")；来源批准不会授予能力。")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    if entry.decision == "已批准（本进程）" {
                        VStack(alignment: .leading, spacing: DS.Padding.s) {
                            Toggle("在本机连接 Executor", isOn: $entry.localConnection)
                                .accessibilityIdentifier("plugin-local-mode")
                            if !entry.localConnection {
                            TextField("SSH 主机别名", text: $entry.sshAlias)
                                .accessibilityIdentifier("plugin-ssh-alias-\(entry.review.id)")
                            }
                            TextField(entry.localConnection ? "本机 gterm 绝对路径" : "远端 gterm 可执行文件", text: $entry.executable)
                                .accessibilityIdentifier("plugin-executable-\(entry.review.id)")
                            if entry.localConnection {
                                TextField("所选 gterm SHA-256", text: $entry.localDigest)
                                    .accessibilityIdentifier("plugin-local-digest")
                                TextField("本机工作目录", text: $entry.localWorkingDirectory)
                                    .accessibilityIdentifier("plugin-local-working-directory")
                                if entry.attachExisting || entry.executionDraft.selection == nil {
                                    TextField("本机临时根目录（每次新建私有子目录）", text: $entry.localTemporaryDirectory)
                                        .accessibilityIdentifier("plugin-local-temporary-directory")
                                }
                                Toggle("允许本次运行所填本机 gterm 并清理其私有临时目录（最长十分钟）", isOn: $entry.localAllowed)
                                    .accessibilityIdentifier("plugin-local-allow")
                                Text("核对程序来源与摘要。修改连接、命令或依赖后须重新允许；插件能力仍会另行确认。")
                                    .font(DS.Font.caption).foregroundStyle(.secondary)
                            }
                            Toggle("连接已有前台 gterm 实例", isOn: $entry.attachExisting)
                                .accessibilityIdentifier("plugin-ipc-mode")
                            ForEach(model.frontendCandidates.filter { $0.offer.clientDigest == entry.review.digest }) { candidate in
                                Button("使用终端候选位置：\(candidate.sourceDescription)") {
                                    guard model.frontendCandidateIsCurrent(candidate) else { issue = "候选已失效，请重新运行前台命令。"; return }
                                    entry.attachExisting = true
                                    entry.ipcDirectory = candidate.offer.directory; entry.ipcInstance = candidate.offer.instance
                                    entry.foregroundDeclaration = nil; entry.foregroundKey = nil
                                    entry.remoteProcesses = false; entry.scopedFiles = false; entry.attachmentAllowed = false
                                    entry.localAllowed = false; entry.clientAuthorized = false; entry.executionDraft.authorized = false
                                }.accessibilityIdentifier("plugin-discovery-use-\(candidate.id)")
                            }
                            if entry.attachExisting {
                                TextField(entry.localConnection ? "本机私有 IPC 目录" : "远端私有 IPC 目录", text: $entry.ipcDirectory)
                                    .accessibilityIdentifier("plugin-ipc-directory")
                                TextField("前台显示的 32 位实例标识", text: $entry.ipcInstance)
                                    .accessibilityIdentifier("plugin-ipc-instance")
                                foregroundInspection($entry)
                                Toggle("允许沿用此实例已选工作区的只读文件范围", isOn: $entry.scopedFiles)
                                    .accessibilityIdentifier("plugin-ipc-files")
                                Toggle("允许沿用此实例已有的业务进程范围", isOn: $entry.remoteProcesses)
                                    .accessibilityIdentifier("plugin-ipc-processes")
                                Toggle("明确连接所选位置的该前台实例", isOn: $entry.attachmentAllowed)
                                    .accessibilityIdentifier("plugin-ipc-allow")
                                Text("请核对原前台进程显示的标识、命令与参数。运行档、工作区与程序映射沿用前台启动配置，连接不会修改；失效或不匹配将报错。")
                                    .font(DS.Font.caption).foregroundStyle(.secondary)
                            } else {
                            ExecutorRuntimeFields(draft: $entry.executionDraft, location: entry.localConnection ? "本机" : "远端")
                            TextField("Executor 包目录", text: $entry.executorDirectory)
                                .accessibilityIdentifier("plugin-executor-directory-\(entry.review.id)")
                            }
                            TextField("Executor SHA-256", text: $entry.executorDigest)
                                .font(.system(size: 11, design: .monospaced))
                                .accessibilityIdentifier("plugin-executor-digest-\(entry.review.id)")
                            TextField("完整命令路径", text: $entry.command)
                                .accessibilityIdentifier("plugin-command-\(entry.review.id)")
                            TextField("命令参数 JSON 字符串数组", text: $entry.argumentsJSON)
                                .accessibilityIdentifier("plugin-arguments-\(entry.review.id)")
                            TextField("Executor RPC allowlist JSON", text: $entry.methodsJSON)
                                .accessibilityIdentifier("plugin-executor-methods-\(entry.review.id)")
                            TextField("Host RPC allowlist JSON", text: $entry.hostMethodsJSON)
                                .accessibilityIdentifier("plugin-host-methods-\(entry.review.id)")
                            if !entry.attachExisting {
                            TextField("执行端资源目录（留空则不开放）", text: $entry.resourceRoot)
                                .accessibilityIdentifier("plugin-resource-root-\(entry.review.id)")
                            }
                            if entry.review.requiredCapabilities.contains("resource.receive") {
                                TextField("Native 本机只读资源目录（可选）", text: $entry.clientDirectory)
                                    .accessibilityIdentifier("plugin-client-workspace")
                                Toggle("允许本次 Native Host 从此目录向 Executor 发布资源（单文件最多 256 KiB）",
                                       isOn: $entry.clientAuthorized)
                                    .accessibilityIdentifier("plugin-client-workspace-allow")
                                Text("目录仅供 Native 发布按钮使用，不交给 Client JS，也不传入启动参数。修改目录或连接配置须重新允许。")
                                    .font(DS.Font.caption).foregroundStyle(.secondary)
                            }
                            if !entry.attachExisting {
                            TextField("执行端进程映射 JSON", text: $entry.processProgramsJSON, axis: .vertical)
                                .lineLimit(2...5)
                                .accessibilityIdentifier("plugin-process-programs-\(entry.review.id)")
                            Text("映射字段：name、executable、version、sha256；可选 prefixArguments 字符串数组用于固定解释器入口和 profile 参数。还需提供 workspace root；可执行摘要由执行端 Core 核对。")
                                .font(DS.Font.caption).foregroundStyle(.secondary)
                            TextField("执行端环境变量名称 JSON 数组", text: $entry.processEnvironmentJSON)
                                .accessibilityIdentifier("plugin-process-environment-\(entry.review.id)")
                            Text("只填写执行机器上的变量名，例如 [\"CLAUDE_CONFIG_DIR\"]；不要填写凭据值。缺失的显式变量会报错。")
                                .font(DS.Font.caption).foregroundStyle(.secondary)
                            }
                            if !entry.localConnection && !entry.attachExisting {
                                TextField("连接组的执行端临时目录", text: $entry.multiplexTemporaryDirectory)
                                    .accessibilityIdentifier("plugin-group-temporary-" + entry.review.id)
                            }
                            Button("加入连接组") { enqueue(entry) }
                                .disabled(entry.attachExisting || pendingConnections.count >= 8)
                                .accessibilityIdentifier("plugin-enqueue-" + entry.review.id)
                            Button("连接并启动 Client 命令") { launch(entry) }
                                .disabled(!model.extensionInstance.canDiscard)
                                .accessibilityIdentifier("plugin-launch-\(entry.review.id)")
                        }
                    } else {
                        HStack {
                            Button("批准此摘要") { decide(entry: entry, approve: true) }
                                .disabled(entry.decision == "已批准（本进程）")
                                .accessibilityIdentifier("plugin-approve-\(entry.review.id)")
                            Button("拒绝") { decide(entry: entry, approve: false) }
                                .accessibilityIdentifier("plugin-deny-\(entry.review.id)")
                        }
                    }
                }
                .padding(DS.Padding.m)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(.quaternary, in: RoundedRectangle(cornerRadius: DS.Radius.control))
            }

            connectionGroups
            localReservations
            if let run = model.extensionRun {
                HStack {
                    Text("正在运行：\(run.packageID) · \(run.command)").font(DS.Font.caption)
                    Spacer()
                    Button("前往插件工作区") { model.showExtensionWorkspace(); SettingsWindow.close() }
                        .accessibilityIdentifier("plugin-show-workspace")
                }
            }
            if model.extensionRun == nil, let temporary = model.extensionLastLocalTemporary {
                localTemporaryState(temporary)
                if model.extensionRunIssue != nil, model.extensionLastLocalProcess == nil {
                    Button("重试释放本机临时目录") { Task { await model.retryLocalExtensionCleanup() } }
                        .disabled(model.extensionRunBusy).accessibilityIdentifier("plugin-local-temporary-retry")
                }
            }
            if model.extensionRun == nil, let process = model.extensionLastLocalProcess {
                Text("上次本机进程：\(process.childExited ? "已观察退出" : "退出结果未知") · reap：\(process.reaped ? "完成" : "待确认") · code：\(process.exitCode.map(String.init) ?? "未知") · signal：\(process.termSignal.map(String.init) ?? "无")")
                    .font(DS.Font.caption).accessibilityIdentifier("plugin-local-final-state")
                Text("组清理信号：\(process.groupFenced ? "已发送" : "未确认") · 系统错误：\(process.groupSignalError)").font(DS.Font.caption)
                if !process.diagnostic.isEmpty { Text(process.diagnostic).font(DS.Font.caption).textSelection(.enabled) }
                if model.extensionRunIssue != nil {
                    Button("重试释放本机插件资源") { Task { await model.retryLocalExtensionCleanup() } }
                        .disabled(model.extensionRunBusy).accessibilityIdentifier("plugin-local-cleanup-retry")
                }
            }
            if let message = model.extensionRunIssue {
                Text(message).foregroundStyle(.red).textSelection(.enabled)
            }
            SettingsNote("来源批准不等于授予访问文件或启动程序的权限。运行内容在工作区中打开；关闭窗格会停止插件并释放资源。")
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.folder], allowsMultipleSelection: false,
                      onCompletion: selectedPackage)
        .onDisappear { inspectionTask?.cancel() }
    }

    private func foregroundInspection(_ binding: Binding<Entry>) -> some View {
        let entry = binding.wrappedValue
        return VStack(alignment: .leading, spacing: DS.Padding.s) {
            Button("检查此位置的前台实例") {
                let key = entry.inspectionKey, id = entry.id
                inspectionTask = Task {
                    do {
                        let value = try await model.inspectForegroundPlugin(sshAlias: entry.sshAlias,
                            executable: entry.executable, directory: entry.ipcDirectory, instance: entry.ipcInstance,
                            localLaunch: entry.localLaunch)
                        try Task.checkCancellation()
                        guard let index = entries.firstIndex(where: { $0.id == id }), entries[index].inspectionKey == key else { return }
                        entries[index].foregroundDeclaration = value; entries[index].foregroundKey = key
                        entries[index].foregroundIssue = nil
                    } catch {
                        guard let index = entries.firstIndex(where: { $0.id == id }), entries[index].inspectionKey == key else { return }
                        entries[index].foregroundDeclaration = nil; entries[index].foregroundIssue = String(describing: error)
                    }
                }
            }
            .disabled(!model.extensionInstance.canDiscard || (entry.localConnection && !entry.localAllowed))
            .accessibilityIdentifier("plugin-ipc-inspect")
            if let value = entry.foregroundDeclaration, entry.foregroundKey == entry.inspectionKey {
                Text("当前声明：\(value.command) · \(value.runtime)\nExecutor：\(value.executorDigest)\nClient：\(value.clientDigest)")
                    .font(DS.Font.caption).textSelection(.enabled)
                    .accessibilityIdentifier("plugin-ipc-declaration")
                Text("参数：\(String(decoding: (try? JSONEncoder().encode(value.arguments)) ?? Data(), as: UTF8.self))")
                    .font(DS.Font.caption).textSelection(.enabled)
                Text("Executor RPC：\(value.methods.joined(separator: ", "))\nHost RPC：\(value.hostMethods.joined(separator: ", "))\nClient 服务：\(value.clientCapabilities.joined(separator: ", "))")
                    .font(DS.Font.caption).textSelection(.enabled)
                Button("将核对后的命令和参数填入表单") {
                    do { try binding.wrappedValue.adoptForegroundDeclaration() }
                    catch { binding.wrappedValue.foregroundIssue = String(describing: error) }
                }
                .disabled(model.extensionRunBusy || value.clientDigest != entry.review.digest || !entry.review.commands.contains(where: { $0.path == value.command }))
                .accessibilityIdentifier("plugin-ipc-adopt")
            }
            if let issue = entry.foregroundIssue {
                Text(issue).foregroundStyle(.red).font(DS.Font.caption)
                    .accessibilityIdentifier("plugin-ipc-inspection-issue")
            }
            Text("检查只读取当前声明，不启动插件或批准附着。填入后请重新确认本次连接；工作区与业务进程仍沿用前台配置。")
                .font(DS.Font.caption).foregroundStyle(.secondary)
        }
    }

    private func selectedPackage(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            Task { await review(url) }
        case .failure(let error):
            issue = "无法选择插件包：\(error.localizedDescription)"
        }
    }

    private var localReservations: some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            Button("检查本机临时目录记录") { Task { await model.inspectLocalReservations() } }
                .disabled(model.extensionController == nil || model.localReservationBusy).accessibilityIdentifier("plugin-local-reservations-inspect")
            if !model.localReservationEntries.isEmpty {
                Text("已预约 \(model.localReservationEntries.count) × 256 MiB；总预约上限 1 GiB。残留记录继续计入预约，这不是文件系统写入配额。")
                    .font(DS.Font.caption)
            }
            ForEach(model.localReservationEntries) { entry in
                VStack(alignment: .leading) {
                    Text(entry.directory).textSelection(.enabled).font(DS.Font.caption)
                    Text(reservationLocation(entry)).font(DS.Font.caption)
                    if !entry.held, entry.location == .missing {
                        Button("释放已消失目录的预约") { reservationToRelease = entry }
                            .disabled(model.localReservationBusy).accessibilityIdentifier("plugin-local-reservation-release-\(entry.id)")
                    }
                }
            }
            if let issue = model.localReservationIssue { Text(issue).foregroundStyle(.red).textSelection(.enabled) }
            Text("只检查已记录的目录身份；不恢复进程或授权，也不删除残留目录。目录仍在或身份不明时继续保留预约。")
                .font(DS.Font.caption)
        }
        .confirmationDialog("确认目录已消失后释放这份预约？", isPresented: Binding(
            get: { reservationToRelease != nil }, set: { if !$0 { reservationToRelease = nil } }),
            presenting: reservationToRelease) { entry in
            Button("重新核对并释放预约") {
                reservationToRelease = nil
                Task { await model.releaseMissingLocalReservation(entry, confirmed: true) }
            }
            Button("保留预约", role: .cancel) { reservationToRelease = nil }
        } message: { entry in Text(entry.directory + "\n不会删除任何文件，也不代表旧任务成功或已停止。") }
    }

    private func reservationLocation(_ entry: ExtensionLocalJournalEntry) -> String {
        if entry.held { return "本机运行仍在使用，保留预约" }
        switch entry.location {
        case .matching: return "目录身份匹配，继续保留预约"
        case .missing: return "已记录目录目前不存在，可显式重新核对并释放预约"
        case .changed: return "目录或父目录身份变化，保留预约"
        case .unbound: return "目录创建结果不明，保留预约"
        case .unavailable: return "目录位置查不出来，保留预约"
        }
    }

    private func review(_ url: URL) async {
        guard let controller = model.extensionController else { return }
        issue = nil
        do {
            let value = try await controller.review(directory: url)
            guard !entries.contains(where: { $0.id == value.id + ":" + value.digest }) else { return }
            entries.append(Entry(review: value))
        } catch {
            issue = "插件包审查失败：\(error)"
        }
    }

    private func decide(entry: Entry, approve: Bool) {
        guard let controller = model.extensionController else { return }
        Task {
            do {
                let status = approve
                    ? try await controller.approve(id: entry.review.id, digest: entry.review.digest)
                    : try await controller.deny(id: entry.review.id, digest: entry.review.digest)
                guard let index = entries.firstIndex(where: { $0.id == entry.id }) else { return }
                entries[index].decision = status == 0 ? (approve ? "已批准（本进程）" : "已拒绝") : "裁决失败（\(status)）"
            } catch {
                issue = "无法保存来源裁决：\(error)"
            }
        }
    }

    private func configuration(_ entry: Entry) throws -> ExtensionGroupInvocation {
    let arguments = try ExtensionRelayBootstrap.parseArgumentsJSON(entry.argumentsJSON)
    var methods = try ExtensionRelayBootstrap.parseMethodsJSON(entry.methodsJSON)
    var hostMethods = try ExtensionRelayBootstrap.parseHostMethodsJSON(entry.hostMethodsJSON)
    if entry.command == "ai codex" {
        methods = Array(Set(methods + ["codex.thread.start", "codex.turn.send",
            "codex.turn.interrupt", "codex.approval.respond"])).sorted()
        hostMethods = Array(Set(hostMethods + ["codex.event"])).sorted()
    }
    if entry.command == "ai claude" { methods = Array(Set(methods + ClaudeRPCClient.methods)).sorted() }
    let processPrograms = entry.attachExisting ? [] : try ExtensionRelayBootstrap.parseProcessProgramsJSON(entry.processProgramsJSON)
    let processEnvironment = entry.attachExisting ? [] : try ExtensionRelayBootstrap.parseProcessEnvironmentJSON(entry.processEnvironmentJSON)
    let roots: [ExtensionRelayBootstrap.ScopedRoot] = entry.attachExisting || entry.resourceRoot.isEmpty
        ? [] : [.init(id: "workspace", directory: entry.resourceRoot)]
    try entry.clientWorkspace?.validate(requiredCapabilities: entry.review.requiredCapabilities)
    guard entry.localConnection || !entry.sshAlias.isEmpty else { throw GhostermCore.GhostermError.failed("请填写 SSH 主机别名") }
        let request = ExtensionRelayBootstrap.Request(executable: entry.executable, executorDirectory: entry.attachExisting ? "" : entry.executorDirectory,
            executorDigest: entry.executorDigest, clientDigest: entry.review.digest, methods: methods, hostMethods: hostMethods,
            scopedRoots: roots, processPrograms: processPrograms, processEnvironmentNames: processEnvironment,
            execution: entry.attachExisting ? nil : entry.executionDraft.selection, attachment: entry.attachment,
            command: entry.command, arguments: arguments)
        let temporary = entry.localLaunch?.temporaryDirectory ?? entry.executionDraft.selection?.temporaryDirectory ?? entry.multiplexTemporaryDirectory
        return ExtensionGroupInvocation(packageID: entry.review.id, request: request, sshAlias: entry.sshAlias,
            localLaunch: entry.localLaunch, clientWorkspace: entry.clientWorkspace, temporaryDirectory: temporary)
    }
    private func enqueue(_ entry: Entry) {
        do {
            guard pendingConnections.count < 8, !entry.attachExisting else { throw GhostermError.failed("连接组最多八项，且不能包含已有前台附着") }
            pendingConnections.append(try configuration(entry))
        } catch { issue = "连接组配置无效：\(error)" }
    }
    private func launch(_ entry: Entry) {
        do {
            let launch = try configuration(entry), request = launch.request
            model.showExtensionWorkspace(); SettingsWindow.close()
            Task {
                await model.startExtensionClient(packageID: launch.packageID, packageDigest: request.clientDigest,
                    command: request.command, arguments: request.arguments, sshAlias: launch.sshAlias,
                    executable: request.executable, executorDirectory: request.executorDirectory, executorDigest: request.executorDigest,
                    methods: request.methods, hostMethods: request.hostMethods, scopedRoots: request.scopedRoots,
                    processPrograms: request.processPrograms, processEnvironmentNames: request.processEnvironmentNames,
                    executorExecution: request.execution, executorAttachment: request.attachment,
                    clientWorkspace: launch.clientWorkspace, localLaunch: launch.localLaunch)
            }
        } catch { issue = "插件启动参数无效：\(error)" }
    }

    @ViewBuilder private var connectionGroups: some View {
        if !pendingConnections.isEmpty {
            VStack(alignment: .leading, spacing: 8) {
                Text("待启动连接组（\(pendingConnections.count)/8）").font(DS.Font.caption)
                Text("已固定加入时的配置。组内使用同一目标和 gterm，每个实例仍须单独确认权限。").font(DS.Font.caption).foregroundStyle(.secondary)
                ForEach(pendingConnections) { launch in
                    HStack {
                        Text(verbatim: launch.packageID + " · " + launch.request.command)
                        Spacer()
                        Button("移除") { pendingConnections.removeAll { $0.id == launch.id } }
                    }
                    DisclosureGroup("查看配置") {
                        Text(verbatim: "目标：" + (launch.localLaunch == nil ? launch.sshAlias : "本机") + " · " + launch.request.executable)
                        Text(verbatim: "Client：" + launch.request.clientDigest + "\nExecutor：" + launch.request.executorDigest)
                        Text(verbatim: "命令参数：" + String(decoding: (try? JSONEncoder().encode(launch.request.arguments)) ?? Data(), as: UTF8.self))
                        Text(verbatim: "连接临时根目录：" + launch.temporaryDirectory)
                        if let local = launch.localLaunch { Text(verbatim: local.reviewDescription) }
                        if let execution = launch.request.execution { Text(verbatim: execution.reviewDescription) }
                        if let workspace = launch.clientWorkspace { Text(verbatim: workspace.reviewDescription) }
                        ForEach(launch.request.scopedRoots, id: \.id) { root in Text(verbatim: "只读范围：" + root.id + " · " + root.directory) }
                        ForEach(launch.request.processPrograms, id: \.name) { program in
                            Text(verbatim: "业务进程：" + program.name + " · " + program.executable + " · " + program.sha256)
                            Text(verbatim: "固定参数：" + String(decoding: (try? JSONEncoder().encode(program.prefixArguments)) ?? Data(), as: UTF8.self))
                        }
                        Text(verbatim: "选定环境名称：" + launch.request.processEnvironmentNames.joined(separator: ", "))
                    }.font(DS.Font.caption).textSelection(.enabled)
                }
                Button("连接并启动整组") {
                    let snapshot = pendingConnections; pendingConnections = []
                    SettingsWindow.close()
                    Task { await model.startExtensionGroup(snapshot) }
                }.accessibilityIdentifier("plugin-launch-group")
            }
        }
        if let issue = model.extensionConnectionIssue { Text(verbatim: issue).foregroundStyle(.red) }
        ForEach(model.extensionConnections) { connection in
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(verbatim: "连接组：" + connection.title)
                    Spacer()
                    Button(connection.closed ? "已关闭" : "停止并关闭连接") { Task { _ = await connection.close() } }
                        .disabled(connection.closed)
                }
                ForEach(connection.instances) { instance in
                    Button(instance.extensionRun?.command ?? "查看实例") { model.showExtensionWorkspace(instance); SettingsWindow.close() }
                }
                if let issue = connection.issue { Text(verbatim: issue).font(DS.Font.caption).foregroundStyle(.red) }
                if connection.cleanupPending { Text("连接或资源仍待释放，请重试关闭。").font(DS.Font.caption).foregroundStyle(.red) }
                if let process = connection.process {
                    Text(verbatim: "共享宿主进程退出码：" + (process.exitCode.map(String.init) ?? "未知") + " · reap：" + (process.reaped ? "完成" : "待确认"))
                        .font(DS.Font.caption)
                }
                if let temporary = connection.temporary { localTemporaryState(temporary) }
            }
        }
    }

    @ViewBuilder
    private func localTemporaryState(_ temporary: ExtensionLocalTemporaryState) -> some View {
        Text("\(temporary.detail) · \(temporary.directory)").font(DS.Font.caption).textSelection(.enabled)
            .accessibilityIdentifier("plugin-local-temporary-state")
        Text("目录清理与任务结果分开；宿主退出或目录移除不能确认其它业务进程已停止。")
            .font(DS.Font.caption).foregroundStyle(.secondary)
        if temporary.error != 0 { Text("临时清理错误：\(temporary.error)").font(DS.Font.caption).foregroundStyle(.red) }
    }

}

struct CodexFileChangesView: View {
    let changes: [CodexFileChange]
    var omittedCount = 0

    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.xs) {
            ForEach(changes) { change in
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: DS.Padding.xs) {
                        Text(change.kind == "add" ? "新增" : change.kind == "delete" ? "删除" : "修改")
                            .font(DS.Font.caption).foregroundStyle(change.kind == "delete" ? .red : .secondary)
                        Text(change.path).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                        if let movePath = change.movePath, !movePath.isEmpty {
                            Text("→ \(movePath)").font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                        }
                    }
                    ForEach(change.lines) { line in
                        Text(line.text.isEmpty ? " " : line.text)
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundStyle(line.kind == .added ? .green : line.kind == .removed ? .red :
                                line.kind == .hunk ? .blue : .secondary)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    if change.diffTruncated {
                        Text("补丁内容较长，当前显示部分预览。")
                            .font(DS.Font.caption).foregroundStyle(.secondary)
                    }
                }
                .padding(DS.Padding.xs)
                .background(.background.opacity(0.45), in: RoundedRectangle(cornerRadius: DS.Radius.control))
            }
            if omittedCount > 0 {
                Text("另有 \(omittedCount) 个文件未包含在本次预览中。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            }
        }
    }
}

struct PluginResourceSelection: Equatable {
    private(set) var resourceID: String?

    mutating func select(_ resourceID: String, available: [String]) {
        guard available.contains(resourceID) else { return }
        self.resourceID = resourceID
    }

    mutating func reconcile(_ available: [String]) {
        guard let current = resourceID, available.contains(current) else {
            resourceID = available.first
            return
        }
    }
}

struct PluginResourceBrowser: View {
    let resources: [ExtensionController.ReceivedResource]
    @Binding var selection: PluginResourceSelection

    private var identifiers: [String] { resources.map(\.resourceID) }
    private var selectedResource: ExtensionController.ReceivedResource? {
        resources.first { $0.resourceID == selection.resourceID }
    }

    var body: some View {
        HStack(alignment: .top, spacing: DS.Padding.s) {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: DS.Padding.xs) {
                    ForEach(resources, id: \.resourceID) { resource in
                        Button {
                            selection.select(resource.resourceID, available: identifiers)
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(resource.displayName.isEmpty ? "未命名资源" : resource.displayName)
                                    .lineLimit(1)
                                Text("\(resource.mime) · \(resource.size) 字节")
                                    .font(DS.Font.caption).foregroundStyle(.secondary).lineLimit(1)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(DS.Padding.s)
                            .background(selection.resourceID == resource.resourceID
                                ? Color.accentColor.opacity(0.16) : Color.clear,
                                in: RoundedRectangle(cornerRadius: DS.Radius.control))
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("plugin-resource-select-\(resource.resourceID)")
                    }
                }
            }
            .frame(width: 190)
            .frame(maxHeight: 360)
            if let selectedResource {
                PluginResourcePreviewCard(resource: selectedResource, resources: resources)
                    .id(selectedResource.resourceID)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .accessibilityIdentifier("plugin-resource-preview-\(selectedResource.resourceID)")
            }
        }
        .onAppear { selection.reconcile(identifiers) }
        .onChange(of: identifiers) { selection.reconcile($0) }
    }
}

private struct PluginResourcePreviewCard: View {
    let resource: ExtensionController.ReceivedResource
    let resources: [ExtensionController.ReceivedResource]

    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.xs) {
            Text(resource.displayName.isEmpty ? "未命名资源" : resource.displayName)
                .font(DS.Font.caption).textSelection(.enabled)
            Text("\(resource.mime) · \(resource.size) 字节 · SHA-256 \(resource.sha256)")
                .font(.system(size: 10, design: .monospaced)).textSelection(.enabled)
            if PluginResourceHTMLDecoder.supports(mime: resource.mime) {
                PluginResourceHTMLPreview(resource: resource, resources: resources)
            } else if resource.mime.lowercased().hasPrefix("image/") {
                PluginResourceImagePreview(data: resource.bytes, mime: resource.mime)
            } else if PluginResourceMarkdownDecoder.supports(
                mime: resource.mime, displayName: resource.displayName) {
                PluginResourceMarkdownPreview(resource: resource, resources: resources)
            } else if let preview = String(data: resource.bytes.prefix(16384), encoding: .utf8) {
                Text(preview.isEmpty ? "空文本资源" : preview)
                    .font(.system(size: 11, design: .monospaced))
                    .textSelection(.enabled).lineLimit(12)
            } else {
                Text("此资源不是有效 UTF-8 文本；当前显示摘要，不解码为图片或 HTML。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            }
        }
        .padding(DS.Padding.s)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.background, in: RoundedRectangle(cornerRadius: DS.Radius.control))
    }
}

private struct PluginResourceImagePreview: View {
    let data: Data
    let mime: String
    @State private var image: CGImage?
    @State private var rejected = false

    var body: some View {
        Group {
            if let image {
                Image(decorative: image, scale: 1)
                    .resizable().scaledToFit().frame(maxHeight: 360)
            } else if rejected {
                Text("图片格式不受支持或超过 16,777,216 像素解码上限。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            } else {
                ProgressView("检查图片…")
                    .controlSize(.small)
            }
        }
        .task(id: data) {
            let bytes = data
            let contentType = mime
            guard let decoded = await PluginResourceDecodeTask.run({
                PluginResourceImageDecoder.decode(bytes, mime: contentType)
            }) else { return }
            image = decoded
            rejected = decoded == nil
        }
    }
}

private struct PluginResourceMarkdownPreview: View {
    let resource: ExtensionController.ReceivedResource
    let resources: [ExtensionController.ReceivedResource]
    @State private var markdown: AttributedString?
    @State private var rejected = false

    var body: some View {
        Group {
            if let markdown {
                VStack(alignment: .leading, spacing: DS.Padding.xs) {
                    ForEach(segments(from: markdown)) { segment in
                        if let text = segment.text {
                            Text(text).textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        } else if let asset = segment.asset {
                            PluginResourceImagePreview(data: asset.data, mime: asset.mime)
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            } else if rejected {
                Text("Markdown 无法解析或超过 256 KiB 预览上限。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            } else {
                ProgressView("检查 Markdown…").controlSize(.small)
            }
        }
        .task(id: resource.sha256) {
            let bytes = resource.bytes
            let mime = resource.mime
            let name = resource.displayName
            let baseURL = PluginResourceHTMLAssetResolver.documentURL(name)
            guard let decoded = await PluginResourceDecodeTask.run({
                PluginResourceMarkdownDecoder.decode(bytes, mime: mime, displayName: name,
                                                     baseURL: baseURL)
            }) else { return }
            markdown = decoded
            rejected = decoded == nil
        }
    }

    private func segments(from markdown: AttributedString) -> [Segment] {
        let resolver = PluginResourceHTMLAssetResolver(document: resource, resources: resources)
        var result: [Segment] = []
        var text = AttributedString()

        func flushText() {
            guard !text.characters.isEmpty else { return }
            result.append(Segment(id: result.count, text: text, asset: nil))
            text = AttributedString()
        }

        for run in markdown.runs {
            let part = AttributedString(markdown[run.range])
            if let imageURL = run.imageURL,
               let asset = resolver.resolve(imageURL.absoluteURL), asset.mime.hasPrefix("image/") {
                flushText()
                result.append(Segment(id: result.count, text: nil,
                                      asset: PluginResourceHTMLAssetResolver.Asset(data: asset.data,
                                                                                  mime: asset.mime)))
            } else {
                text.append(part)
            }
        }
        flushText()
        return result
    }

    private struct Segment: Identifiable {
        let id: Int
        let text: AttributedString?
        let asset: PluginResourceHTMLAssetResolver.Asset?
    }
}

private struct PluginResourceHTMLPreview: View {
    let resource: ExtensionController.ReceivedResource
    let resources: [ExtensionController.ReceivedResource]
    @State private var snapshot: PluginResourceHTMLSnapshot?
    @State private var completed: [PluginResourceHTMLSnapshot.Input]?

    var body: some View {
        let identity = PluginResourceHTMLSnapshot.identity(document: resource, resources: resources)
        Group {
            if let snapshot, snapshot.inputs == identity {
                VStack(alignment: .leading, spacing: 6) {
                    PluginResourceHTMLWebView(snapshot: snapshot)
                        .id(snapshot.fingerprint)
                        .frame(minHeight: 180, maxHeight: 420)
                    if snapshot.resolver.omittedAttachments {
                        Text("部分 HTML 附件未通过校验或超过预览预算。")
                            .font(DS.Font.caption).foregroundStyle(.secondary)
                    }
                }
            } else if completed == identity {
                Text("HTML 无法解析、路径无效或超过 256 KiB 预览上限。脚本和网络请求禁止；只加载本次显式收到的同目录 CSS / 图片。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            } else {
                ProgressView("检查 HTML…").controlSize(.small)
            }
        }
        .task(id: identity) {
            snapshot = nil
            completed = nil
            let prepared = await PluginResourceHTMLSnapshot.prepare(document: resource, resources: resources)
            guard !Task.isCancelled else { return }
            snapshot = prepared
            completed = identity
        }
    }
}

private struct PluginResourceHTMLWebView: NSViewRepresentable {
    let snapshot: PluginResourceHTMLSnapshot

    func makeCoordinator() -> PluginResourceHTMLCoordinator {
        PluginResourceHTMLCoordinator(snapshot: snapshot)
    }

    func makeNSView(context: Context) -> WKWebView {
        let configuration = PluginResourceHTMLWebViewConfiguration.make(
            schemeHandler: context.coordinator.schemeHandler)
        let webView = WKWebView(frame: .zero, configuration: configuration)
        webView.navigationDelegate = context.coordinator
        webView.uiDelegate = context.coordinator
        webView.setValue(false, forKey: "drawsBackground")
        context.coordinator.update(snapshot: snapshot, in: webView)
        return webView
    }

    func updateNSView(_ webView: WKWebView, context: Context) {
        context.coordinator.update(snapshot: snapshot, in: webView)
    }

    static func dismantleNSView(_ webView: WKWebView, coordinator: PluginResourceHTMLCoordinator) {
        coordinator.close(in: webView)
    }
}
