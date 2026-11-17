// Disposable, windowless app for the real Sparkle installation smoke test.
// Never linked into Ghosterm. Feed and receipts are restricted by the runner.
import AppKit
import Sparkle

func receipt(_ value: String) {
    let path = (ProcessInfo.processInfo.environment["GHOSTERM_UPDATE_RECEIPT"] ?? Bundle.main.object(forInfoDictionaryKey: "GhostermUpdateReceipt") as? String)!
    let data = (value + "\n").data(using: .utf8)!
    if !FileManager.default.fileExists(atPath: path) { FileManager.default.createFile(atPath: path, contents: nil) }
    let file = try! FileHandle(forWritingTo: URL(fileURLWithPath: path))
    try! file.seekToEnd(); try! file.write(contentsOf: data); try! file.close()
}

final class Driver: NSObject, SPUUserDriver, SPUUpdaterDelegate {
    var engine: SPUUpdater!
    var quitCount = 0
    func start() {
        engine = SPUUpdater(hostBundle: .main, applicationBundle: .main, userDriver: self, delegate: self)
        do { try engine.start(); engine.checkForUpdates() }
        catch { receipt("ERROR start \(error)"); exit(1) }
    }
    func updaterShouldPromptForPermissionToCheck(forUpdates updater: SPUUpdater) -> Bool { false }
    func show(_ request: SPUUpdatePermissionRequest, reply: @escaping (SUUpdatePermissionResponse) -> Void) {
        reply(SUUpdatePermissionResponse(automaticUpdateChecks: false, sendSystemProfile: false))
    }
    func showUserInitiatedUpdateCheck(cancellation: @escaping () -> Void) { receipt("checking") }
    func showUpdateFound(with appcastItem: SUAppcastItem, state: SPUUserUpdateState, reply: @escaping (SPUUserUpdateChoice) -> Void) {
        receipt("found \(appcastItem.versionString)"); reply(.install)
    }
    func showUpdateReleaseNotes(with downloadData: SPUDownloadData) {}
    func showUpdateReleaseNotesFailedToDownloadWithError(_ error: Error) {}
    func showUpdateNotFoundWithError(_ error: Error, acknowledgement: @escaping () -> Void) {
        receipt("ERROR no-update \(error)"); acknowledgement(); exit(2)
    }
    func showUpdaterError(_ error: Error, acknowledgement: @escaping () -> Void) {
        receipt("ERROR \(error)"); acknowledgement(); exit(3)
    }
    func showDownloadInitiated(cancellation: @escaping () -> Void) { receipt("downloading") }
    func showDownloadDidReceiveExpectedContentLength(_ expectedContentLength: UInt64) {}
    func showDownloadDidReceiveData(ofLength length: UInt64) {}
    func showDownloadDidStartExtractingUpdate() { receipt("extracting") }
    func showExtractionReceivedProgress(_ progress: Double) {}
    func showReady(toInstallAndRelaunch reply: @escaping (SPUUserUpdateChoice) -> Void) {
        receipt("ready")
        if (ProcessInfo.processInfo.environment["GHOSTERM_UPDATE_MODE"] ?? Bundle.main.object(forInfoDictionaryKey: "GhostermUpdateMode") as? String) == "later" {
            receipt("later-kept-old"); return
        }
        reply(.install)
    }
    func showInstallingUpdate(withApplicationTerminated applicationTerminated: Bool, retryTerminatingApplication: @escaping () -> Void) {
        receipt("installing")
        // Only this owned smoke app auto-retries; production waits for the user.
        if (ProcessInfo.processInfo.environment["GHOSTERM_UPDATE_MODE"] ?? Bundle.main.object(forInfoDictionaryKey: "GhostermUpdateMode") as? String) == "cancel-quit" {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { retryTerminatingApplication() }
        }
    }
    func showUpdateInstalledAndRelaunched(_ relaunched: Bool, acknowledgement: @escaping () -> Void) { acknowledgement() }
    func dismissUpdateInstallation() {}
}

final class Delegate: NSObject, NSApplicationDelegate {
    let driver = Driver()
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.prohibited)
        if Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String == "200" {
            receipt("relaunched-200"); exit(0)
        }
        driver.start()
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        driver.quitCount += 1
        if (ProcessInfo.processInfo.environment["GHOSTERM_UPDATE_MODE"] ?? Bundle.main.object(forInfoDictionaryKey: "GhostermUpdateMode") as? String) == "cancel-quit", driver.quitCount == 1 {
            receipt("quit-cancelled-kept-old"); return .terminateCancel
        }
        receipt("quit-approved"); return .terminateNow
    }
}
let app = NSApplication.shared
let delegate = Delegate()
app.delegate = delegate
app.setActivationPolicy(.prohibited)
app.run()
