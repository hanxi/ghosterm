import Darwin
import Foundation

/// No selection write or source registration. The candidate shared reference
/// remains pinned until this one review is cancelled, committed or closed.
final class PluginSelectionReview {
    enum Failure: Error, LocalizedError {
        case confirmation
        var errorDescription: String? { "请先核对候选声明与旧版本状态，并明确确认本次活动选择切换。" }
    }
    typealias View = ExtensionController.SelectionReview
    let value: View
    let policyURL: URL
    private let rootIdentity: PluginReleaseRemoval.Stamp
    private let candidate: PluginReleaseCatalog.VersionHandle

    init(owner: PluginReleaseCatalog, releaseID: String, expectedRevision: String?, policyURL: URL,
         inspect: (Int32) throws -> ExtensionController.VersionDeclarations) throws {
        guard try !owner.hasPendingRemoval() else { throw PluginReleaseRemoval.Failure.pending }
        let selection = try owner.selection()
        guard selection.revision == expectedRevision else { throw PluginReleaseCatalog.Failure.conflict }
        let policy = try PluginSignatureFile.read(policyURL, limit: 131072)
        rootIdentity = try Self.rootStamp(owner); self.policyURL = policyURL
        let opened = try owner.openVersion(releaseID); try opened.pin(); candidate = opened
        let (next, proof) = try Self.read(owner, handle: opened, policy: policy, inspect: inspect)
        let choice = selection.choices[next.review.id]
        let previous: ExtensionController.VersionDeclarations?
        let state: View.PreviousState, issue: String?
        if let choice {
            if choice.releaseID == releaseID { previous = next; state = .verified; issue = nil }
            else {
                do {
                    let handle = try owner.openVersion(choice.releaseID)
                    let (content, _) = try Self.read(owner, handle: handle, policy: policy, inspect: inspect)
                    guard content.review.id == next.review.id else { throw PluginReleaseCatalog.Failure.invalid }
                    previous = content; state = .verified; issue = nil
                } catch {
                    previous = nil; state = .unknown
                    issue = "无法用本次策略完整核验旧版；不能把旧声明当作空，也不能判断没有新增权限。"
                }
            }
        } else { previous = nil; state = .none; issue = nil }
        try owner.validate()
        guard try owner.selection().revision == selection.revision else { throw PluginReleaseCatalog.Failure.conflict }
        value = View(id: UUID().uuidString, revision: selection.revision, candidateReleaseID: releaseID,
            candidate: next, signature: proof, previousReleaseID: choice?.releaseID, previousEnabled: choice?.enabled,
            previous: previous, previousState: state, previousIssue: issue)
    }
    func validate(owner: PluginReleaseCatalog) throws {
        guard rootIdentity.sameObject(try Self.rootStamp(owner)) else { throw PluginReleaseCatalog.Failure.changed }
        guard try owner.selection().revision == value.revision else { throw PluginReleaseCatalog.Failure.conflict }
        let named = try owner.openVersion(value.candidateReleaseID)
        guard try Self.stamp(named.fd).sameObject(Self.stamp(candidate.fd)) else { throw PluginReleaseCatalog.Failure.changed }
    }
    private static func read(_ owner: PluginReleaseCatalog, handle: PluginReleaseCatalog.VersionHandle, policy: Data,
                             inspect: (Int32) throws -> ExtensionController.VersionDeclarations) throws -> (ExtensionController.VersionDeclarations, PluginDirectorySignature) {
        let descriptor = try handle.read("release.json", limit: 65536)
        guard PluginReleaseCatalog.sha(descriptor) == handle.id else { throw PluginReleaseCatalog.Failure.changed }
        let signatureBytes = try handle.read("release.sig", limit: 64)
        let authenticated = try PluginDirectorySignatureVerifier.authenticate(descriptor: descriptor, signature: signatureBytes, trustedPolicy: policy)
        let payload = try handle.payload(); defer { Darwin.close(payload) }
        let content = try inspect(payload), review = content.review
        let proof = try authenticated.bind(.init(id: review.id, version: review.version, publisher: review.publisher, packageSHA256: review.digest))
        try owner.validateVersion(handle.id, handle: handle, payload: payload)
        return (content, proof)
    }
    private static func stamp(_ fd: Int32) throws -> PluginReleaseRemoval.Stamp {
        var value = stat(); guard fstat(fd, &value) == 0 else { throw PluginReleaseCatalog.Failure.io }
        return PluginReleaseRemoval.Stamp(value)
    }
    private static func rootStamp(_ owner: PluginReleaseCatalog) throws -> PluginReleaseRemoval.Stamp {
        let fd = try owner.journalRoot(); defer { Darwin.close(fd) }; return try stamp(fd)
    }
}
