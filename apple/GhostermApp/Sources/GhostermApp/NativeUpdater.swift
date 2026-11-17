// Sparkle owns archive authentication, staging, replacement and relaunch.
// The driver only presents state; it never terminates the app on its own.
import AppKit
import GhostermAppModel
import Sparkle

@MainActor
final class NativeUpdater: NSObject, SPUUserDriver, SPUUpdaterDelegate {
    static let shared = NativeUpdater(model: .shared)
    let model: UpdateChecker
    private var updater: SPUUpdater?
    private var expected: RemoteBuild?
    private var readyReply: ((SPUUserUpdateChoice) -> Void)?

    init(model: UpdateChecker) {
        self.model = model
        super.init()
    }

    func configure(bundle: Bundle = .main) {
        guard updater == nil,
              let key = bundle.object(forInfoDictionaryKey: "SUPublicEDKey") as? String,
              Data(base64Encoded: key)?.count == 32 else { return }
        let engine = SPUUpdater(hostBundle: bundle, applicationBundle: bundle,
                                userDriver: self, delegate: self)
        do {
            try engine.start()
            updater = engine
            model.downloadAction = { [weak self] build in self?.download(build) }
        } catch {
            // No UI/network at launch. Report the setup failure only on request.
            model.downloadAction = { [weak model] _ in
                model?.phase = .failed("无法启动更新：\(error.localizedDescription)")
            }
        }
    }

    private func download(_ build: RemoteBuild) {
        guard let updater, updater.canCheckForUpdates else {
            fail("更新引擎仍在处理上一次请求，请稍后重新检查。")
            return
        }
        guard build.updateVersion != nil else {
            model.phase = .failed("发布缺少更新序号，请重新检查或手动下载。")
            return
        }
        expected = build
        model.phase = .downloading(received: 0, total: nil)
        updater.checkForUpdates()
    }

    func feedURLString(for updater: SPUUpdater) -> String? {
        expected.map { UpdateCheck.feedURL(channel: $0.channel).absoluteString }
    }

    func updaterShouldPromptForPermissionToCheck(forUpdates updater: SPUUpdater) -> Bool { false }
    func updater(_ updater: SPUUpdater, shouldDownloadReleaseNotesForUpdate updateItem: SUAppcastItem) -> Bool { false }

    func show(_ request: SPUUpdatePermissionRequest,
              reply: @escaping (SUUpdatePermissionResponse) -> Void) {
        reply(SUUpdatePermissionResponse(automaticUpdateChecks: false, sendSystemProfile: false))
    }

    func showUserInitiatedUpdateCheck(cancellation: @escaping () -> Void) {
        setCancellation(cancellation)
    }

    func showUpdateFound(with appcastItem: SUAppcastItem, state: SPUUserUpdateState,
                         reply: @escaping (SPUUserUpdateChoice) -> Void) {
        // The rolling dev release may change between manifest and appcast fetches.
        // Never download another build under the identity the user approved.
        guard let expected, appcastItem.versionString == expected.updateVersion,
              appcastItem.displayVersionString == expected.version,
              appcastItem.fileURL?.absoluteString.hasPrefix(expected.downloadBase + "/") == true else {
            reply(.dismiss)
            fail("发布内容已变化，请重新检查更新。")
            return
        }
        reply(.install) // The user already pressed Download; this starts only the download.
    }

    func showUpdateReleaseNotes(with downloadData: SPUDownloadData) {}
    func showUpdateReleaseNotesFailedToDownloadWithError(_ error: Error) {}

    func showUpdateNotFoundWithError(_ error: Error, acknowledgement: @escaping () -> Void) {
        fail("更新包暂不可用：\(error.localizedDescription)")
        acknowledgement()
    }

    func showUpdaterError(_ error: Error, acknowledgement: @escaping () -> Void) {
        fail(error.localizedDescription)
        acknowledgement()
    }

    func showDownloadInitiated(cancellation: @escaping () -> Void) {
        model.phase = .downloading(received: 0, total: nil)
        setCancellation(cancellation)
    }

    private func setCancellation(_ cancellation: @escaping () -> Void) {
        model.cancelAction = { [weak self] in
            cancellation()
            self?.model.phase = .idle
            self?.model.cancelAction = nil
        }
    }

    func showDownloadDidReceiveExpectedContentLength(_ expectedContentLength: UInt64) {
        guard case .downloading(let received, _) = model.phase else { return }
        model.phase = .downloading(received: received, total: expectedContentLength > 0 ? expectedContentLength : nil)
    }

    func showDownloadDidReceiveData(ofLength length: UInt64) {
        guard case .downloading(let received, let total) = model.phase else { return }
        model.phase = .downloading(received: received + length, total: total)
    }

    func showDownloadDidStartExtractingUpdate() {
        model.cancelAction = nil
        model.phase = .extracting(0)
    }

    func showExtractionReceivedProgress(_ progress: Double) { model.phase = .extracting(progress) }

    func showReady(toInstallAndRelaunch reply: @escaping (SPUUserUpdateChoice) -> Void) {
        readyReply = reply
        model.phase = .ready
        model.restartAction = { [weak self] in
            // Sparkle asks NSApplication to terminate, preserving the existing
            // terminal/transfer/plugin shutdown confirmation, including Cancel.
            self?.readyReply?(.install)
            self?.readyReply = nil
        }
    }

    func showInstallingUpdate(withApplicationTerminated applicationTerminated: Bool,
                             retryTerminatingApplication: @escaping () -> Void) {
        model.phase = .ready
        model.restartAction = retryTerminatingApplication
    }

    func showUpdateInstalledAndRelaunched(_ relaunched: Bool, acknowledgement: @escaping () -> Void) {
        model.phase = .idle
        acknowledgement()
    }

    func dismissUpdateInstallation() {
        readyReply = nil
        model.restartAction = nil
        model.cancelAction = nil
        if case .failed = model.phase { return }
        model.phase = .idle
    }

    private func fail(_ detail: String) {
        readyReply = nil
        model.cancelAction = nil
        model.restartAction = nil
        model.phase = .failed(detail)
        model.noticeVisible = true
    }
}
