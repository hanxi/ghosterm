import Darwin
import Foundation

/// A read-only policy observation followed by an explicit, disable-only CAS.
/// Neither the observation nor its accepted rows authorize source or execution.
final class PluginPolicyReview {
    typealias View = ExtensionController.PolicyReview
    let value: View
    private let policyURL: URL
    private let rootIdentity: PluginReleaseRemoval.Stamp

    init(owner: PluginReleaseCatalog, policyURL: URL,
         inspect: (Int32) throws -> PluginDirectorySignatureVerifier.Identity) throws {
        let policy = try PluginSignatureFile.read(policyURL, limit: 131072)
        let parsed = try PluginDirectorySignatureVerifier.policy(policy)
        let selection = try owner.selection()
        self.policyURL = policyURL
        rootIdentity = try Self.rootStamp(owner)
        var entries: [View.Entry] = []
        for id in selection.choices.keys.sorted() {
            guard let choice = selection.choices[id], choice.enabled else { continue }
            let state: View.State, reason: String
            do {
                let handle = try owner.openVersion(choice.releaseID)
                let descriptor = try handle.read("release.json", limit: 65536)
                guard PluginReleaseCatalog.sha(descriptor) == choice.releaseID else { throw PluginReleaseCatalog.Failure.changed }
                let signed = try PluginDirectorySignatureVerifier.authenticate(descriptor: descriptor,
                    signature: handle.read("release.sig", limit: 64), trustedPolicy: policy)
                let payload = try handle.payload(); defer { Darwin.close(payload) }
                let identity = try inspect(payload)
                _ = try signed.bind(identity)
                guard identity.id == id else { throw PluginDirectorySignatureVerifier.Failure.identity }
                try owner.validateVersion(choice.releaseID, handle: handle, payload: payload)
                state = .accepted; reason = "此策略快照接受签名和当前包内容；未授予来源或运行权限。"
            } catch {
                switch error as? PluginDirectorySignatureVerifier.Failure {
                case .revokedKey, .unknownKey, .scope, .signature, .identity:
                    state = .rejected; reason = error.localizedDescription
                default:
                    state = .unknown; reason = "无法完整读取或核验此选择；不能判断为已撤销或仍受信任。"
                }
            }
            entries.append(.init(pluginID: id, releaseID: choice.releaseID, state: state, reason: reason))
        }
        try owner.validate()
        guard try owner.selection().revision == selection.revision else { throw PluginReleaseCatalog.Failure.conflict }
        value = View(id: UUID().uuidString, revision: selection.revision, channel: parsed.channel,
            policySHA256: PluginReleaseCatalog.sha(policy), entries: entries)
    }
    func apply(owner: PluginReleaseCatalog, pluginIDs: [String]) throws -> (PluginReleaseCatalog.Selection, Bool) {
        guard !pluginIDs.isEmpty, Set(pluginIDs).count == pluginIDs.count,
              pluginIDs.allSatisfy({ id in value.entries.contains { $0.pluginID == id && $0.state != .accepted } }) else { throw PluginReleaseCatalog.Failure.invalid }
        guard rootIdentity.sameObject(try Self.rootStamp(owner)) else { throw PluginReleaseCatalog.Failure.changed }
        let policy = try PluginSignatureFile.read(policyURL, limit: 131072)
        guard PluginReleaseCatalog.sha(policy) == value.policySHA256 else { throw PluginReleaseCatalog.Failure.changed }
        var selection = try owner.selection()
        guard selection.revision == value.revision else { throw PluginReleaseCatalog.Failure.conflict }
        for id in pluginIDs {
            guard let entry = value.entries.first(where: { $0.pluginID == id }),
                  let choice = selection.choices[id], choice.enabled, choice.releaseID == entry.releaseID else { throw PluginReleaseCatalog.Failure.conflict }
            selection.choices[id] = .init(releaseID: choice.releaseID, enabled: false, generation: UUID().uuidString)
        }
        return try owner.writeSelection(selection, expected: value.revision)
    }
    private static func rootStamp(_ owner: PluginReleaseCatalog) throws -> PluginReleaseRemoval.Stamp {
        let fd = try owner.journalRoot(); defer { Darwin.close(fd) }
        var value = stat()
        guard fstat(fd, &value) == 0 else { throw PluginReleaseCatalog.Failure.io }
        return PluginReleaseRemoval.Stamp(value)
    }
}
