import Foundation
import GhostermAppModel
import SwiftUI

@MainActor
final class PluginReleaseInstallDraft: ObservableObject {
    @Published var artifactPath = "" { didSet { clear() } }
    @Published var descriptorPath = "" { didSet { clear() } }
    @Published var signaturePath = "" { didSet { clear() } }
    @Published var policyPath = "" { didSet { trustedPolicy = false; clear() } }
    @Published var trustedPolicy = false { didSet { clear() } }
    @Published var useConfiguredChannel = false { didSet { clear() } }
    @Published private(set) var prepared: ExtensionController.PreparedRelease?
    @Published private(set) var busy = false
    @Published private(set) var issue: String?
    @Published private(set) var installed: ExtensionController.InstalledRelease?
    @Published private(set) var cleanupFailed = false
    private var owner: ExtensionController?
    private var generation = UUID()
    private var activities = 0
    private func beginActivity() { activities += 1; busy = true }
    private func endActivity() { activities -= 1; busy = activities != 0 }
    var canInspect: Bool {
        let paths = useConfiguredChannel ? [artifactPath, descriptorPath, signaturePath] : [artifactPath, descriptorPath, signaturePath, policyPath]
        return (useConfiguredChannel || trustedPolicy) && !busy && !cleanupFailed && prepared == nil && paths.allSatisfy {
            $0.hasPrefix("/") && !$0.utf8.contains(0)
        }
    }
    func clear() {
        generation = UUID(); if !cleanupFailed { issue = nil }; installed = nil
        if let prepared, let owner {
            beginActivity()
            Task {
                defer { endActivity() }
                do { try await owner.discardPreparedRelease(id: prepared.id) }
                catch { cleanupFailed = true; issue = message(error) }
            }
        }
        prepared = nil
    }
    func inspect(using controller: ExtensionController) async {
        guard canInspect else { return }
        clear(); owner = controller; beginActivity()
        defer { endActivity() }
        let token = generation
        let paths = [artifactPath, descriptorPath, signaturePath].map { URL(fileURLWithPath: $0) }
        do {
            let result: ExtensionController.PreparedRelease
            if useConfiguredChannel {
                result = try await controller.prepareReleaseUsingConfiguredChannel(artifact: paths[0], descriptor: paths[1], signature: paths[2])
            } else {
                result = try await controller.prepareSignedRelease(artifact: paths[0], descriptor: paths[1],
                    signature: paths[2], trustedPolicy: URL(fileURLWithPath: policyPath))
            }
            guard token == generation else {
                try await controller.discardPreparedRelease(id: result.id); return
            }
            prepared = result
        } catch {
            if token == generation { cleanupFailed = error is ExtensionController.InstallCleanupError; issue = message(error) }
        }
    }
    func install(using controller: ExtensionController) async -> ExtensionController.InstalledRelease? {
        guard !busy, (useConfiguredChannel || trustedPolicy), let prepared else { return nil }
        beginActivity(); issue = nil
        defer { endActivity() }
        do {
            let result = try await controller.installPreparedRelease(id: prepared.id)
            // Publication is a real effect even if a page disappears meanwhile.
            // Return the captured result; never relabel it as the current draft.
            self.prepared = nil; installed = result
            return result
        } catch { issue = message(error); return nil }
    }
    func retryCleanup(using controller: ExtensionController) async {
        guard !busy else { return }
        beginActivity(); defer { endActivity() }
        do { try await controller.retryFailedInstallCleanup(); issue = nil; cleanupFailed = false }
        catch { cleanupFailed = true; issue = message(error) }
    }
    private func message(_ error: Error) -> String {
        (error as? LocalizedError)?.errorDescription ?? String(describing: error)
    }
}

struct PluginReleaseInstallView: View {
    let controller: ExtensionController?
    let didInstall: (ExtensionController.InstalledRelease) -> Void
    @StateObject private var draft = PluginReleaseInstallDraft()
    @State private var expanded = false
    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            Button(expanded ? "收起发布包安装" : "安装签名发布包…") { expanded.toggle() }
                .disabled(controller == nil).accessibilityIdentifier("plugin-release-expand")
            if expanded {
                Group {
                    TextField("ZIP 发布包绝对路径", text: $draft.artifactPath).accessibilityIdentifier("plugin-release-artifact")
                    TextField("发布描述 release.json", text: $draft.descriptorPath).accessibilityIdentifier("plugin-release-descriptor")
                    TextField("签名 release.sig", text: $draft.signaturePath).accessibilityIdentifier("plugin-release-signature")
                    if controller?.hasConfiguredChannel == true {
                        Toggle("使用已验证的渠道缓存", isOn: $draft.useConfiguredChannel)
                            .accessibilityIdentifier("plugin-release-channel")
                    }
                    if !draft.useConfiguredChannel {
                        TextField("独立可信渠道公钥策略", text: $draft.policyPath).accessibilityIdentifier("plugin-release-policy")
                        Toggle("我已独立核对并信任此渠道公钥策略", isOn: $draft.trustedPolicy)
                            .accessibilityIdentifier("plugin-release-trust")
                    }
                }.disabled(draft.busy)
                Button(draft.busy ? "正在处理…" : "检查发布包") {
                    if let controller { Task { await draft.inspect(using: controller) } }
                }.disabled(controller == nil || !draft.canInspect).accessibilityIdentifier("plugin-release-inspect")
                if let prepared = draft.prepared {
                    if let generation = prepared.channelGeneration {
                        Text("已验证渠道策略 · 第 \(generation) 代").font(DS.Font.caption)
                    }
                    Text("\(prepared.review.id) · \(prepared.review.version) · \(prepared.review.publisher) · \(prepared.review.license)")
                    Text("渠道：\(prepared.signature.channel)\n策略 SHA-256：\(prepared.signature.policySHA256)\n包 SHA-256：\(prepared.review.digest)")
                        .font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                    Text("运行时：\(prepared.review.runtime) · 所需能力：\(prepared.review.requiredCapabilities.joined(separator: ", "))")
                    Text("SDK：\(prepared.sdkVersion) · 协议：\(prepared.protocols.map(String.init).joined(separator: ", "))")
                    Text("未提供的可选能力：\(prepared.review.optionalMissingCapabilities.joined(separator: ", "))")
                    ForEach(Array(prepared.permissions.enumerated()), id: \.offset) { _, permission in
                        Text("\(permission.side) · \(permission.capability)（\(permission.scope)）：\(permission.purpose)")
                            .font(DS.Font.caption).textSelection(.enabled)
                        if !permission.programs.isEmpty {
                            Text("声明的程序：\(permission.programs.joined(separator: ", "))").font(DS.Font.caption).textSelection(.enabled)
                        }
                    }
                    if let declarations = prepared.executorDeclarations {
                        DisclosureGroup("执行端与依赖声明（本次不安装执行端）") {
                            Text(declarations).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                        }
                    }
                    HStack {
                        Button("取消并清理暂存") { draft.clear() }.disabled(draft.busy)
                            .accessibilityIdentifier("plugin-release-cancel")
                        Button("安装此发布包") {
                            if let controller { Task { if let result = await draft.install(using: controller) { didInstall(result) } } }
                        }.disabled(draft.busy).accessibilityIdentifier("plugin-release-install")
                    }
                }
                if let result = draft.installed {
                    Text(result.durabilityConfirmed ? "已安装为独立只读版本" : "版本已发布，但目录持久性确认失败，请保留此结果核对")
                        .accessibilityIdentifier("plugin-release-installed")
                    Text(result.directory.path).font(DS.Font.caption).textSelection(.enabled)
                    if let issue = result.maintenanceIssue { Text(issue).foregroundStyle(.orange) }
                }
                if let issue = draft.issue {
                    Text(issue).foregroundStyle(.red).accessibilityIdentifier("plugin-release-error")
                    if draft.cleanupFailed {
                        Button("重试清理失败的安装暂存") {
                            if let controller { Task { await draft.retryCleanup(using: controller) } }
                        }.disabled(draft.busy).accessibilityIdentifier("plugin-release-retry-cleanup")
                    }
                }
                Text("检查会验证签名、ZIP 完整字节及解压内容，并展示 Core 兼容性结果。安装保存独立版本，不覆盖已有版本、不批准来源或授予 Session 权限；请在已安装版本管理中重新核对并选择活动版本。运行中的实例继续使用原快照，自动更新尚未接入。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            }
        }.onDisappear { draft.clear() }
    }
}
