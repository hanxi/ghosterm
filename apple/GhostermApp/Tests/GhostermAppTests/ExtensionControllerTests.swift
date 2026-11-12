import CryptoKit
import Darwin
import Foundation
import GhostermAppModel
import GhostermCore
import XCTest

final class ExtensionControllerTests: XCTestCase {
    @MainActor
    func testClientHostProfileOnlyAdvertisesIntegratedCapabilities() {
        XCTAssertEqual(AppModel.extensionHostCapabilities, ["session.rpc"])
        XCTAssertFalse(AppModel.extensionHostCapabilities.contains("session.resources"))
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
        XCTAssertEqual(try ExtensionRelayBootstrap.parseArgumentsJSON(#"["a", "b c"]"#), ["a", "b c"])
        XCTAssertThrowsError(try ExtensionRelayBootstrap.parseArgumentsJSON(#"["ok"] false"#))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.parseMethodsJSON(#"["bad..name"]"#))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(.init(
            executable: "/opt/gterm", executorDirectory: "/opt/executor",
            executorDigest: digest, clientDigest: digest, methods: ["same"],
            hostMethods: ["same"], command: "fixture.run")))
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
            "client": ["runtime": "portable-js", "entry": "client/index.js", "requires": ["session.rpc"]],
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

        let profile = try ExtensionController.HostProfile(side: .client, sdkVersion: "1.0.0",
                                                          protocols: [1], runtimes: ["portable-js"],
                                                          capabilities: ["session.rpc"])
        let controller = try ExtensionController(profile: profile)
        defer { Task { await controller.close() } }

        let review = try await controller.review(directory: root)
        let secondReview = try await controller.review(directory: secondRoot)
        XCTAssertEqual(review.id, "org.ghosterm.controller-fixture")
        XCTAssertEqual(review.commands.map(\.path), ["fixture"])
        XCTAssertEqual(review.runtime, "portable-js")
        XCTAssertEqual(review.requiredCapabilities, ["session.rpc"])
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
}
