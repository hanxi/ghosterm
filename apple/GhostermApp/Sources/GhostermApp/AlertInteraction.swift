// AlertInteraction — 模态对话框（主机密钥三态 / 私钥口令）。
//
// 铁律 #6 的 UI 落点：主机密钥是**三态**（信任 / 拒绝 / 暂不连接），
// 不是二态确认框——「未知」与「拒绝」是不同的事。
import AppKit
import GhostermAppModel
import GhostermCore

// MARK: - 对话框（真三态：未知 ≠ 拒绝）

/// 主机密钥确认对话框的 UI 实现。模态：工作线程在 semaphore 上等结果。
final class AlertInteraction: AppInteraction {
    private weak var model: AppModel?

    init(model: AppModel) { self.model = model }

    func confirmHostKey(_ prompt: HostKeyPrompt) -> HostKeyDecision {
        let alert = NSAlert()
        alert.messageText = "无法确认主机身份"
        alert.informativeText = """
        \(prompt.host):\(prompt.port) 的主机密钥是新的（首次连接或密钥已变更）。

        类型：\(prompt.keyType)
        指纹：\(prompt.fingerprint)

        信任并连接后，指纹将写入 ~/.ssh/known_hosts；之后密钥若不一致，\
        连接会被直接拒绝。
        """
        alert.alertStyle = .critical
        alert.addButton(withTitle: "信任并连接")
        alert.addButton(withTitle: "拒绝")
        // 第三态：先看指纹（连不连由用户再决定），不是「拒绝」。
        alert.addButton(withTitle: "暂不连接")

        let response = alert.runModal()
        switch response {
        case .alertFirstButtonReturn: return .accept
        case .alertSecondButtonReturn: return .reject
        default: return .unknown
    }
    }

    /// 凭据对话框。**两种问法分开**（见 CredentialPrompt.Kind）：
    /// 私钥口令 vs 登录密码。混成一句话就是用户最容易被激怒的那个体验
    /// （「我明明输了登录密码，为什么还问」）。
    func askCredential(_ prompt: CredentialPrompt) -> CredentialAnswer? {
        let alert = NSAlert()
        alert.messageText = prompt.title
        alert.informativeText = prompt.detail
        alert.alertStyle = .informational
        alert.addButton(withTitle: prompt.kind == CredentialPrompt.Kind.passphrase
                        ? "解锁" : "登录")
        alert.addButton(withTitle: "取消")

        let field = NSSecureTextField(
            frame: NSRect(x: 0, y: 0, width: 260, height: 24))
        field.placeholderString = prompt.kind == CredentialPrompt.Kind.passphrase
            ? "私钥口令" : "登录密码"
        alert.accessoryView = field
        alert.window.initialFirstResponder = field

        guard alert.runModal() == .alertFirstButtonReturn else { return nil }
        let secret = field.stringValue
        guard !secret.isEmpty else { return nil }
        switch prompt.kind {
        case .passphrase:
            return CredentialAnswer(pem: "", passphrase: secret)
        case .password:
            return CredentialAnswer(pem: "", password: secret)
        }
    }
}

