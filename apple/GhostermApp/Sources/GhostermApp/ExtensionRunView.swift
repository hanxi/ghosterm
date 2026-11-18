import GhostermAppModel
import GhostermCore
import SwiftUI

/// Runtime content belongs to the invocation captured by this view. It never
/// routes a delayed action through the currently focused workspace pane.
struct ExtensionRunView: View {
    @ObservedObject var model: ExtensionInstanceModel
    @State private var resourceSelection = PluginResourceSelection()
    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            if let run = model.extensionRun { activeRun(run) }
            else if model.extensionRunBusy { ProgressView("正在连接插件…") }
            else { Text(model.closeRequested ? "插件已停止" : "插件尚未启动").foregroundStyle(.secondary) }
            if let message = model.extensionRunIssue {
                Text(message).foregroundStyle(.red).textSelection(.enabled)
            }
            if model.extensionRun == nil, model.cleanupPending {
                Button("重试释放插件资源") { Task { await model.retryLocalExtensionCleanup() } }
                    .disabled(model.extensionRunBusy)
                    .accessibilityIdentifier("plugin-cleanup-retry")
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    @ViewBuilder
    private func localTemporaryState(_ temporary: ExtensionLocalTemporaryState) -> some View {
        Text("\(temporary.detail) · \(temporary.directory)").font(DS.Font.caption).textSelection(.enabled)
            .accessibilityIdentifier("plugin-local-temporary-state")
        Text("目录清理与任务结果分开；宿主退出或目录移除不能确认其它业务进程已停止。")
            .font(DS.Font.caption).foregroundStyle(.secondary)
        if temporary.error != 0 { Text("临时清理错误：\(temporary.error)").font(DS.Font.caption).foregroundStyle(.red) }
    }

    @ViewBuilder
    private func activeRun(_ run: AppModel.ExtensionRun) -> some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            Text("插件运行").font(DS.Font.labelEmphasis)
            Text("\(run.packageID) · \(run.command) · \(run.peerIdentity)")
                .font(DS.Font.caption).textSelection(.enabled)
            if run.session?.phase == GhostermCore.ExtensionSession.Phase.consent.rawValue {
                connectionDetails(run)
            } else {
                DisclosureGroup("连接与授权详情") { connectionDetails(run) }
            }
            if run.clientWorkspace != nil, run.runtime != nil {
                NativeResourcePublisher(model: model, run: run)
            }
            if let session = run.session,
               session.phase == GhostermCore.ExtensionSession.Phase.consent.rawValue {
                Text("执行端请求建立 Native Session。请核对上方固定包摘要、执行端身份、命令、参数与本次 RPC allowlist；批准后 Client JavaScript 才会激活。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
                Text("请求能力：\(run.requiredCapabilities.joined(separator: ", "))")
                    .font(DS.Font.caption).textSelection(.enabled)
                HStack {
                    Button("拒绝并结束") { Task { await model.decideExtensionConsent(allow: false) } }
                        .accessibilityIdentifier("plugin-consent-deny")
                    Button("批准并启动") { Task { await model.decideExtensionConsent(allow: true) } }
                        .buttonStyle(.borderedProminent)
                        .accessibilityIdentifier("plugin-consent-allow")
                }
                .disabled(model.extensionRunBusy)
            } else {
                if let session = run.session {
                    Text("Session phase：\(session.phase) · end：\(session.end) · cleanup：\(session.cleanupError)")
                        .font(DS.Font.caption).textSelection(.enabled)
                }
                if let runtime = run.runtime {
                    Text((model.runtimeStateIsUnknown ? "Runtime 最后观测（当前状态未知）：" : "Runtime phase：") + "\(runtime.phase) · failure：\(runtime.failure) · cleanup：\(runtime.cleanupError)")
                        .font(DS.Font.caption).textSelection(.enabled)
                }
                if !run.receivedResources.isEmpty {
                    Text("本次运行收到的资源").font(DS.Font.labelEmphasis)
                    PluginResourceBrowser(resources: run.receivedResources, selection: $resourceSelection)
                }
                codexControls(run: run)
                if let views = model.extensionRegisteredViews {
                    PluginViewsView(model: views)
                }
                if let table = model.extensionTable {
                    PluginTableView(model: table)
                }
                if let diff = model.extensionDiff {
                    PluginDiffView(model: diff)
                }
                if let task = model.extensionTask {
                    PluginTaskView(model: task)
                }
                if run.command == "ai claude", let conversation = model.claudeConversation {
                    ClaudeConversationView(model: conversation)
                } else if run.command == "ai claude", run.runtime?.phase == ExtensionRuntime.Phase.active.rawValue {
                    Text("本次会话未提供完整的 Claude 操作能力。")
                        .foregroundStyle(.red)
                }
                Button(model.extensionRunBusy ? "正在停止…" : "停止并释放插件") {
                    Task { await model.stopExtensionRun() }
                }
                .disabled(model.extensionRunBusy)
                .accessibilityIdentifier("plugin-stop")
            }
        }
        .padding(DS.Padding.m)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary, in: RoundedRectangle(cornerRadius: DS.Radius.control))
        .onChange(of: model.extensionRun?.receivedResources.map(\.resourceID) ?? []) { identifiers in
            resourceSelection.reconcile(identifiers)
        }
    }

    @ViewBuilder
    private func connectionDetails(_ run: AppModel.ExtensionRun) -> some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            if let local = run.localLaunch {
                Text(local.reviewDescription).font(DS.Font.caption).textSelection(.enabled)
                    .accessibilityIdentifier("plugin-local-review")
            }
            if let temporary = model.extensionLastLocalTemporary ?? run.runtime?.temporary ?? run.session?.temporary { localTemporaryState(temporary) }
            if let process = model.extensionLastLocalProcess ?? run.runtime?.process ?? run.session?.process {
                Text("本机进程：\(process.childExited ? "已观察退出" : "尚无退出结果") · reap：\(process.reaped ? "完成" : "待确认") · code：\(process.exitCode.map(String.init) ?? "未知") · signal：\(process.termSignal.map(String.init) ?? "无")")
                    .font(DS.Font.caption).accessibilityIdentifier("plugin-local-process-state")
                Text("组清理信号：\(process.groupFenced ? "已发送" : "未确认") · 系统错误：\(process.groupSignalError)")
                    .font(DS.Font.caption)
                if !process.diagnostic.isEmpty { Text(process.diagnostic).font(DS.Font.caption).textSelection(.enabled) }
            }
            Text("Client 摘要：\(run.packageDigest)")
                .font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
            Text("Executor 摘要：\(run.executorDigest)")
                .font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
            Text("\(run.localLaunch == nil ? "远端启动命令" : "本机启动参数")：\(run.remoteCommand)")
                .font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
            Text("参数：\(run.arguments.isEmpty ? "无" : run.arguments.map { String(reflecting: $0) }.joined(separator: " "))")
                .font(DS.Font.caption).textSelection(.enabled)
            Text("Executor 运行档：\(run.executorAttachment?.reviewDescription ?? run.executorExecution?.reviewDescription(temporaryLabel: run.localLaunch == nil ? "执行端临时目录" : "本机临时根目录（每次新建私有子目录）") ?? "Portable JS（内嵌运行时）")")
                .font(DS.Font.caption).textSelection(.enabled)
                .accessibilityIdentifier("plugin-consent-executor-runtime")
            Text(run.clientWorkspace?.reviewDescription ?? "Client 本机资源目录：未开放")
                .font(DS.Font.caption).textSelection(.enabled)
                .accessibilityIdentifier("plugin-consent-client-workspace")
            Text("按已检查 argsSchema 解码：\(run.decodedArguments ?? "无映射（空参数）")")
                .font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
            Text("Executor RPC：\(run.methods.isEmpty ? "无" : run.methods.joined(separator: ", "))")
                .font(DS.Font.caption)
            Text("Host RPC：\(run.hostMethods.isEmpty ? "无" : run.hostMethods.joined(separator: ", "))")
                .font(DS.Font.caption)
        }
    }

    @ViewBuilder
    private func codexControls(run: AppModel.ExtensionRun) -> some View {
        if run.command == "ai codex" {
            if !model.codexTimeline.isEmpty {
                Text("Codex 对话与工具活动").font(DS.Font.labelEmphasis)
                ForEach(model.codexTimeline) { item in
                    VStack(alignment: .leading, spacing: DS.Padding.xs) {
                        Text(item.title).font(DS.Font.labelEmphasis)
                        if !item.fileChanges.isEmpty {
                            CodexFileChangesView(changes: item.fileChanges, omittedCount: item.omittedFileCount)
                        } else if !item.detail.isEmpty {
                            Text(item.detail).font(.system(size: 12, design: .monospaced))
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        if let reference = item.changeReference {
                            CodexChangeSnapshotControl(model: model, reference: reference)
                        } else if item.changeUnavailable {
                            Text("完整补丁超过读取上限，当前只有部分预览。")
                                .font(DS.Font.caption).foregroundStyle(.secondary)
                        }
                    }
                    .padding(DS.Padding.s)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(.background.opacity(0.55), in: RoundedRectangle(cornerRadius: DS.Radius.control))
                    .accessibilityIdentifier("plugin-codex-timeline-\(item.id)")
                }
            }
            if !run.codexEvents.isEmpty {
                DisclosureGroup("最近原始 Codex 协议事件（\(run.codexEvents.count)）") {
                    Text("仅显示近期原始事件；待处理审批单独保留。")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    ForEach(Array(run.codexEvents.enumerated()), id: \.offset) { _, event in
                        Text(event).font(.system(size: 10, design: .monospaced)).textSelection(.enabled)
                    }
                }
            }
            if !model.codexPendingApprovals.isEmpty {
                Text("Codex 等待中的批准请求").font(DS.Font.labelEmphasis)
                ForEach(model.codexPendingApprovals, id: \.stableID) { approval in
                    let snapshotID = model.codexApprovalSnapshot(approval)
                    VStack(alignment: .leading, spacing: DS.Padding.xs) {
                        Text(approval.method).font(DS.Font.caption).textSelection(.enabled)
                        if approval.method == "item/fileChange/requestApproval",
                           let itemID = approval.itemID,
                           let entry = model.codexTimeline.first(where: { $0.id == "item:\(itemID)" }) {
                            if !entry.fileChanges.isEmpty {
                                CodexFileChangesView(changes: entry.fileChanges, omittedCount: entry.omittedFileCount)
                            }
                            if let reference = entry.changeReference {
                                CodexChangeSnapshotControl(model: model, reference: reference)
                            } else if entry.changeUnavailable {
                                Text("完整补丁超过读取上限，当前只有部分预览。")
                                    .font(DS.Font.caption).foregroundStyle(.secondary)
                            }
                        } else if approval.method == "item/fileChange/requestApproval" {
                            Text("正在等待 Codex 提供补丁内容。")
                                .font(DS.Font.caption).foregroundStyle(.secondary)
                        } else if approval.method == "item/permissions/requestApproval" {
                            ConversationReadOnlyText(text: approval.paramsJSON,
                                identifier: "plugin-codex-permissions-\(approval.stableID)", label: "本轮完整权限请求")
                                .id(approval.permissionReviewID).frame(height: 160)
                        } else {
                            Text(approval.paramsJSON).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                        }
                        if approval.method == "item/permissions/requestApproval" {
                            Text("以下完整权限范围仅用于当前轮，可能允许范围内后续操作。")
                                .font(DS.Font.caption).foregroundStyle(.secondary)
                        }
                        ConversationDecisionButtons(prefix: "plugin-codex-approval", busy: model.codexCallBusy,
                            canAllow: approval.method == "item/commandExecution/requestApproval" || snapshotID != nil,
                            allowLabel: approval.method == "item/permissions/requestApproval" ? "允许本轮所列权限" : "批准一次",
                            deny: { Task { await model.respondCodexApproval(id: approval.id, allow: false, snapshotID: snapshotID) } },
                            allow: { Task { await model.respondCodexApproval(id: approval.id, allow: true, snapshotID: snapshotID) } })
                            .disabled(!model.canInteractWithRuntime)
                        if approval.method == "item/fileChange/requestApproval" && snapshotID == nil {
                            Text("先读取并核对当前完整补丁，才能批准此版本。")
                                .font(DS.Font.caption).foregroundStyle(.secondary)
                        }
                    }
                }
            }
            if let threadID = model.codexThreadID {
                Text("Codex thread：\(threadID)").font(DS.Font.caption).textSelection(.enabled)
                ConversationComposer(placeholder: "发送给 Codex", draft: $model.codexMessageDraft,
                    busy: model.codexCallBusy, canSend: model.canInteractWithRuntime && !model.codexTurnActive && !model.codexSendUnconfirmed,
                    prefix: "plugin-codex") {
                    Task { await model.sendCodexMessage() }
                }
                .disabled(!model.canInteractWithRuntime)
                if model.codexSendUnconfirmed && !model.codexCallBusy {
                    Text("上一条消息的发送结果未知，当前会话无法再次发送。")
                        .font(DS.Font.caption).accessibilityIdentifier("plugin-codex-send-unconfirmed")
                }
                if model.codexTurnActive {
                    Button(model.codexCallBusy ? "正在处理中…" : "中断当前 turn") {
                        Task { await model.interruptCodexTurn() }
                    }
                    .disabled(!model.canInteractWithRuntime || model.codexCallBusy || !run.methods.contains("codex.turn.interrupt"))
                    .accessibilityIdentifier("plugin-codex-interrupt")
                }
            } else {
                Button(model.codexCallBusy ? "正在启动…" : "启动 Codex 对话") {
                    Task { await model.startCodexThread() }
                }
                .disabled(!model.canInteractWithRuntime || model.codexCallBusy || !run.methods.contains("codex.thread.start"))
                .accessibilityIdentifier("plugin-codex-start-thread")
            }
        }
    }
}

private struct NativeResourcePublisher: View {
    @ObservedObject var model: ExtensionInstanceModel
    let run: AppModel.ExtensionRun
    @State private var path = ""
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            TextField("从已允许本机目录发布的相对文件名", text: $path)
                .disabled(!model.canInteractWithRuntime)
                .accessibilityIdentifier("plugin-client-publication-path")
            Button("向 Executor 发布文件") {
                guard let instanceID = run.runtime?.instanceID else { return }
                let selectedPath = path
                Task { await model.publishClientResource(relativePath: selectedPath, instanceID: instanceID) }
            }
                .disabled(path.isEmpty || !model.canInteractWithRuntime ||
                    run.runtime?.publication?.released == false)
                .accessibilityIdentifier("plugin-client-publish")
            if let publication = run.runtime?.publication {
                Text("\(publication.relativePath)：\(publication.reviewDescription)")
                    .font(.caption).textSelection(.enabled)
                    .accessibilityIdentifier("plugin-client-publication-state")
                if !publication.sha256.isEmpty { Text("\(publication.bytes) 字节 · SHA-256 \(publication.sha256)").font(.caption).textSelection(.enabled) }
                if !publication.released {
                    Button("取消发布") { Task { await model.cancelClientPublication(id: publication.id) } }
                        .accessibilityIdentifier("plugin-client-publication-cancel")
                }
            }
        }
    }
}
