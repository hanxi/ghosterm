import Foundation
import GhostermAppModel
import SwiftUI

/// Page-local evidence. Editing inputs or withdrawing trust also invalidates
/// in-flight results. A receipt never changes the package's source decision.
struct PluginSignatureDraft {
    var descriptorPath = "" { didSet { invalidate() } }
    var signaturePath = "" { didSet { invalidate() } }
    var policyPath = "" { didSet { trustedPolicy = false; invalidate() } }
    var trustedPolicy = false { didSet { invalidate() } }
    private(set) var receipt: PluginDirectorySignature?
    private(set) var issue: String?
    private(set) var request: UUID?
    var canVerify: Bool {
        trustedPolicy && request == nil &&
            [descriptorPath, signaturePath, policyPath].allSatisfy { $0.hasPrefix("/") && !$0.utf8.contains(0) }
    }
    private mutating func invalidate() { receipt = nil; issue = nil; request = nil }
    mutating func begin() -> UUID? {
        guard canVerify else { return nil }
        receipt = nil; issue = nil
        let token = UUID(); request = token; return token
    }
    mutating func finish(_ token: UUID, result: Result<PluginDirectorySignature, Error>) {
        guard request == token else { return }
        request = nil
        switch result {
        case .success(let value): receipt = value
        case .failure(let error): receipt = nil; issue = error.localizedDescription
        }
    }
}

struct PluginSignatureReviewView: View {
    let controller: ExtensionController?
    let review: ExtensionController.PackageReview
    @Binding var draft: PluginSignatureDraft
    @State private var expanded = false

    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            Button(expanded ? "收起目录包签名" : "目录包发布签名…") { expanded.toggle() }
                .accessibilityIdentifier("plugin-signature-disclosure")
            if expanded {
            VStack(alignment: .leading, spacing: DS.Padding.s) {
                Text("填写公开发布材料的绝对路径，并从独立可信来源核对渠道公钥策略。包自带的策略不能证明其发布者。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
                TextField("发布描述 release.json", text: $draft.descriptorPath)
                    .accessibilityIdentifier("plugin-signature-descriptor")
                TextField("签名 release.sig", text: $draft.signaturePath)
                    .accessibilityIdentifier("plugin-signature-file")
                TextField("可信渠道公钥策略", text: $draft.policyPath)
                    .accessibilityIdentifier("plugin-signature-policy")
                Toggle("我已独立核对并信任此渠道公钥策略", isOn: $draft.trustedPolicy)
                    .accessibilityIdentifier("plugin-signature-policy-trust")
                Button(draft.request == nil ? "核对目录包签名" : "正在核对…", action: verify)
                    .disabled(controller == nil || !draft.canVerify)
                    .accessibilityIdentifier("plugin-signature-verify")
                if let receipt = draft.receipt {
                    Text("签名已核对：\(receipt.publisher) · 渠道 \(receipt.channel)（本次策略快照）")
                        .accessibilityIdentifier("plugin-signature-verified")
                    Text("密钥 SHA-256：\(receipt.keyID)\n策略 SHA-256：\(receipt.policySHA256)\n目录包 SHA-256：\(receipt.packageSHA256)")
                        .font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                }
                if let issue = draft.issue {
                    Text(issue).foregroundStyle(.red).accessibilityIdentifier("plugin-signature-issue")
                }
                Text("仅核对已审查目录的签名归属；未导入或核对 ZIP 内容，未安装。来源批准与本次 Session 授权仍需分别确认。策略新鲜度与撤销范围以所选快照为准。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            }
            }
        }
    }

    private func verify() {
        guard let controller, let token = draft.begin() else { return }
        let descriptor = URL(fileURLWithPath: draft.descriptorPath)
        let signature = URL(fileURLWithPath: draft.signaturePath)
        let policy = URL(fileURLWithPath: draft.policyPath)
        Task { @MainActor in
            let result: Result<PluginDirectorySignature, Error>
            do {
                result = .success(try await controller.verifyDirectorySignature(packageID: review.id,
                    digest: review.digest, descriptor: descriptor, signature: signature, trustedPolicy: policy))
            } catch { result = .failure(error) }
            draft.finish(token, result: result)
        }
    }
}
