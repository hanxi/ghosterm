import AppKit
import CryptoKit
import CoreGraphics
import Darwin
import Foundation
import GhostermAppModel
import GhostermCore
import ImageIO
import SwiftUI
import XCTest
import Vision
@testable import GhostermApp

final class ExtensionControllerTests: XCTestCase {
    @MainActor
    func testClientHostProfileOnlyAdvertisesIntegratedCapabilities() {
        XCTAssertEqual(AppModel.extensionHostCapabilities,
                       ["session.rpc", "resource.receive", "file.readScoped"])
    }

    @MainActor
    func testHostProtocolVersionNormalizesAppleMarketingVersion() {
        XCTAssertEqual(AppModel.extensionHostProtocolVersion("16.0"), "16.0.0")
        XCTAssertEqual(AppModel.extensionHostProtocolVersion("1.2.3"), "1.2.3")
        XCTAssertEqual(AppModel.extensionHostProtocolVersion("01.2"), "0.1.0")
        XCTAssertEqual(AppModel.extensionHostProtocolVersion(nil), "0.1.0")
    }

    func testResourceCapabilityRequiresAnExplicitPrivateDirectory() throws {
        let profile = try ExtensionController.HostProfile(side: .client, sdkVersion: "1.0.0",
            protocols: [1], runtimes: ["portable-js"], capabilities: ["session.rpc", "resource.receive"])
        XCTAssertThrowsError(try ExtensionController(profile: profile))
    }

    func testPluginResourceImageDecoderEnforcesFormatAndPixelBudget() throws {
        let pixels = Data(repeating: 0xff, count: 2 * 2 * 4)
        let provider = CGDataProvider(data: pixels as CFData)!
        let sourceImage = CGImage(width: 2, height: 2, bitsPerComponent: 8, bitsPerPixel: 32,
            bytesPerRow: 8, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue), provider: provider,
            decode: nil, shouldInterpolate: false, intent: .defaultIntent)!
        let encoded = NSMutableData()
        let destination = CGImageDestinationCreateWithData(encoded, "public.png" as CFString, 1, nil)!
        CGImageDestinationAddImage(destination, sourceImage, nil)
        XCTAssertTrue(CGImageDestinationFinalize(destination))
        let data = encoded as Data

        let decoded = try XCTUnwrap(PluginResourceImageDecoder.decode(data, mime: "image/png"))
        XCTAssertEqual(decoded.width, 2)
        XCTAssertNil(PluginResourceImageDecoder.decode(data, mime: "image/png", maxPixels: 3))
        XCTAssertNil(PluginResourceImageDecoder.decode(data, mime: "image/svg+xml"))
    }

    func testPluginResourceMarkdownDecoderRequiresMarkdownAndEnforcesBudget() throws {
        let bytes = Data("# Notes\n\n**safe** [external](https://example.invalid)".utf8)
        let decoded = try XCTUnwrap(PluginResourceMarkdownDecoder.decode(
            bytes, mime: "text/markdown; charset=utf-8", displayName: "notes.md"))
        XCTAssertEqual(String(decoded.characters), "Notessafe external")
        XCTAssertTrue(decoded.runs.contains {
            $0.inlinePresentationIntent?.contains(.stronglyEmphasized) == true
        })
        XCTAssertFalse(decoded.runs.contains { $0.link != nil }, "Markdown links must not navigate out of the preview")
        XCTAssertNil(PluginResourceMarkdownDecoder.decode(bytes, mime: "text/html", displayName: "page.html"))
        XCTAssertNil(PluginResourceMarkdownDecoder.decode(Data([0xff]), mime: "text/markdown", displayName: "x.md"))
        XCTAssertNil(PluginResourceMarkdownDecoder.decode(
            Data(repeating: 0x61, count: PluginResourceMarkdownDecoder.maximumBytes + 1),
            mime: "text/markdown", displayName: "large.md"))
    }

    func testPluginMarkdownImageResolvesAgainstItsScopedDocumentBaseURL() throws {
        let source = Data("before ![logo](images/logo.png) after".utf8)
        let baseURL = try XCTUnwrap(URL(string: "ghosterm-resource://preview/site/index.md"))
        let markdown = try XCTUnwrap(PluginResourceMarkdownDecoder.decode(
            source, mime: "text/markdown", displayName: "site/index.md", baseURL: baseURL))
        let imageRun = try XCTUnwrap(markdown.runs.first(where: { $0.imageURL != nil }))
        let imageURL = try XCTUnwrap(imageRun.imageURL?.absoluteURL)
        XCTAssertEqual(imageURL.absoluteString,
                       "ghosterm-resource://preview/site/images/logo.png")
    }

    func testPluginResourceHTMLDecoderCreatesNetworkAndScriptRestrictedDocument() throws {
        let source = Data("<h1>Preview</h1><script>fetch('https://example.invalid')</script>".utf8)
        let document = try XCTUnwrap(PluginResourceHTMLDecoder.document(source, mime: "text/html; charset=utf-8"))
        XCTAssertTrue(document.contains("<h1>Preview</h1>"))
        XCTAssertTrue(document.contains("script-src 'none'"))
        XCTAssertTrue(document.contains("default-src 'none'"))
        XCTAssertTrue(document.contains("style-src 'self'"))
        XCTAssertTrue(document.contains("img-src 'self'"))
        XCTAssertFalse(document.contains("img-src data:"))
        XCTAssertFalse(document.contains("style-src 'unsafe-inline'"))
        XCTAssertTrue(document.contains("connect-src 'none'"))
        XCTAssertTrue(document.contains("frame-src 'none'"))
        XCTAssertNil(PluginResourceHTMLDecoder.document(source, mime: "text/markdown"))
        XCTAssertNil(PluginResourceHTMLDecoder.document(Data([0xff]), mime: "text/html"))
        XCTAssertNil(PluginResourceHTMLDecoder.document(
            Data(repeating: 0x61, count: PluginResourceHTMLDecoder.maximumBytes + 1), mime: "text/html"))
    }

    func testControllerWithoutResourceCapabilityCannotTakeResources() async throws {
        let profile = try ExtensionController.HostProfile(side: .client, sdkVersion: "1.0.0",
            protocols: [1], runtimes: ["portable-js"], capabilities: ["session.rpc"])
        let controller = try ExtensionController(profile: profile)
        do {
            _ = try await controller.takeSessionResource()
            XCTFail("resource access must require the advertised and provisioned capability")
        } catch ExtensionController.ControllerError.resourceUnavailable {
        }
        let closed = await controller.close()
        XCTAssertTrue(closed)
    }

    func testRelayBootstrapValidatesAndQuotesEveryArgument() throws {
        let digest = String(repeating: "a", count: 64)
        let request = ExtensionRelayBootstrap.Request(
            executable: "/opt/gterm", executorDirectory: "/opt/plugins/executor",
            executorDigest: digest, clientDigest: digest, methods: ["fixture.echo"],
            hostMethods: ["host.confirm"], command: "fixture.run",
            arguments: ["space value", "it's quoted"])
        XCTAssertEqual(try ExtensionRelayBootstrap.build(request),
            "exec '/opt/gterm' 'relay' '--stdio' '--trust-development-digest' '\(digest)' " +
            "'--client-digest' '\(digest)' '--method' 'fixture.echo' '--host-method' 'host.confirm' " +
            "'--' '/opt/plugins/executor' 'fixture.run' 'space value' 'it'\\''s quoted'")
        let rooted = ExtensionRelayBootstrap.Request(
            executable: "/opt/gterm", executorDirectory: "/opt/plugins/executor",
            executorDigest: digest, clientDigest: digest, methods: [],
            scopedRoots: [.init(id: "workspace", directory: "/srv/project files")],
            command: "fixture.run")
        XCTAssertEqual(try ExtensionRelayBootstrap.build(rooted),
            "exec '/opt/gterm' 'relay' '--stdio' '--trust-development-digest' '\(digest)' " +
            "'--client-digest' '\(digest)' '--root' 'workspace' '/srv/project files' " +
            "'--' '/opt/plugins/executor' 'fixture.run'")
        XCTAssertEqual(try ExtensionRelayBootstrap.parseArgumentsJSON(#"["a", "b c"]"#), ["a", "b c"])
        XCTAssertThrowsError(try ExtensionRelayBootstrap.parseArgumentsJSON(#"["ok"] false"#))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.parseMethodsJSON(#"["bad..name"]"#))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(.init(
            executable: "/opt/gterm", executorDirectory: "/opt/executor",
            executorDigest: digest, clientDigest: digest, methods: ["same"],
            hostMethods: ["same"], command: "fixture.run")))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(.init(
            executable: "/opt/gterm", executorDirectory: "/opt/executor",
            executorDigest: digest, clientDigest: digest, methods: [],
            scopedRoots: [.init(id: "../workspace", directory: "/srv/project")],
            command: "fixture.run")))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(.init(
            executable: "/opt/gterm", executorDirectory: "/opt/executor",
            executorDigest: digest, clientDigest: digest, methods: [],
            scopedRoots: [.init(id: "workspace", directory: "/srv/a"),
                          .init(id: "workspace", directory: "/srv/b")],
            command: "fixture.run")))
    }

    @MainActor
    func testControllerOwnsSSHChannelAndAuthenticationOnWorker() async throws {
        guard let host = ProcessInfo.processInfo.environment["GHOSTERM_SSH_HOST"],
              let portText = ProcessInfo.processInfo.environment["GHOSTERM_SSH_PORT"],
              let keyPath = ProcessInfo.processInfo.environment["GHOSTERM_SSH_KEY"],
              let knownHosts = ProcessInfo.processInfo.environment["GHOSTERM_SSH_KNOWN_HOSTS"],
              let user = ProcessInfo.processInfo.environment["GHOSTERM_SSH_USER"],
              let port = UInt16(portText) else {
            throw XCTSkip("requires the local sshd fixture from apple-app-gate")
        }
        let address: [UInt8] = [127, 0, 0, 1] + Array(repeating: 0, count: 12)
        let profile = try ExtensionController.HostProfile(side: .client, sdkVersion: "1.0.0",
            protocols: [1], runtimes: ["portable-js"], capabilities: ["session.rpc"])
        let controller = try ExtensionController(profile: profile)
        var finished = false
        let deadline = ProcessInfo.processInfo.systemUptime + 20
        do {
            var state = try await controller.openSSHChannel(hops: [
                .init(host: host, port: port, username: user, knownHostsPath: knownHosts,
                      addressFamily: 4, addressBytes: address),
            ], bootstrap: "exec true")
            while state.phase != ExtensionChannel.Phase.ready.rawValue {
                XCTAssertLessThan(ProcessInfo.processInfo.systemUptime, deadline,
                                  "Controller SSH channel did not become ready: \(state)")
                let index = Int(state.hop)
                if state.authentication.indices.contains(index) {
                    let auth = state.authentication[index]
                    if auth.phase == SSHAuthenticator.Phase.hostkey.rawValue {
                        let accepted = try await controller.decideSSHHost(hop: state.hop,
                            challenge: state.challenge, decision: HostKeyDecision.accept.rawValue)
                        XCTAssertEqual(accepted, 0)
                    } else if auth.phase == SSHAuthenticator.Phase.credentials.rawValue {
                        let pem = try String(contentsOfFile: keyPath, encoding: .utf8)
                        let submitted = try await controller.submitSSHCredential(hop: state.hop,
                            challenge: state.challenge, credential: .privateKey(pem: pem, passphrase: nil))
                        XCTAssertEqual(submitted, 0)
                    } else if auth.phase == SSHAuthenticator.Phase.failed.rawValue {
                        XCTFail("Controller SSH authentication failed: \(auth.error)")
                    }
                }
                state = try await controller.stepSSHChannel(milliseconds: 5)
            }
            XCTAssertEqual(state.phase, ExtensionChannel.Phase.ready.rawValue)
            finished = true
        } catch {
            await controller.close()
            throw error
        }
        XCTAssertTrue(finished)
        var closed = false
        for _ in 0..<4 where !closed {
            closed = await controller.close()
            if !closed { try await Task.sleep(nanoseconds: 10_000_000) }
        }
        XCTAssertTrue(closed, "Controller must close its SSH channel before releasing the worker")
    }

    @MainActor
    func testAppModelAuthenticatesConfiguredPluginSSHRoute() async throws {
        guard let host = ProcessInfo.processInfo.environment["GHOSTERM_SSH_HOST"],
              let port = ProcessInfo.processInfo.environment["GHOSTERM_SSH_PORT"],
              let keyPath = ProcessInfo.processInfo.environment["GHOSTERM_SSH_KEY"],
              let user = ProcessInfo.processInfo.environment["GHOSTERM_SSH_USER"] else {
            throw XCTSkip("requires the local sshd fixture from apple-app-gate")
        }
        let root = sessionTestDirectory().appendingPathComponent("plugin-route-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let config = root.appendingPathComponent("ssh_config")
        let knownHosts = root.appendingPathComponent("known_hosts")
        try Data("""
        Host plugin-fixture
          HostName \(host)
          Port \(port)
          User \(user)
          IdentityFile \(keyPath)
        """.utf8).write(to: config)
        try Data().write(to: knownHosts)

        let model = isolatedAppModel(configPath: config.path, knownHostsPath: knownHosts.path)
        guard let controller = model.extensionController else {
            throw XCTSkip("requires the native plugin channel runtime")
        }
        defer { Task { await controller.close() } }
        model.interaction = AcceptAllInteraction()
        let state = try await model.authenticatePluginSSHChannel(alias: "plugin-fixture", bootstrap: "exec true")
        XCTAssertEqual(state.phase, ExtensionChannel.Phase.ready.rawValue)
        try await controller.stopSSHChannel()
        try await controller.releaseFinished()
        let closed = await controller.close()
        XCTAssertTrue(closed)
    }

    @MainActor
    func testPluginSSHRouteUsesConfiguredProxyJumpAndOnlyResolvesFirstHop() async throws {
        let root = sessionTestDirectory().appendingPathComponent("plugin-proxyjump-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let config = root.appendingPathComponent("ssh_config")
        let knownHosts = root.appendingPathComponent("known_hosts")
        try Data("""
        Host plugin-jump
          HostName 127.0.0.1
          Port 2221
          User jump-user
        Host plugin-target
          HostName 192.0.2.19
          Port 2222
          User target-user
          ProxyJump plugin-jump
        """.utf8).write(to: config)
        try Data().write(to: knownHosts)
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: knownHosts.path)
        let hops = try await model.resolvePluginSSHRoute(alias: "plugin-target")
        XCTAssertEqual(hops.count, 2)
        XCTAssertEqual(hops[0].host, "127.0.0.1")
        XCTAssertEqual(hops[0].port, 2221)
        XCTAssertEqual(hops[0].username, "jump-user")
        XCTAssertEqual(hops[0].addressFamily, 4)
        XCTAssertEqual(hops[0].addressBytes?.prefix(4), [127, 0, 0, 1])
        XCTAssertNil(hops[0].viaHost)
        XCTAssertEqual(hops[1].host, "192.0.2.19")
        XCTAssertEqual(hops[1].port, 2222)
        XCTAssertEqual(hops[1].username, "target-user")
        XCTAssertNil(hops[1].addressFamily)
        XCTAssertEqual(hops[1].viaHost, "192.0.2.19")
    }

    @MainActor
    func testReviewDoesNotApproveAndResolutionRequiresExactApproval() async throws {
        guard ExtensionRuntime.supportStatus == 0 else {
            throw XCTSkip("requires the explicit JS-enabled Core profile")
        }
        let root = sessionTestDirectory().appendingPathComponent("extension-controller-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }

        let manifest: [String: Any] = [
            "schemaVersion": 1,
            "id": "org.ghosterm.controller-fixture",
            "pluginVersion": "0.1.0",
            "sdkApiRange": "^1.0.0",
            "bridgeProtocolRange": [1],
            "publisher": "fixture",
            "license": "MIT",
            "commands": [["path": ["fixture"], "title": "Fixture", "argsSchema": "schemas/input.json"]],
            "client": ["runtime": "portable-js", "entry": "client/index.js", "requires": ["session.rpc", "resource.receive"]],
            "permissions": [],
        ]
        let schema = #"{"type":"object","properties":{"wrap":{"type":"boolean"}},"required":["wrap"],"additionalProperties":false,"x-ghosterm-cli":{"version":1,"fields":{"wrap":{"option":"wrap","default":false}}}}"#
        var files: [String: Data] = [
            "plugin.json": try JSONSerialization.data(withJSONObject: manifest, options: [.sortedKeys]),
            "README": Data("fixture".utf8),
            "LICENSES/MIT.txt": Data("fixture".utf8),
            "schemas/input.json": Data(schema.utf8),
            "client/index.js": Data("export function activate(){return {dispose(){}}}".utf8),
        ]
        func writePackage(_ files: [String: Data], to directory: URL) throws {
            var members: [[String: Any]] = []
            for (path, data) in files {
                let file = directory.appendingPathComponent(path)
                try FileManager.default.createDirectory(at: file.deletingLastPathComponent(),
                                                        withIntermediateDirectories: true)
                try data.write(to: file)
                members.append(["path": path, "bytes": data.count,
                                "sha256": SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()])
            }
            let integrity = try JSONSerialization.data(withJSONObject: ["schemaVersion": 1, "members": members],
                                                       options: [.sortedKeys])
            try integrity.write(to: directory.appendingPathComponent("integrity.json"))
        }
        try writePackage(files, to: root)
        let secondRoot = root.deletingLastPathComponent().appendingPathComponent("extension-controller-second-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: secondRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: secondRoot) }
        var secondManifest = manifest
        secondManifest["id"] = "org.ghosterm.controller-second"
        files["plugin.json"] = try JSONSerialization.data(withJSONObject: secondManifest, options: [.sortedKeys])
        try writePackage(files, to: secondRoot)

        let resourceDirectory = root.appendingPathComponent("resources")
        try FileManager.default.createDirectory(at: resourceDirectory, withIntermediateDirectories: true)
        let profile = try ExtensionController.HostProfile(side: .client, sdkVersion: "1.0.0",
                                                          protocols: [1], runtimes: ["portable-js"],
                                                          capabilities: ["session.rpc", "resource.receive"])
        let controller = try ExtensionController(profile: profile, resourceDirectory: resourceDirectory)
        defer { Task { await controller.close() } }

        let review = try await controller.review(directory: root)
        let secondReview = try await controller.review(directory: secondRoot)
        XCTAssertEqual(review.id, "org.ghosterm.controller-fixture")
        XCTAssertEqual(review.commands.map(\.path), ["fixture"])
        XCTAssertEqual(review.runtime, "portable-js")
        XCTAssertEqual(review.requiredCapabilities, ["session.rpc", "resource.receive"])
        XCTAssertTrue(review.optionalMissingCapabilities.isEmpty)
        XCTAssertFalse(review.authorizationGranted)
        XCTAssertFalse(review.publisherAuthenticated)
        XCTAssertFalse(review.sourceApproved)

        let ambiguous = try await controller.resolve(command: "fixture")
        XCTAssertEqual(ambiguous.status, 7, "An unreviewed overlapping command must preserve conflict")
        let unapproved = try await controller.resolve(command: "fixture", selectedID: review.id,
                                                      selectedDigest: review.digest)
        XCTAssertEqual(unapproved.status, 2, "UNKNOWN source must not resolve as runnable")
        XCTAssertNil(unapproved.command)

        let approved = try await controller.approve(id: review.id, digest: review.digest)
        XCTAssertEqual(approved, 0)
        let decodedArguments = try await controller.decodeArguments(packageID: review.id,
            packageDigest: review.digest, command: "fixture", arguments: [])
        XCTAssertEqual(decodedArguments, #"{"wrap":false}"#)
        do {
            _ = try await controller.decodeArguments(packageID: review.id, packageDigest: review.digest,
                command: "fixture", arguments: ["--unknown"])
            XCTFail("schema-invalid argv must fail before consent")
        } catch { }
        let stillAmbiguous = try await controller.resolve(command: "fixture")
        XCTAssertEqual(stillAmbiguous.status, 7, "Approval must not hide another pending command")
        let selected = try await controller.resolve(command: "fixture", selectedID: review.id,
                                                    selectedDigest: review.digest)
        XCTAssertEqual(selected.status, 0)
        XCTAssertEqual(selected.packageID, review.id)
        XCTAssertEqual(selected.packageDigest, review.digest)
        XCTAssertEqual(selected.command?.path, "fixture")

        let deniedStatus = try await controller.deny(id: review.id, digest: review.digest)
        XCTAssertEqual(deniedStatus, 3)
        let denied = try await controller.resolve(command: "fixture", selectedID: review.id,
                                                  selectedDigest: review.digest)
        XCTAssertEqual(denied.status, 3)
        XCTAssertNil(denied.command)
        let hiddenDenied = try await controller.resolve(command: "fixture")
        XCTAssertEqual(hiddenDenied.status, 2, "Denied command must not shadow the other UNKNOWN candidate")
        XCTAssertEqual(secondReview.commands.map(\.path), ["fixture"])
        await controller.close()
        do {
            _ = try await controller.resolve(command: "fixture")
            XCTFail("closed controller must reject further work")
        } catch { }
    }

    @MainActor
    func testAppModelReceivesScopedResourceFromRemoteGterm() async throws {
        guard ExtensionRuntime.supportStatus == 0,
              let gterm = ProcessInfo.processInfo.environment["GHOSTERM_TEST_GTERM"],
              let host = ProcessInfo.processInfo.environment["GHOSTERM_SSH_HOST"],
              let port = ProcessInfo.processInfo.environment["GHOSTERM_SSH_PORT"],
              let keyPath = ProcessInfo.processInfo.environment["GHOSTERM_SSH_KEY"],
              let user = ProcessInfo.processInfo.environment["GHOSTERM_SSH_USER"] else {
            throw XCTSkip("requires JS-enabled Core and isolated gterm/sshd fixture")
        }

        let base = sessionTestDirectory().appendingPathComponent("gterm-resource-" + UUID().uuidString)
        let repositoryRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let package = repositoryRoot.appendingPathComponent("plugins/official/preview", isDirectory: true)
        let sourceRoot = base.appendingPathComponent("source", isDirectory: true)
        let imageDirectory = sourceRoot.appendingPathComponent("site/images", isDirectory: true)
        let webDirectory = sourceRoot.appendingPathComponent("site/web", isDirectory: true)
        try FileManager.default.createDirectory(at: imageDirectory, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: webDirectory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: base) }

        let markdownBytes = Data("![sample](images/sample.png)\nresource from AppModel over SSH\n".utf8)
        let imageBytes = try solidRedPNG()
        let htmlBytes = Data("<html><head><link rel='stylesheet' href='style.css'></head><body><img width='64' height='64' src='image.png'></body></html>".utf8)
        let cssBytes = Data("body { margin: 0; background: rgb(0, 0, 255); }".utf8)
        try markdownBytes.write(to: sourceRoot.appendingPathComponent("site/index.md"))
        try imageBytes.write(to: imageDirectory.appendingPathComponent("sample.png"))
        try htmlBytes.write(to: webDirectory.appendingPathComponent("index.html"))
        try cssBytes.write(to: webDirectory.appendingPathComponent("style.css"))
        try imageBytes.write(to: webDirectory.appendingPathComponent("image.png"))
        let integrity = try Data(contentsOf: package.appendingPathComponent("integrity.json"))
        let executorDigest = SHA256.hash(data: integrity).map { String(format: "%02x", $0) }.joined()

        let config = base.appendingPathComponent("ssh_config")
        let knownHosts = base.appendingPathComponent("known_hosts")
        try Data("""
        Host plugin-resource-fixture
          HostName \(host)
          Port \(port)
          User \(user)
          IdentityFile \(keyPath)
        """.utf8).write(to: config)
        try Data().write(to: knownHosts)
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: knownHosts.path)
        guard let controller = model.extensionController else {
            throw XCTSkip("QuickJS-enabled AppModel plugin owner is unavailable")
        }
        defer { Task { await model.stopExtensionRun(); await controller.close() } }
        model.interaction = AcceptAllInteraction()
        let review = try await controller.review(directory: package)
        let approved = try await controller.approve(id: review.id, digest: review.digest)
        XCTAssertEqual(approved, 0)

        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest,
            command: "preview", arguments: ["site/index.md", "--assets", "site/images/sample.png"],
            sshAlias: "plugin-resource-fixture",
            executable: gterm, executorDirectory: package.path,
            executorDigest: executorDigest,
            methods: [], hostMethods: [],
            scopedRoots: [.init(id: "workspace", directory: sourceRoot.path)])
        guard model.extensionRunIssue == nil, let consentRun = model.extensionRun,
              consentRun.session?.phase == ExtensionSession.Phase.consent.rawValue else {
            XCTFail(model.extensionRunIssue ?? "Client Session did not reach Native consent")
            return
        }
        XCTAssertTrue(consentRun.remoteCommand
            .contains("'--root' 'workspace' '\(sourceRoot.path)'"))

        await model.decideExtensionConsent(allow: true)
        let deadline = ProcessInfo.processInfo.systemUptime + 35
        while model.extensionRun != nil && (model.extensionRun?.receivedResources.count ?? 0) < 2 &&
              ProcessInfo.processInfo.systemUptime < deadline {
            if let issue = model.extensionRunIssue { XCTFail("resource runtime failed: \(issue)"); break }
            if let runtime = model.extensionRun?.runtime,
               runtime.phase == GhostermCore.ExtensionRuntime.Phase.failed.rawValue {
                XCTFail("resource runtime entered failed state: \(runtime); " +
                    "session=\(String(describing: model.extensionRun?.session)); " +
                    "resources=\(model.extensionRun?.receivedResources.count ?? 0)")
                break
            }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        let resources = try XCTUnwrap(model.extensionRun?.receivedResources,
            "\(model.extensionRunIssue ?? "gterm resources did not reach AppModel"); runtime=\(String(describing: model.extensionRun?.runtime)), session=\(String(describing: model.extensionRun?.session))")
        XCTAssertEqual(resources.count, 2)
        let markdown = try XCTUnwrap(resources.first { $0.displayName == "site/index.md" })
        XCTAssertEqual(markdown.bytes, markdownBytes)
        XCTAssertEqual(markdown.mime, "text/markdown")
        XCTAssertEqual(markdown.size, UInt64(markdownBytes.count))
        XCTAssertEqual(markdown.sha256, SHA256.hash(data: markdownBytes).map { String(format: "%02x", $0) }.joined())
        let image = try XCTUnwrap(resources.first { $0.displayName == "site/images/sample.png" })
        XCTAssertEqual(image.bytes, imageBytes)
        XCTAssertEqual(image.mime, "image/png")
        XCTAssertEqual(image.size, UInt64(imageBytes.count))
        XCTAssertEqual(image.sha256, SHA256.hash(data: imageBytes).map { String(format: "%02x", $0) }.joined())
        let pane = NSHostingView(rootView: ExtensionsPane(model: model))
        pane.frame = NSRect(x: 0, y: 0, width: 1440, height: 1500)
        var recognizedPreview = ""
        var redPixels = 0
        for _ in 0..<30 {
            pane.layoutSubtreeIfNeeded()
            if let bitmap = pane.bitmapImageRepForCachingDisplay(in: pane.bounds) {
                pane.cacheDisplay(in: pane.bounds, to: bitmap)
                if let image = bitmap.cgImage {
                    let request = VNRecognizeTextRequest()
                    request.recognitionLevel = .accurate
                    request.usesLanguageCorrection = false
                    try VNImageRequestHandler(cgImage: image).perform([request])
                    recognizedPreview = (request.results ?? [])
                        .compactMap { $0.topCandidates(1).first?.string }
                        .joined(separator: " ")
                    redPixels = redPixelCount(in: bitmap)
                    if recognizedPreview.lowercased().contains("resource from appmodel over ssh") &&
                       redPixels > 10 { break }
                }
            }
            try await Task.sleep(for: .milliseconds(100))
        }
        XCTAssertTrue(recognizedPreview.lowercased().contains("resource from appmodel over ssh"),
            "the production ExtensionsPane must render the exact resource received by AppModel; OCR: \(recognizedPreview)")
        XCTAssertGreaterThan(redPixels, 10,
            "the production ExtensionsPane must render the explicitly received Markdown PNG attachment")

        await model.stopExtensionRun()
        XCTAssertNil(model.extensionRun)
        XCTAssertNil(model.extensionRunIssue)

        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest,
            command: "preview", arguments: ["site/web/index.html", "--assets",
                "site/web/style.css,site/web/image.png"],
            sshAlias: "plugin-resource-fixture",
            executable: gterm, executorDirectory: package.path,
            executorDigest: executorDigest, methods: [], hostMethods: [],
            scopedRoots: [.init(id: "workspace", directory: sourceRoot.path)])
        guard model.extensionRunIssue == nil, let htmlConsent = model.extensionRun,
              htmlConsent.session?.phase == ExtensionSession.Phase.consent.rawValue else {
            XCTFail(model.extensionRunIssue ?? "HTML Preview Session did not reach Native consent")
            return
        }
        XCTAssertTrue(htmlConsent.remoteCommand
            .contains("'--root' 'workspace' '\(sourceRoot.path)'"))
        await model.decideExtensionConsent(allow: true)
        let htmlDeadline = ProcessInfo.processInfo.systemUptime + 35
        while model.extensionRun != nil && (model.extensionRun?.receivedResources.count ?? 0) < 3 &&
              ProcessInfo.processInfo.systemUptime < htmlDeadline {
            if let issue = model.extensionRunIssue { XCTFail("HTML resource runtime failed: \(issue)"); break }
            if let runtime = model.extensionRun?.runtime,
               runtime.phase == GhostermCore.ExtensionRuntime.Phase.failed.rawValue {
                XCTFail("HTML resource runtime failed: \(runtime); " +
                    "session=\(String(describing: model.extensionRun?.session))")
                break
            }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        let htmlResources = try XCTUnwrap(model.extensionRun?.receivedResources,
            "HTML Preview resources did not reach AppModel; runtime=\(String(describing: model.extensionRun?.runtime))")
        XCTAssertEqual(htmlResources.count, 3)
        let html = try XCTUnwrap(htmlResources.first { $0.displayName == "site/web/index.html" })
        XCTAssertEqual(html.bytes, htmlBytes)
        XCTAssertEqual(html.mime, "text/html")
        XCTAssertEqual(html.size, UInt64(htmlBytes.count))
        XCTAssertEqual(html.sha256, SHA256.hash(data: htmlBytes).map { String(format: "%02x", $0) }.joined())
        let css = try XCTUnwrap(htmlResources.first { $0.displayName == "site/web/style.css" })
        XCTAssertEqual(css.bytes, cssBytes)
        XCTAssertEqual(css.mime, "text/css")
        XCTAssertEqual(css.size, UInt64(cssBytes.count))
        XCTAssertEqual(css.sha256, SHA256.hash(data: cssBytes).map { String(format: "%02x", $0) }.joined())
        let htmlImage = try XCTUnwrap(htmlResources.first { $0.displayName == "site/web/image.png" })
        XCTAssertEqual(htmlImage.bytes, imageBytes)
        XCTAssertEqual(htmlImage.mime, "image/png")
        XCTAssertEqual(htmlImage.size, UInt64(imageBytes.count))
        XCTAssertEqual(htmlImage.sha256, SHA256.hash(data: imageBytes).map { String(format: "%02x", $0) }.joined())
        if let capturePath = ProcessInfo.processInfo.environment["GHOSTERM_HTML_PROBE_CAPTURE"] {
            let captureDirectory = URL(fileURLWithPath: capturePath, isDirectory: true)
                .appendingPathComponent("site/web", isDirectory: true)
            try FileManager.default.createDirectory(at: captureDirectory, withIntermediateDirectories: true)
            try html.bytes.write(to: captureDirectory.appendingPathComponent("index.html"))
            try css.bytes.write(to: captureDirectory.appendingPathComponent("style.css"))
            try htmlImage.bytes.write(to: captureDirectory.appendingPathComponent("image.png"))
        }

        await model.stopExtensionRun()
        XCTAssertNil(model.extensionRun)
        XCTAssertNil(model.extensionRunIssue)
        let closed = await controller.close()
        XCTAssertTrue(closed)
    }

    private func solidRedPNG() throws -> Data {
        let pixels = Data((0..<16 * 16).flatMap { _ in [UInt8(0xff), 0, 0, 0xff] })
        let provider = try XCTUnwrap(CGDataProvider(data: pixels as CFData))
        let image = try XCTUnwrap(CGImage(width: 16, height: 16, bitsPerComponent: 8,
            bitsPerPixel: 32, bytesPerRow: 64, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue), provider: provider,
            decode: nil, shouldInterpolate: false, intent: .defaultIntent))
        let encoded = NSMutableData()
        let destination = try XCTUnwrap(CGImageDestinationCreateWithData(encoded, "public.png" as CFString, 1, nil))
        CGImageDestinationAddImage(destination, image, nil)
        XCTAssertTrue(CGImageDestinationFinalize(destination))
        return encoded as Data
    }

    private func redPixelCount(in bitmap: NSBitmapImageRep) -> Int {
        var count = 0
        for y in stride(from: 0, to: bitmap.pixelsHigh, by: 2) {
            for x in stride(from: 0, to: bitmap.pixelsWide, by: 2) {
                guard let color = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                if color.redComponent > 0.8 && color.greenComponent < 0.3 && color.blueComponent < 0.3 {
                    count += 1
                }
            }
        }
        return count
    }
}
