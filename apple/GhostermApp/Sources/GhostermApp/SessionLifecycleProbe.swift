// Diagnostic GUI process entry. Uses the default application session routes,
// a real RootView and an explicit private profile; the driver owns SIGKILL.
import AppKit
import Foundation
import GhostermAppModel
import GhostermTerminal

@MainActor enum SessionLifecycleProbe {
    private struct Configuration: Decodable {
        let config, knownHosts, records, runtimeRoot, installRoot, remoteRuntime, alias, status: String
        let localShell: Bool?
    }
    private static var window: NSWindow?
    private static var model: AppModel?
    private static var timer: Timer?
    static func run(configuration path: String, restore: Bool) {
        do {
            let configuration = try JSONDecoder().decode(Configuration.self, from: Data(contentsOf: URL(fileURLWithPath: path)))
            let model = AppModel(configPath: configuration.config, knownHostsPath: configuration.knownHosts,
                sessionDirectory: URL(fileURLWithPath: configuration.records), sessionRuntimeRoot: URL(fileURLWithPath: configuration.runtimeRoot),
                remoteInstallRoot: configuration.installRoot, remoteRuntimeDirectory: configuration.remoteRuntime)
            model.interaction = AlertInteraction(model: model)
            let host = makeRootHostingView(model: model)
            let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 600), styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
            // 自动化探针：不进 Dock、不抢焦点、不上桌面（布局/抓帧照常）。
            NSApplication.shared.setActivationPolicy(.accessory)
            window.contentView = host
            window.setFrameOrigin(NSPoint(x: -30_000, y: -30_000))
            window.makeKeyAndOrderFront(nil)
            self.window = window; self.model = model
            if restore { model.startInitialSessions() } else { model.connect(to: configuration.localShell == true ? .localShell : .host(configuration.alias)) }
            var sent = false, stopping = false, pruning = false
            // 恢复这一轮要恢复的那条记录本来就是从存储上读出来的，所以它的 id
            // 既是「刚才确实活着」的证据，也是「停止之后必须消失」的判据。
            var durableID: UUID?
            let prefix = "GHOSTERM-GUI-" + UUID().uuidString + "-PID:"
            let status = URL(fileURLWithPath: configuration.status)
            let records = URL(fileURLWithPath: configuration.records)
            timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { _ in
                MainActor.assumeIsolated {
                    do {
                        if FileManager.default.fileExists(atPath: configuration.status + ".stop"), !stopping {
                            stopping = true; model.disconnectForTesting()
                        }
                        if stopping {
                            guard let durableID, !pruning else { return }
                            // 已结束的会话打不开：清单与存储都不许留着它。存储的
                            // 读有自己的 owner 队列断言，只能离开主线程查。
                            pruning = true
                            let owner = DispatchQueue(label: "dev.ghosterm.lifecycle-probe.records")
                            owner.async {
                                let rows = ((try? SessionRecordStore(directory: records, owner: owner).load()) ?? [])
                                    .contains(where: { $0.id == durableID })
                                DispatchQueue.main.async {
                                    MainActor.assumeIsolated {
                                        if rows || model.savedSessions.contains(where: { $0.id == durableID }) {
                                            pruning = false    // 终止回执还没落地，下一轮再查
                                            return
                                        }
                                        do {
                                            try publish(["state": "ended"], to: status)
                                            NSApp.terminate(nil)
                                        } catch { fputs("session GUI probe: \(error)\n", stderr) }
                                    }
                                }
                            }
                            return
                        }
                        if model.activePane?.status.isFailed == true || model.sessionError != nil {
                            try publish(["state": "failed", "error": model.sessionError ?? String(describing: model.status)], to: status)
                            return
                        }
                        guard model.activePane?.status == .ready, let surface = model.surface, surface.view.window === window else { return }
                        if !sent {
                            sent = true
                            surface.sendCommittedText("cd /usr; printf '\(prefix)%s\\n' \"$$\"\r")
                        }
                        guard let pid = surface.readText()?.components(separatedBy: .newlines).compactMap({ line -> Int32? in
                            let text = line.trimmingCharacters(in: .whitespaces)
                            guard text.hasPrefix(prefix) else { return nil }
                            return Int32(text.dropFirst(prefix.count))
                        }).first, let record = model.activePane?.sessionRecord, record.session > 0, model.activePane?.currentDirectory == "/usr" else { return }
                        try publish(["state": "ready", "pid": pid, "session": record.session,
                            "instance": record.instance, "cwd": "/usr", "visible": true], to: status)
                        durableID = record.id
                    } catch { fputs("session GUI probe: \(error)\n", stderr) }
                }
            }
        } catch { fputs("session GUI probe: \(error)\n", stderr); exit(1) }
    }
    private static func publish(_ value: [String: Any], to url: URL) throws {
        let data = try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
        try data.write(to: url, options: [.atomic]); try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
    }
}
