import AppKit
import CryptoKit
import Darwin
import GhostermCore
import ImageIO
import SwiftUI
import WebKit
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

/// Actual production controls from package selection callback through owned SSH
/// Preview and stop. The system chooser is a fixed test-only return value.
@MainActor
final class PluginPaneFlowTests: XCTestCase {
    private func until(_ check: () -> Bool) async throws -> Bool {
        let deadline = ProcessInfo.processInfo.systemUptime + 10
        while !check(), ProcessInfo.processInfo.systemUptime < deadline {
            try await Task.sleep(for: .milliseconds(25))
        }
        return check()
    }

    func testPreviewThroughPackageApprovalLaunchConsentAndStopControls() async throws {
        try await runPreview(.markdown, jump: false)
    }
    func testForegroundInspectionAndAttachmentThroughControls() async throws {
        try await runPreview(.markdown, jump: false, foreground: true)
    }
    func testProxyJumpForegroundInspectionAndAttachmentThroughControls() async throws {
        try await runPreview(.markdown, jump: true, foreground: true)
    }
    func testDiscoveredForegroundInspectionAndAttachmentThroughControls() async throws {
        try await runPreview(.markdown, jump: false, foreground: true, discovery: true)
    }
    func testMarkdownAttachmentsThroughProxyJumpControls() async throws {
        try await runPreview(.markdown, jump: true)
    }
    func testHTMLAttachmentsThroughDirectControls() async throws {
        try await runPreview(.html, jump: false)
    }
    func testHTMLAttachmentsThroughProxyJumpControls() async throws {
        try await runPreview(.html, jump: true)
    }
    func testUnicodeHTMLAttachmentsThroughDirectControls() async throws {
        try await runPreview(.htmlUnicode, jump: false)
    }
    func testUnicodeHTMLAttachmentsThroughProxyJumpControls() async throws {
        try await runPreview(.htmlUnicode, jump: true)
    }
    func testAnimatedHTMLAttachmentsThroughDirectControls() async throws {
        try await runPreview(.htmlAnimated, jump: false)
    }
    func testAnimatedHTMLAttachmentsThroughProxyJumpControls() async throws {
        try await runPreview(.htmlAnimated, jump: true)
    }
    func testImageThroughDirectControls() async throws {
        try await runPreview(.image, jump: false)
    }
    func testImageThroughProxyJumpControls() async throws {
        try await runPreview(.image, jump: true)
    }
    func testRunningPreviewSurvivesVersionSelectionAndDisable() async throws {
        guard ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] != nil else { throw XCTSkip("requires signed version fixtures") }
        try await runPreview(.markdown, jump: false, versionChanges: true)
    }
    func testManagedPreviewRetainsVersionUntilStopped() async throws {
        guard ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] != nil else { throw XCTSkip("requires signed version fixtures") }
        try await runPreview(.markdown, jump: false, versionRemoval: true)
    }

    func testManagedPreviewSurvivesPolicyReviewAndExplicitDisable() async throws {
        guard ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] != nil else { throw XCTSkip("requires signed version fixtures") }
        try await runPreview(.markdown, jump: false, versionRemoval: true, versionPolicy: true)
    }

    func testManagedPreviewSurvivesSelectionReviewAndCommit() async throws {
        guard ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] != nil else { throw XCTSkip("requires signed version fixtures") }
        try await runPreview(.markdown, jump: false, versionRemoval: true, versionSelectionReview: true)
    }

    func testManagedPreviewReportsChannelRevocationUntilStopped() async throws {
        guard ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] != nil else { throw XCTSkip("requires signed version fixtures") }
        try await runPreview(.markdown, jump: false, versionRemoval: true, channelRevocation: true)
    }

    func testManagedPreviewReportsChannelRevocationUntilStoppedThroughProxyJump() async throws {
        guard ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] != nil else { throw XCTSkip("requires signed version fixtures") }
        try await runPreview(.markdown, jump: true, versionRemoval: true, channelRevocation: true)
    }

    func testManagedPreviewRejectsChannelRevocationBeforeConsent() async throws {
        guard ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] != nil else { throw XCTSkip("requires signed version fixtures") }
        try await runPreview(.markdown, jump: false, versionRemoval: true, channelRevocation: true, revokeBeforeConsent: true)
    }

    private enum Format { case markdown, html, htmlUnicode, htmlAnimated, image }

    private func runPreview(_ format: Format, jump: Bool, versionChanges: Bool = false, versionRemoval: Bool = false, versionPolicy: Bool = false, versionSelectionReview: Bool = false, foreground: Bool = false, discovery: Bool = false, channelRevocation: Bool = false, revokeBeforeConsent: Bool = false) async throws {
        let env = ProcessInfo.processInfo.environment
        guard ExtensionRuntime.supportStatus == 0, let gterm = env["GHOSTERM_TEST_GTERM"],
              let host = env["GHOSTERM_SSH_HOST"], let port = env["GHOSTERM_SSH_PORT"],
              let key = env["GHOSTERM_SSH_KEY"], let user = env["GHOSTERM_SSH_USER"] else {
            throw XCTSkip("requires JS-enabled Core and the explicitly owned gate SSH fixture")
        }
        if jump && (env["GHOSTERM_SSH_JUMP_HOST"] == nil || env["GHOSTERM_SSH_JUMP_PORT"] == nil) {
            throw XCTSkip("requires explicitly requested owned forwarding-only jump fixture")
        }
        @Sendable func trace(_ step: String) {
            guard env["GHOSTERM_TEST_PLUGIN_UI_TRACE"] == "1" else { return }
            FileHandle.standardError.write(Data("plugin-ui-step: \(step)\n".utf8))
        }
        trace("enter automation mode")
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        trace("enable accessibility")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer {
            trace("restore accessibility")
            NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced)
            trace("accessibility restored")
        }
        let repository = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let package = repository.appendingPathComponent("plugins/official/preview")
        let id = "org.ghosterm.preview"
        let root = repository.appendingPathComponent("tmp/pane-" + UUID().uuidString.prefix(8))
        let workspace = root.appendingPathComponent("workspace")
        try FileManager.default.createDirectory(at: workspace.appendingPathComponent("site"), withIntermediateDirectories: true)
        addTeardownBlock {
            trace("remove fixture tree")
            try? FileManager.default.removeItem(at: root)
            trace("fixture tree removed")
        }
        let png = try redPNG()
        let markdown = Data("# Preview controls\n\n![red sample](image.png)\n\nOwned SSH pane flow token\n".utf8)
        let unicodeHTML = format == .htmlUnicode
        let animatedHTML = format == .htmlAnimated
        let directory = unicodeHTML ? "site/cafe\u{301}" : "site"
        let htmlPath = unicodeHTML ? directory + "/页面 #?%.html" : "site/index.html"
        let cssPath = unicodeHTML ? directory + "/主 题%.css" : "site/style.css"
        let imagePath = unicodeHTML ? directory + "/图😀.png" : animatedHTML ? "site/animation.gif" : "site/image.png"
        let cssLink = unicodeHTML ? "%E4%B8%BB%20%E9%A2%98%25.css" : "style.css"
        let imageLink = unicodeHTML ? "%E5%9B%BE%F0%9F%98%80.png" : animatedHTML ? "animation.gif" : "image.png"
        let html = Data("<html><head><link rel='stylesheet' href='\(cssLink)'></head><body><img width='64' height='64' src='\(imageLink)'></body></html>".utf8)
        let css = Data("body { margin: 0; background: rgb(0, 0, 255); }".utf8)
        let raster = animatedHTML ? try animatedGIF() : png
        let files = ["site/index.md": markdown, htmlPath: html, cssPath: css, imagePath: raster]
        for (path, bytes) in files {
            let location = workspace.appendingPathComponent(path)
            try FileManager.default.createDirectory(at: location.deletingLastPathComponent(), withIntermediateDirectories: true)
            try bytes.write(to: location)
        }
        let path: String
        let arguments: [String]
        switch format {
        case .markdown: path = "site/index.md"; arguments = [path, "--assets", "site/image.png"]
        case .html, .htmlUnicode, .htmlAnimated: path = htmlPath; arguments = [path, "--assets", cssPath + "," + imagePath]
        case .image: path = "site/image.png"; arguments = [path]
        }
        let document = try XCTUnwrap(files[path])
        let argumentsJSON = String(decoding: try JSONSerialization.data(withJSONObject: arguments), as: UTF8.self)
        let count = (format == .html || unicodeHTML || animatedHTML) ? 3 : format == .markdown ? 2 : 1
        let config = root.appendingPathComponent("config"), known = root.appendingPathComponent("known")
        var source = "Host pane-target\n HostName \(host)\n Port \(port)\n User \(user)\n IdentityFile \(key)\n"
        if jump {
            source += " ProxyJump pane-jump\nHost pane-jump\n HostName \(env["GHOSTERM_SSH_JUMP_HOST"]!)\n Port \(env["GHOSTERM_SSH_JUMP_PORT"]!)\n User \(user)\n IdentityFile \(key)\n"
        }
        try Data(source.utf8).write(to: config)
        try Data().write(to: known)
        let channelKey = channelRevocation ? Curve25519.Signing.PrivateKey() : nil
        let channelDirectory = root.appendingPathComponent("channel")
        let channelName: String?
        let initialChannelPolicy: Data?
        if channelRevocation {
            let fixture = URL(fileURLWithPath: env["GHOSTERM_TEST_SIGNED_RELEASE"]!)
            let bytes = try Data(contentsOf: fixture.appendingPathComponent("live/policy.json"))
            let parsed = try XCTUnwrap(JSONSerialization.jsonObject(with: bytes) as? [String: Any])
            channelName = try XCTUnwrap(parsed["channel"] as? String)
            initialChannelPolicy = bytes
            try FileManager.default.createDirectory(at: channelDirectory, withIntermediateDirectories: false,
                                                    attributes: [.posixPermissions: 0o700])
        } else {
            channelName = nil
            initialChannelPolicy = nil
        }
        func signChannel(_ generation: UInt64, policy: Data) throws -> (Data, Data) {
            let key = try XCTUnwrap(channelKey), channel = try XCTUnwrap(channelName)
            let now = UInt64(Date().timeIntervalSince1970)
            let digest = SHA256.hash(data: policy).map { String(format: "%02x", $0) }.joined()
            let statement = try JSONSerialization.data(withJSONObject: [
                "schemaVersion": 1, "channel": channel, "generation": String(generation),
                "issuedAt": String(now), "expiresAt": String(now + 600), "policySHA256": digest,
            ])
            let signature = try key.signature(for: Data("ghosterm.channel-policy.v1\0".utf8) + statement)
            return (statement, signature)
        }
        func refreshChannel(_ generation: UInt64, policy: Data) throws -> PluginDirectorySignatureVerifier.VerifiedChannelPolicy {
            let (statement, signature) = try signChannel(generation, policy: policy)
            let key = try XCTUnwrap(channelKey), channel = try XCTUnwrap(channelName)
            let now = UInt64(Date().timeIntervalSince1970)
            return try PluginDirectorySignatureVerifier.refreshChannelPolicy(statement: statement, signature: signature,
                policy: policy, channel: channel, channelRoot: key.publicKey.rawRepresentation, now: now,
                stateDirectory: channelDirectory)
        }
        let trust = channelKey.map { key in
            ExtensionController.ChannelTrust(channel: channelName!, root: key.publicKey.rawRepresentation, stateDirectory: channelDirectory)
        }
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: known.path,
                                     pluginResourceDirectory: root.appendingPathComponent("received"), pluginChannelTrust: trust)
        let controller = try XCTUnwrap(model.extensionController)
        addTeardownBlock {
            trace("teardown stops Runtime")
            await model.stopExtensionRun()
            trace("teardown closes controller")
            let closed = await controller.close()
            XCTAssertTrue(closed)
            trace("controller teardown completed")
        }
        let interaction = CodexRouteInteraction()
        model.interaction = interaction
        var selections = 0
        trace("create hosting view")
        let pane = NSHostingView(rootView: PluginWorkspaceTestView(model: model, packagePicker: { completion in
            selections += 1
            if selections == 1 { completion(.failure(CocoaError(.userCancelled))) }
            else {
                completion(.success([package]))
                completion(.failure(CocoaError(.fileReadNoSuchFile)))
            }
        }))
        let window = PluginTableWindow(contentRect: NSRect(x: 0, y: 0, width: 1440, height: 1400),
                                       styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = pane
        trace("present offscreen window")
        TestWindows.presentOffscreen(window)
        defer {
            trace("order window out")
            window.orderOut(nil)
            trace("detach hosting view")
            window.contentView = nil
            trace("hosting view detached")
        }
        func nodes() -> [NSObject] { pane.layoutSubtreeIfNeeded(); return PluginTableRendering.elements(pane) }
        func node(_ identifier: String) -> NSObject? {
            nodes().first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == identifier }
        }
        func press(_ identifier: String) throws {
            trace("press \(identifier)")
            let control = try XCTUnwrap(node(identifier), identifier)
            XCTAssertTrue(PluginTableRendering.press(control), identifier)
        }
        func set(_ identifier: String, _ value: String) throws {
            trace("edit \(identifier)")
            let control = try XCTUnwrap(node(identifier), identifier)
            let selector = NSSelectorFromString("setAccessibilityValue:")
            guard control.responds(to: selector) else { throw GhostermError.failed("not editable: \(identifier)") }
            control.perform(selector, with: value)
            let field = try XCTUnwrap((control as? NSTextField) ?? (control as? NSCell)?.controlView as? NSTextField, "Native editable field: \(type(of: control))")
            NotificationCenter.default.post(name: NSControl.textDidChangeNotification, object: field)
        }
        func scrollBottom() throws {
            func scrollViews(_ view: NSView) -> [NSScrollView] {
                (view as? NSScrollView).map { [$0] } ?? view.subviews.flatMap(scrollViews)
            }
            pane.layoutSubtreeIfNeeded()
            let scroll = try XCTUnwrap(scrollViews(pane).first)
            let clip = scroll.contentView
            let documentView = try XCTUnwrap(scroll.documentView)
            clip.scroll(to: NSPoint(x: 0, y: documentView.isFlipped ? max(0, documentView.bounds.height - clip.bounds.height) : 0))
            scroll.reflectScrolledClipView(clip)
        }
        func hasText(_ value: String) -> Bool {
            nodes().contains { node in
                ["accessibilityValue", "accessibilityLabel"].contains {
                    PluginTableRendering.attribute(node, $0)?.contains(value) == true
                }
            }
        }
        trace("locate package import control")
        guard try await until({ node("plugin-package-import") != nil }) else { XCTFail("import control missing"); return }
        XCTAssertNil(model.extensionRun)
        try press("plugin-package-import")
        XCTAssertEqual(selections, 1)
        XCTAssertNil(node("plugin-package-issue"), "cancelling the picker must not display an import error")
        XCTAssertNil(node("plugin-approve-" + id))
        XCTAssertNil(model.extensionRun)
        try press("plugin-package-import")
        guard try await until({ node("plugin-approve-" + id) != nil }) else { XCTFail("directory selection callback did not review selected package"); return }
        XCTAssertEqual(selections, 2)
        XCTAssertNil(node("plugin-package-issue"), "a second picker callback must not overwrite an active review")
        let digest = SHA256.hash(data: try Data(contentsOf: package.appendingPathComponent("integrity.json")))
            .map { String(format: "%02x", $0) }.joined()
        XCTAssertTrue(hasText(digest), "review must display the exact immutable package digest")
        XCTAssertTrue(hasText("未认证"))
        XCTAssertNil(model.extensionRun, "review must not activate the plugin")
        XCTAssertNil(node("plugin-launch-" + id), "source decision must precede the launch form")
        var managed: ExtensionController.InstalledRelease?
        if versionRemoval {
            let fixture = URL(fileURLWithPath: env["GHOSTERM_TEST_SIGNED_RELEASE"]!).appendingPathComponent("live")
            let prepared: ExtensionController.PreparedRelease
            if let initialChannelPolicy {
                _ = try refreshChannel(1, policy: initialChannelPolicy)
                prepared = try await controller.prepareReleaseUsingConfiguredChannel(artifact: fixture.appendingPathComponent("r/plugin.zip"),
                    descriptor: fixture.appendingPathComponent("r/release.json"), signature: fixture.appendingPathComponent("r/release.sig"))
            } else {
                prepared = try await controller.prepareSignedRelease(artifact: fixture.appendingPathComponent("r/plugin.zip"),
                    descriptor: fixture.appendingPathComponent("r/release.json"), signature: fixture.appendingPathComponent("r/release.sig"), trustedPolicy: fixture.appendingPathComponent("policy.json"))
            }
            let installed = try await controller.installPreparedRelease(id: prepared.id)
            XCTAssertEqual(installed.review.digest, digest, "real managed Session must use exactly the reviewed live bytes")
            _ = try await controller.selectInstalledVersion(releaseID: installed.signature.descriptorSHA256, expectedRevision: nil,
                trustedPolicy: fixture.appendingPathComponent("policy.json"))
            managed = installed
        }
        try press("plugin-approve-" + id)
        guard try await until({ node("plugin-launch-" + id) != nil }) else { XCTFail("approval did not reveal launch form"); return }
        for (name, value) in [
            ("ssh-alias", "pane-target"), ("executable", gterm),
            ("executor-directory", package.path), ("executor-digest", digest),
            ("arguments", argumentsJSON), ("resource-root", workspace.path),
        ] {
            try set("plugin-" + name + "-" + id, value)
            try await Task.sleep(for: .milliseconds(25))
        }
        if versionChanges || versionRemoval {
            try set("plugin-client-workspace", workspace.path)
            try await Task.sleep(for: .milliseconds(25))
            try press("plugin-client-workspace-allow")
            try await Task.sleep(for: .milliseconds(25))
        }
        var frontend: Process?
        var discoveredCandidate: FrontendCandidate?
        if discovery { addTeardownBlock { model.disconnectForTesting() } }
        if foreground {
            guard host == "127.0.0.1", user == NSUserName() else { throw XCTSkip("foreground fixture requires owned same-user loopback SSH") }
            let endpoint = root.appendingPathComponent("e")
            try FileManager.default.createDirectory(at: endpoint, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
            let process = Process(); process.executableURL = URL(fileURLWithPath: gterm)
            process.arguments = ["relay", "--listen", endpoint.path, "--trust-development-digest", digest,
                "--client-digest", digest, "--root", "workspace", workspace.path, "--", package.path, "preview"] + arguments
            process.standardInput = FileHandle.nullDevice; process.standardOutput = Pipe(); process.standardError = Pipe()
            if discovery {
                model.connect(to: .localShell)
                guard try await until({ model.activePane?.isRunning == true }) else { XCTFail("owned local PTY did not start"); return }
                let nested = #"\033P\033]777;ghosterm-discovery;1;EREREREREREREREREREREaqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqAAsvd29yay9mcm9udA==\007\033\0134"#
                model.activePane?.surface?.sendCommittedText("printf '%b%s%s\\n' '\(nested)' 'PTY_' 'CONTROL_MARKER'\r")
                guard try await until({ model.activePane?.surface?.readText()?.contains("PTY_CONTROL_MARKER") == true }) else {
                    XCTFail("ordinary PTY output after the control string did not reach the terminal grid"); return
                }
                func quote(_ value: String) -> String { "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'" }
                let command = ([gterm] + (process.arguments ?? [])).map(quote).joined(separator: " ")
                model.activePane?.surface?.sendCommittedText("GTERM_DISCOVERY_TMUX_LAYERS=0 " + command + "\r")
            } else {
                try process.run(); frontend = process
                addTeardownBlock {
                    if process.isRunning { process.terminate() }
                    let deadline = ProcessInfo.processInfo.systemUptime + 5
                    while process.isRunning, ProcessInfo.processInfo.systemUptime < deadline { try await Task.sleep(for: .milliseconds(10)) }
                    XCTAssertFalse(process.isRunning, "owned foreground fixture must exit")
                }
            }
            let permitURL = endpoint.appendingPathComponent("permit")
            guard try await until({ (try? Data(contentsOf: permitURL).count) == 56 }) else { XCTFail("foreground endpoint did not become ready"); return }
            let permit = try Data(contentsOf: permitURL)
            let instance = permit[8..<24].map { String(format: "%02x", $0) }.joined()
            if discovery {
                guard try await until({ model.frontendCandidates.contains { $0.offer.instance == instance } }) else { XCTFail("real PTY hint did not reach native candidate list"); return }
                let candidate = try XCTUnwrap(model.frontendCandidates.first { $0.offer.instance == instance })
                XCTAssertFalse(model.frontendCandidates.contains { $0.offer.instance == String(repeating: "1", count: 32) },
                               "a discovery sequence inside a terminal control string must not create a candidate")
                discoveredCandidate = candidate
                XCTAssertEqual(candidate.offer.directory, endpoint.path)
                XCTAssertEqual(candidate.offer.clientDigest, digest)
                XCTAssertTrue(model.frontendCandidateIsCurrent(candidate))
                XCTAssertNil(model.extensionRun, "passive hint must not create a Session")
                XCTAssertFalse(model.extensionRunBusy, "passive hint must not authenticate")
                try press("plugin-discovery-use-" + candidate.id.uuidString)
            } else {
                try press("plugin-ipc-mode")
                try await Task.sleep(for: .milliseconds(30))
                try set("plugin-ipc-directory", endpoint.path)
                try set("plugin-ipc-instance", instance)
            }
            try await Task.sleep(for: .milliseconds(30))
            try press("plugin-ipc-inspect")
            guard try await until({ node("plugin-ipc-declaration") != nil && !model.extensionRunBusy }) else {
                let issue = node("plugin-ipc-inspection-issue").flatMap { PluginTableRendering.attribute($0, "accessibilityValue") }
                XCTFail("inspection did not display live declaration: \(issue ?? model.extensionRunIssue ?? "none")"); return
            }
            XCTAssertNil(model.extensionRun, "inspection must not create a Session or Runtime")
            XCTAssertEqual(try Data(contentsOf: permitURL), permit, "inspection must leave attach capability intact")
            try press("plugin-ipc-adopt")
            try await Task.sleep(for: .milliseconds(30))
            XCTAssertEqual(PluginTableRendering.attribute(try XCTUnwrap(node("plugin-arguments-" + id)), "accessibilityValue"), argumentsJSON)
            let allow = try XCTUnwrap(node("plugin-ipc-allow"))
            let allowed = allow.perform(NSSelectorFromString("accessibilityValue"))?.takeUnretainedValue() as? NSNumber
            XCTAssertEqual(allowed?.intValue, 0, "adopting declaration must clear attachment approval")
            try press("plugin-ipc-files")
            try await Task.sleep(for: .milliseconds(30))
            try press("plugin-ipc-allow")
            try await Task.sleep(for: .milliseconds(30))
        }
        XCTAssertNil(model.extensionRun, "editing the launch form must not connect")
        try scrollBottom()
        try await Task.sleep(for: .milliseconds(50))
        try press("plugin-launch-" + id)
        guard try await until({ node("plugin-consent-allow") != nil && !model.extensionRunBusy }) else {
            XCTFail("launch control did not reach consent: \(model.extensionRunIssue ?? "none")"); return
        }
        XCTAssertEqual(model.extensionRun?.session?.phase, ExtensionSession.Phase.consent.rawValue)
        XCTAssertNil(model.extensionRun?.runtime, "source approval must not grant runtime consent")
        XCTAssertEqual(model.extensionRun?.arguments, arguments)
        XCTAssertEqual(interaction.hosts.map(\.port), jump ? [UInt16(env["GHOSTERM_SSH_JUMP_PORT"]!)!, UInt16(port)!] : [UInt16(port)!],
                       "actual host-key prompts must prove the configured route and order")
        if foreground {
            XCTAssertTrue(model.extensionRun?.remoteCommand.contains("'--attach'") == true)
        } else {
            XCTAssertTrue(model.extensionRun?.remoteCommand.contains("'--root' 'workspace' '\(workspace.path)'") == true)
        }
        try scrollBottom()
        try await Task.sleep(for: .milliseconds(50))
        if revokeBeforeConsent {
            var policy = try XCTUnwrap(JSONSerialization.jsonObject(with: XCTUnwrap(initialChannelPolicy)) as? [String: Any])
            policy["revokedKeyIds"] = [try XCTUnwrap(managed).signature.keyID]
            _ = try refreshChannel(2, policy: JSONSerialization.data(withJSONObject: policy))
            try press("plugin-consent-allow")
            guard try await until({ !model.extensionRunBusy && model.extensionRunIssue != nil }) else {
                XCTFail("allow did not surface the revoked publisher"); return
            }
            XCTAssertEqual(model.extensionRun?.session?.phase, ExtensionSession.Phase.consent.rawValue,
                           "revoked consent must not advance the Native Session")
            XCTAssertNil(model.extensionRun?.runtime)
            XCTAssertTrue(model.extensionRun?.receivedResources.isEmpty == true)
            await model.stopExtensionRun()
            XCTAssertNil(model.extensionRun)
            return
        }
        try press("plugin-consent-allow")
        guard try await until({ model.extensionRun?.receivedResources.count == count && model.extensionRun?.receivedResources.first?.bytes == document }) else {
            XCTFail("Preview did not receive exact owned SSH resource: \(model.extensionRunIssue ?? "none")"); return
        }
        XCTAssertNil(model.extensionRunIssue)
        let resources = try XCTUnwrap(model.extensionRun?.receivedResources)
        XCTAssertEqual(resources.count, count)
        if unicodeHTML {
            XCTAssertEqual(Set(resources.map { Data($0.displayName.utf8) }),
                           Set([htmlPath, cssPath, imagePath].map { Data($0.utf8) }),
                           "Unicode SSH receipts must preserve exact path bytes")
        }
        for item in resources {
            XCTAssertEqual(item.bytes, files[item.displayName], "every received attachment must match the owned source")
            XCTAssertEqual(item.sha256, SHA256.hash(data: item.bytes).map { String(format: "%02x", $0) }.joined())
        }
        XCTAssertEqual(model.extensionRun?.runtime?.phase, ExtensionRuntime.Phase.active.rawValue)
        if let initialChannelPolicy, let managed {
            let settings = ConversationUIProbe(root: NSHostingView(rootView: ScrollView {
                ExtensionsPane(model: model)
            }))
            defer { settings.close() }
            func settingsNode(_ identifier: String) -> NSObject? {
                settings.root.layoutSubtreeIfNeeded()
                return PluginTableRendering.elements(settings.root).first {
                    PluginTableRendering.attribute($0, "accessibilityIdentifier") == identifier
                }
            }
            func settingsText(_ identifier: String, contains text: String) -> Bool {
                guard let node = settingsNode(identifier) else { return false }
                return ["accessibilityValue", "accessibilityLabel"].contains {
                    PluginTableRendering.attribute(node, $0)?.contains(text) == true
                }
            }
            guard try await until({ settingsNode("plugin-installed-channel-cache") != nil }) else {
                XCTFail("settings did not show the channel cache control"); return
            }
            XCTAssertNil(settingsNode("plugin-installed-channel-affected"))
            var fields = try XCTUnwrap(JSONSerialization.jsonObject(with: initialChannelPolicy) as? [String: Any])
            fields["revokedKeyIds"] = [managed.signature.keyID]
            let policy = try JSONSerialization.data(withJSONObject: fields)
            let (statement, signature) = try signChannel(2, policy: policy)
            let child = Process()
            child.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
            child.arguments = ["xctest", "-XCTest", "GhostermAppTests.PluginChannelCacheProcessTests/testCommitOwnedChannelPolicy",
                               Bundle(for: PluginChannelCacheProcessTests.self).bundleURL.path]
            var childEnvironment = env.filter { ["PATH", "DEVELOPER_DIR"].contains($0.key) }
            childEnvironment["TMPDIR"] = root.path
            childEnvironment["GHOSTERM_AUTOMATION_OFFSCREEN"] = "1"
            childEnvironment["GHOSTERM_CHANNEL_CACHE_FIXTURE"] = channelDirectory.path
            childEnvironment["GHOSTERM_CHANNEL_NAME"] = try XCTUnwrap(channelName)
            childEnvironment["GHOSTERM_CHANNEL_PUBLIC_ROOT"] = try XCTUnwrap(channelKey).publicKey.rawRepresentation.base64EncodedString()
            childEnvironment["GHOSTERM_CHANNEL_STATEMENT"] = statement.base64EncodedString()
            childEnvironment["GHOSTERM_CHANNEL_SIGNATURE"] = signature.base64EncodedString()
            childEnvironment["GHOSTERM_CHANNEL_POLICY"] = policy.base64EncodedString()
            child.environment = childEnvironment
            try child.run()
            defer { if child.isRunning { kill(child.processIdentifier, SIGKILL); child.waitUntilExit() } }
            guard try await until({ !child.isRunning }) else { XCTFail("channel cache writer did not exit"); return }
            XCTAssertEqual(child.terminationStatus, 0)
            let revoked = try await controller.cachedChannelPolicy()
            XCTAssertEqual(revoked.generation, 2)
            let affected = try await controller.runningInstancesWithRevokedPublisher(in: revoked)
            XCTAssertEqual(affected, 1)
            let staleDraft = PluginInstalledVersionsDraft()
            await staleDraft.readChannelPolicy(controller, refresh: false, automatic: true)
            XCTAssertEqual(staleDraft.affectedChannelInstances, 1)
            let retained = try await controller.stepRuntime()
            XCTAssertEqual(retained.phase, ExtensionRuntime.Phase.active.rawValue)
            XCTAssertEqual(window.alphaValue, 0)
            XCTAssertFalse(NSScreen.screens.contains { $0.frame.intersects(window.frame) })
            let releaseID = try XCTUnwrap(managed).signature.descriptorSHA256
            let beforeStop = try await controller.installedVersions()
            let retainedVersion = try XCTUnwrap(beforeStop.versions.first { $0.releaseID == releaseID })
            XCTAssertEqual(retainedVersion.inUse, true, "revocation observation retains the running version lease")
            guard try await until({ settingsNode("plugin-installed-channel-stop") != nil }) else {
                XCTFail("open settings did not observe the cross-process revocation"); return
            }
            XCTAssertNil(settingsNode("plugin-installed-refresh"), "installed versions must remain collapsed")
            XCTAssertNotNil(settingsNode("plugin-installed-channel-affected"))
            XCTAssertEqual(settings.window.alphaValue, 0)
            XCTAssertFalse(NSScreen.screens.contains { $0.frame.intersects(settings.window.frame) })
            do {
                let lock = open(channelDirectory.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                guard lock >= 0 else { XCTFail("cannot open owned channel cache"); return }
                defer { Darwin.close(lock) }
                guard flock(lock, LOCK_EX | LOCK_NB) == 0 else { XCTFail("cannot lock owned channel cache"); return }
                XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(settingsNode("plugin-installed-channel-stop"))))
                guard try await until({
                    settingsText("plugin-installed-channel-notice", contains: "已确认停止 0 个实例。") &&
                    settingsText("plugin-installed-channel-error", contains: "安装目录正由另一项操作使用")
                }) else { XCTFail("settings did not show the channel cache lock failure"); return }
                let stillActive = try await controller.stepRuntime()
                XCTAssertEqual(stillActive.phase, ExtensionRuntime.Phase.active.rawValue)
                let catalog = try await controller.installedVersions()
                XCTAssertTrue(catalog.versions.contains { $0.releaseID == releaseID && $0.inUse == true })
            }
            guard try await until({ settingsNode("plugin-installed-channel-stop") != nil }) else {
                XCTFail("settings did not restore the stop control after unlocking"); return
            }
            let stop = try XCTUnwrap(settingsNode("plugin-installed-channel-stop"))
            XCTAssertTrue(PluginTableRendering.press(stop))
            guard try await until({
                let confirmed = settingsText("plugin-installed-channel-notice", contains: "已确认停止 1 个实例。")
                return confirmed && settingsNode("plugin-installed-channel-affected") == nil &&
                    model.extensionRun == nil && !model.extensionRunBusy
            }) else {
                XCTFail("settings did not confirm stop and clear the affected count"); return
            }
            XCTAssertNil(settingsNode("plugin-installed-channel-error"))
            let after = try await controller.runningInstancesWithRevokedPublisher(in: revoked)
            XCTAssertEqual(after, 0)
            let afterStop = try await controller.installedVersions()
            let releasedVersion = try XCTUnwrap(afterStop.versions.first { $0.releaseID == releaseID })
            XCTAssertEqual(releasedVersion.inUse, false, "the stopped Runtime must release its version lease")
            await staleDraft.stopAffectedChannelInstances(controller) {
                try await model.stopInstancesWithRevokedPublisher()
            }
            XCTAssertEqual(staleDraft.channelNotice, "已确认停止 0 个实例。")
            XCTAssertNil(staleDraft.channelIssue)
            XCTAssertEqual(staleDraft.affectedChannelInstances, 0, "recheck clears the stale count even when no stop was needed")
            return
        }
        if versionChanges || versionRemoval {
            let fixture = URL(fileURLWithPath: env["GHOSTERM_TEST_SIGNED_RELEASE"]!)
            let original = try await controller.stepRuntime()
            let installed: ExtensionController.InstalledRelease
            var revision: String?
            if let managed {
                installed = managed; revision = try await controller.installedVersions().revision
            } else {
                let prepared = try await controller.prepareSignedRelease(artifact: fixture.appendingPathComponent("r/plugin.zip"),
                    descriptor: fixture.appendingPathComponent("r/release.json"), signature: fixture.appendingPathComponent("r/release.sig"), trustedPolicy: fixture.appendingPathComponent("policy.json"))
                installed = try await controller.installPreparedRelease(id: prepared.id)
                let selected = try await controller.selectInstalledVersion(releaseID: installed.signature.descriptorSHA256,
                    expectedRevision: nil, trustedPolicy: fixture.appendingPathComponent("policy.json"))
                revision = selected.catalog.revision
            }
            if versionSelectionReview {
                let input = fixture.appendingPathComponent("upgrade")
                let prepared = try await controller.prepareSignedRelease(artifact: input.appendingPathComponent("r/plugin.zip"), descriptor: input.appendingPathComponent("r/release.json"),
                    signature: input.appendingPathComponent("r/release.sig"), trustedPolicy: input.appendingPathComponent("policy.json"))
                let candidate = try await controller.installPreparedRelease(id: prepared.id)
                let before = try await controller.installedVersions()
                let view = try await controller.previewInstalledSelection(releaseID: candidate.signature.descriptorSHA256, expectedRevision: before.revision, trustedPolicy: input.appendingPathComponent("policy.json"))
                XCTAssertEqual(view.previousState, .verified)
                let observed = try await controller.installedVersions(); XCTAssertEqual(observed.revision, before.revision)
                let selected = try await controller.confirmInstalledSelection(id: view.id, acknowledged: true)
                revision = selected.catalog.revision
                XCTAssertEqual(selected.catalog.selections.first?.releaseID, candidate.signature.descriptorSHA256)
            }
            let retained: ExtensionController.RuntimeState
            do { retained = try await controller.stepRuntime() }
            catch { XCTFail("version selection must retain the running Runtime: \(error)"); return }
            XCTAssertEqual(retained.phase, ExtensionRuntime.Phase.active.rawValue, "version selection must retain the running Runtime")
            XCTAssertEqual(retained.packageDigest, original.packageDigest)
            let disabledChoice: ExtensionController.VersionSelectionResult
            if versionPolicy {
                let policyURL = root.appendingPathComponent("revoked-policy.json")
                var policy = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: fixture.appendingPathComponent("live/policy.json"))) as? [String: Any])
                policy["revokedKeyIds"] = [installed.signature.keyID]
                try JSONSerialization.data(withJSONObject: policy, options: [.sortedKeys]).write(to: policyURL)
                let review = try await controller.previewInstalledPolicy(trustedPolicy: policyURL)
                XCTAssertEqual(review.entries.first { $0.pluginID == installed.review.id }?.state, .rejected)
                let stillActive = try await controller.stepRuntime(); XCTAssertEqual(stillActive.phase, ExtensionRuntime.Phase.active.rawValue)
                disabledChoice = try await controller.disableReviewedPlugins(id: review.id, pluginIDs: [installed.review.id]).selection
            } else {
                disabledChoice = try await controller.disableInstalledPlugin(pluginID: installed.review.id, expectedRevision: revision)
            }
            if versionRemoval {
                let observer = try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: ["portable-js"], capabilities: ["resource.receive"]),
                    resourceDirectory: root, installationDirectory: installed.directory.deletingLastPathComponent().deletingLastPathComponent())
                do {
                    let unexpected = try await observer.previewVersionRemoval(releaseID: installed.signature.descriptorSHA256, expectedRevision: disabledChoice.catalog.revision)
                    await observer.cancelVersionRemoval(id: unexpected.id)
                    XCTFail("actual managed Runtime reference must block another Controller's deletion")
                } catch { XCTAssertEqual(error as? PluginReleaseRemoval.Failure, .occupied) }
                let closed = await observer.close(); XCTAssertTrue(closed)
            }
            let disabled: ExtensionController.RuntimeState
            do { disabled = try await controller.stepRuntime() }
            catch { XCTFail("disable must retain the running Runtime: \(error)"); return }
            XCTAssertEqual(disabled.phase, ExtensionRuntime.Phase.active.rawValue, "disable must retain the running Runtime")
            XCTAssertEqual(disabled.packageDigest, original.packageDigest)
            do {
                _ = try await controller.publishClientResource(instanceID: disabled.instanceID, relativePath: "site/index.md")
                let deadline = ProcessInfo.processInfo.systemUptime + 10
                var receipt = try await controller.stepRuntime()
                while receipt.publication?.confirmed != true, ProcessInfo.processInfo.systemUptime < deadline {
                    try await Task.sleep(for: .milliseconds(25)); receipt = try await controller.stepRuntime()
                }
                XCTAssertTrue(receipt.publication?.confirmed == true, "old Runtime must still publish and receive a real peer receipt after disable")
                XCTAssertEqual(receipt.publication?.sha256, SHA256.hash(data: markdown).map { String(format: "%02x", $0) }.joined())
            } catch { XCTFail("old Runtime business transport failed after disable: \(error)"); return }
            XCTAssertEqual(model.extensionRun?.receivedResources.map(\.sha256), resources.map(\.sha256))
            let denied = try await controller.resolve(command: "preview", selectedID: original.packageID, selectedDigest: original.packageDigest)
            XCTAssertNotEqual(denied.status, 0, "disabled selection must block a new admission")
        }
        try scrollBottom()
        try await Task.sleep(for: .milliseconds(100))
        var retainedHTML: [WKWebView] = []
        switch format {
        case .markdown:
            var words = ""
            guard try await until({
                // Async image decode changes the document height. Follow the
                // latest layout instead of leaving text below an old viewport.
                try? scrollBottom()
                words = (try? PluginTableRendering.words(pane).joined(separator: " ")) ?? ""
                return words.lowercased().contains("owned ssh pane flow token")
            }) else { XCTFail("production pane did not draw received Markdown; OCR: \(words)"); return }
            guard try await until({ try? scrollBottom(); return panePixels(pane).red > 20 }) else { XCTFail("Markdown attachment did not draw actual red pixels"); return }
        case .image:
            guard try await until({ try? scrollBottom(); return panePixels(pane).red > 20 }) else { XCTFail("standalone image did not draw actual red pixels"); return }
        case .html, .htmlUnicode, .htmlAnimated:
            guard try await until({ !webViews(pane).isEmpty }) else { XCTFail("embedded HTML WebKit view missing"); return }
            let originalWeb = try XCTUnwrap(webViews(pane).first)
            guard !originalWeb.configuration.websiteDataStore.isPersistent else {
                XCTFail("embedded HTML must use nonpersistent storage"); return
            }
            guard !originalWeb.configuration.defaultWebpagePreferences.allowsContentJavaScript else {
                XCTFail("embedded HTML must disable JavaScript"); return
            }
            if animatedHTML {
                let handler = try XCTUnwrap(originalWeb.configuration.urlSchemeHandler(forURLScheme: "ghosterm-resource") as? PluginResourceHTMLSchemeHandler)
                let request = HTMLReadProbe(request: URLRequest(url: try XCTUnwrap(PluginResourceHTMLAssetResolver.documentURL(imagePath))))
                handler.webView(originalWeb, start: request)
                let original = try XCTUnwrap(CGImageSourceCreateWithData(raster as CFData, nil))
                let displayed = request.data.isEmpty ? nil : CGImageSourceCreateWithData(request.data as CFData, nil)
                XCTAssertTrue(CGImageSourceGetCount(original) == 2 && request.mime == "image/png" &&
                              displayed.map { CGImageSourceGetCount($0) == 1 } == true,
                              "actual SSH animation must reach WebKit only as one static PNG frame")
            }
            retainedHTML.append(originalWeb)
            let rendered = try await htmlPixels(pane)
            guard rendered.red > 20 && rendered.blue > 100 else {
                XCTFail("embedded HTML did not draw received CSS and PNG: \(rendered)"); return
            }
            let image = try XCTUnwrap(resources.first { $0.displayName.utf8.elementsEqual(imagePath.utf8) })
            try scrollBottom()
            try press("plugin-resource-select-" + image.resourceID)
            guard try await until({ node("plugin-resource-preview-" + image.resourceID) != nil && webViews(pane).isEmpty }) else {
                XCTFail("resource selection did not replace embedded HTML with the selected image"); return
            }
            guard try await until({ htmlClosed(originalWeb, path: imagePath) }) else {
                XCTFail("removed HTML must detach delegates and revoke attachment reads even while retained"); return
            }
            guard try await until({ try? scrollBottom(); return panePixels(pane).red > 20 }) else { XCTFail("selected HTML attachment did not draw actual red pixels"); return }
            let main = try XCTUnwrap(resources.first { $0.displayName.utf8.elementsEqual(path.utf8) })
            try scrollBottom()
            try press("plugin-resource-select-" + main.resourceID)
            guard try await until({ !webViews(pane).isEmpty }) else { XCTFail("returning to HTML did not rebuild embedded WebKit"); return }
            let returnedWeb = try XCTUnwrap(webViews(pane).first)
            guard !returnedWeb.configuration.websiteDataStore.isPersistent &&
                  returnedWeb.configuration.websiteDataStore !== originalWeb.configuration.websiteDataStore else {
                XCTFail("reopened HTML must use a fresh nonpersistent store"); return
            }
            retainedHTML.append(returnedWeb)
            let reopened = try await htmlPixels(pane)
            XCTAssertGreaterThan(reopened.red, 20); XCTAssertGreaterThan(reopened.blue, 100)
        }
        XCTAssertEqual(window.alphaValue, 0)
        XCTAssertFalse(NSScreen.screens.contains { $0.frame.intersects(window.frame) })
        try scrollBottom()
        try press("plugin-stop")
        trace("stop action returned")
        guard try await until({ model.extensionRun == nil && !model.extensionRunBusy && webViews(pane).isEmpty }) else { XCTFail("stop control did not release run"); return }
        guard try await until({ retainedHTML.allSatisfy { htmlClosed($0, path: imagePath) } }) else {
            XCTFail("stopped HTML must detach delegates and revoke retained attachment handlers"); return
        }
        trace("runtime and embedded views released")
        XCTAssertNil(model.extensionRunIssue)
        for item in resources { XCTAssertNil(node("plugin-resource-select-" + item.resourceID)) }
        XCTAssertNil(node("plugin-consent-allow"))
        if let frontend {
            guard try await until({ !frontend.isRunning }) else { XCTFail("stopping attached runtime must release original frontend"); return }
        }
        if let candidate = discoveredCandidate {
            model.disconnectForTesting()
            XCTAssertFalse(model.frontendCandidateIsCurrent(candidate), "stopped source cannot be reused")
        }
        if let managed {
            let revision = try await controller.installedVersions().revision
            let preview = try await controller.previewVersionRemoval(releaseID: managed.signature.descriptorSHA256, expectedRevision: revision)
            let removed = try await controller.confirmVersionRemoval(id: preview.id)
            XCTAssertTrue(removed.complete && removed.durabilityConfirmed); XCTAssertNil(removed.cleanupIssue)
            XCTAssertFalse(FileManager.default.fileExists(atPath: managed.directory.path), "normal stop must release the real Session/Runtime version lease")
        }
    }

    private func redPNG() throws -> Data {
        let pixels = Data((0..<64 * 64).flatMap { _ in [UInt8(255), 0, 0, 255] })
        let provider = try XCTUnwrap(CGDataProvider(data: pixels as CFData))
        let image = try XCTUnwrap(CGImage(width: 64, height: 64, bitsPerComponent: 8, bitsPerPixel: 32,
            bytesPerRow: 256, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue), provider: provider,
            decode: nil, shouldInterpolate: false, intent: .defaultIntent))
        let bytes = NSMutableData()
        let destination = try XCTUnwrap(CGImageDestinationCreateWithData(bytes, "public.png" as CFString, 1, nil))
        CGImageDestinationAddImage(destination, image, nil)
        XCTAssertTrue(CGImageDestinationFinalize(destination))
        return bytes as Data
    }

    private func animatedGIF() throws -> Data {
        let output = NSMutableData()
        let destination = try XCTUnwrap(CGImageDestinationCreateWithData(output, "com.compuserve.gif" as CFString, 2, nil))
        for green in [false, true] {
            let pixels = Data((0..<32 * 32).flatMap { _ in
                green ? [UInt8(0), 255, 0, 255] : [UInt8(255), 0, 0, 255]
            })
            let provider = try XCTUnwrap(CGDataProvider(data: pixels as CFData))
            let image = try XCTUnwrap(CGImage(width: 32, height: 32, bitsPerComponent: 8, bitsPerPixel: 32,
                bytesPerRow: 128, space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue), provider: provider,
                decode: nil, shouldInterpolate: false, intent: .defaultIntent))
            CGImageDestinationAddImage(destination, image, nil)
        }
        XCTAssertTrue(CGImageDestinationFinalize(destination))
        return output as Data
    }

    private func webViews(_ view: NSView) -> [WKWebView] {
        (view as? WKWebView).map { [$0] } ?? view.subviews.flatMap(webViews)
    }

    private func panePixels(_ pane: NSView) -> (red: Int, blue: Int) {
        pane.layoutSubtreeIfNeeded()
        guard let bitmap = pane.bitmapImageRepForCachingDisplay(in: pane.bounds) else { return (0, 0) }
        pane.cacheDisplay(in: pane.bounds, to: bitmap)
        return pixels(bitmap)
    }

    private func pixels(_ bitmap: NSBitmapImageRep) -> (red: Int, blue: Int) {
        var red = 0, blue = 0
        for y in stride(from: 0, to: bitmap.pixelsHigh, by: 2) {
            for x in stride(from: 0, to: bitmap.pixelsWide, by: 2) {
                guard let c = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                if c.redComponent > 0.8 && c.greenComponent < 0.3 && c.blueComponent < 0.3 { red += 1 }
                if c.blueComponent > 0.8 && c.redComponent < 0.3 && c.greenComponent < 0.3 { blue += 1 }
            }
        }
        return (red, blue)
    }

    private func htmlPixels(_ pane: NSView) async throws -> (red: Int, blue: Int) {
        let web = try XCTUnwrap(webViews(pane).first)
        let deadline = ProcessInfo.processInfo.systemUptime + 10
        var result = (red: 0, blue: 0)
        repeat {
            if !web.isLoading && web.bounds.width > 0 && web.bounds.height > 0 {
                let image = try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<NSImage, Error>) in
                    web.takeSnapshot(with: nil) { image, error in
                        if let error { continuation.resume(throwing: error) }
                        else if let image { continuation.resume(returning: image) }
                        else { continuation.resume(throwing: GhostermError.failed("embedded WebKit snapshot missing")) }
                    }
                }
                let cg = try XCTUnwrap(image.cgImage(forProposedRect: nil, context: nil, hints: nil))
                result = pixels(NSBitmapImageRep(cgImage: cg))
                if result.red > 20 && result.blue > 100 { return result }
            }
            try await Task.sleep(for: .milliseconds(100))
        } while ProcessInfo.processInfo.systemUptime < deadline
        return result
    }

    private func htmlClosed(_ web: WKWebView, path: String) -> Bool {
        guard web.navigationDelegate == nil, web.uiDelegate == nil,
              let handler = web.configuration.urlSchemeHandler(forURLScheme: "ghosterm-resource") as? PluginResourceHTMLSchemeHandler,
              let url = PluginResourceHTMLAssetResolver.documentURL(path) else { return false }
        let request = HTMLReadProbe(request: URLRequest(url: url))
        handler.webView(web, start: request)
        return request.rejected && !request.delivered
    }

    private final class HTMLReadProbe: NSObject, WKURLSchemeTask {
        let request: URLRequest
        var rejected = false, delivered = false
        var mime: String?
        var data = Data()
        init(request: URLRequest) { self.request = request }
        func didReceive(_ response: URLResponse) { delivered = true; mime = response.mimeType }
        func didReceive(_ data: Data) { delivered = true; self.data.append(data) }
        func didFinish() { delivered = true }
        func didFailWithError(_ error: Error) { rejected = true }
    }
}
