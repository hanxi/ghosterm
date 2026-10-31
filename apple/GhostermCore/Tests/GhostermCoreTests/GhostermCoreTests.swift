// GhostermCoreTests — 验证 Swift 绑定真的穿透到 C 核心（不是空壳）。
//
// SSH 真连测试需要 env GHOSTERM_SSH_*（由 scripts/apple-gate.sh 起 sshd 后注入）；
// 缺省时只跑离线断言，SSH 部分显式跳过（不假装通过）。
import XCTest
@testable import GhostermCore

final class GhostermCoreTests: XCTestCase {
    func testIdentitySourcesCopyValuesAcrossEdit() throws {
        let text = "IdentityFile global\nHost demo sibling\n IdentityFile \"/keys/中文 🔑\"\nHost *\n IdentityFile inherited\n"
        let config = try SSHConfig(contents: text)
        let entries = try config.identities(alias: "demo")
        XCTAssertEqual(entries.map(\.value), ["global", "/keys/中文 🔑", "inherited"])
        XCTAssertEqual(entries.map(\.explicitValue), [false, true, false])
        XCTAssertEqual(entries.map(\.effective), [true, true, true])
        XCTAssertEqual(entries.map(\.sourceLine), [1, 3, 5])
        XCTAssertEqual(entries.map(\.sourcePath), ["", "", ""])
        XCTAssertEqual(entries.map(\.sourceHeader), ["", "Host demo sibling\n", "Host *\n"])
        XCTAssertEqual(entries.map(\.included), [false, false, false])
        XCTAssertEqual(config.serialized(), text)
        try config.edit(alias: "demo", [("IdentityFile", ["changed"])])
        XCTAssertEqual(try config.identities(alias: "sibling")[1].value, "changed")
        XCTAssertEqual(entries[1].value, "/keys/中文 🔑", "borrowed Core strings must be copied")
        try config.edit(alias: "demo", [("IdentityFile", nil)])
        XCTAssertEqual(try config.identities(alias: "demo").map(\.value), ["global", "inherited"])
    }

    func testIdentitySourcesUnsupportedIsNotEmpty() throws {
        let config = try SSHConfig(contents: "Host demo\n IdentityFile valid\nMatch all\n IdentityFile conditional\n")
        XCTAssertThrowsError(try config.identities(alias: "demo"))
        let invalid = try SSHConfig(contents: "Host demo\n IdentityFile first second\n")
        XCTAssertThrowsError(try invalid.identities(alias: "demo"))
        let empty = try SSHConfig(contents: "Host demo\n")
        XCTAssertEqual(try empty.identities(alias: "demo"), [])
        XCTAssertThrowsError(try empty.identities(alias: "missing"))
    }

    func testConnectionIssuesCopySourceAndPreserveEffectiveScope() throws {
        let cfg = try SSHConfig(contents: "Host web\n ProxyJump gateway\nHost other\n ProxyJump none\n")
        let issues = try cfg.connectionIssues(host: "web")
        XCTAssertEqual(issues.map(\.keyword), ["proxyjump"])
        XCTAssertEqual(issues.first?.sourceLine, 2)
        XCTAssertEqual(issues.first?.sourcePath, "")
        XCTAssertEqual(issues.first?.included, false)
        XCTAssertTrue(try cfg.connectionIssues(host: "other").isEmpty)
        try cfg.edit(alias: "web", [("ProxyJump", ["none"])])
        XCTAssertTrue(try cfg.connectionIssues(host: "web").isEmpty)
        XCTAssertEqual(issues.first?.keyword, "proxyjump")
    }

    func testVersionAndError() {
        XCTAssertFalse(Ghosterm.version.isEmpty)
        XCTAssertNotNil(Ghosterm.lastError) // 线程局部错误串总可读
    }

    func testConfigRoundTripIsByteIdentical() throws {
        let text = """
        # 顶层注释
        Host web
          HostName 10.0.0.5
          User deploy
          IdentityFile=~/.ssh/id_ed25519

        # 尾部注释
        """
        let cfg = try SSHConfig(contents: text)
        XCTAssertEqual(cfg.serialized(), text, "未编辑往返必须逐字节相同")
        XCTAssertEqual(cfg.hostPatterns, ["web"])
    }

    func testConfigSparseEditTouchesOnlyTargetLine() throws {
        let text = "Host web\n  HostName 10.0.0.5\n  User deploy\n"
        let cfg = try SSHConfig(contents: text)
        try cfg.edit(alias: "web", [("User", ["ops"])])
        XCTAssertEqual(cfg.serialized(), "Host web\n  HostName 10.0.0.5\n  User ops\n")
        // 删除指令
        try cfg.edit(alias: "web", [("User", nil)])
        XCTAssertEqual(cfg.serialized(), "Host web\n  HostName 10.0.0.5\n")
    }

    func testConfigEditMissingAliasThrows() throws {
        let cfg = try SSHConfig(contents: "Host web\n  User x\n")
        XCTAssertThrowsError(try cfg.edit(alias: "nope", [("User", ["y"])]))
    }

    func testConfigRenamePreservesSiblingAndRejectsCollision() throws {
        let cfg = try SSHConfig(contents: "# header\r\nHost \"demo\" sibling # names\r\n  User me\r\nHost occupied\n")
        try cfg.rename(alias: "demo", to: "new-host")
        XCTAssertEqual(cfg.serialized(), "# header\r\nHost new-host sibling # names\r\n  User me\r\nHost occupied\n")
        XCTAssertEqual(cfg.hostPatterns, ["new-host", "sibling", "occupied"])
        let before = cfg.serialized()
        XCTAssertThrowsError(try cfg.rename(alias: "new-host", to: "OCCUPIED"))
        XCTAssertEqual(cfg.serialized(), before)
    }

    func testConfigMetadataCopiesValuesAcrossEdit() throws {
        let cfg = try SSHConfig(contents: "User inherited\nHost web sibling\n  HostName old.test\n  User explicit\n")
        let before = try cfg.inspect(alias: "web")
        XCTAssertEqual(before.header, "Host web sibling\n")
        XCTAssertEqual(before.patternCount, 2)
        XCTAssertEqual(before.user.explicitValue, "explicit")
        XCTAssertFalse(before.user.effectiveIsExplicit)
        XCTAssertEqual(before.user.sourceLine, 1)
        XCTAssertEqual(before.hostname.explicitValue, "old.test")
        XCTAssertTrue(before.hostname.effectiveIsExplicit)
        XCTAssertEqual(before.port.sourceLine, 0)
        try cfg.edit(alias: "web", [("HostName", ["new.test"])])
        XCTAssertEqual(try cfg.inspect(alias: "web").hostname.explicitValue, "new.test")
        XCTAssertEqual(before.hostname.explicitValue, "old.test", "metadata must own copied strings across Core edits")
        XCTAssertThrowsError(try cfg.inspect(alias: "missing"))
    }

    func testLocalPTYReadWriteResize() throws {
        let t = try Transport.openPTY(executable: "/bin/sh", argv: ["-c", "printf BIND_OK"],
                                      kind: .direct, cols: 80, rows: 24)
        var acc = ""
        var sawEOF = false
        for _ in 0..<50 {
            switch try t.read(timeoutMilliseconds: 100) {
            case .data(let bytes): acc += String(decoding: bytes, as: UTF8.self)
            case .timeout: continue
            case .eof: sawEOF = true
            }
            if sawEOF { break }
        }
        XCTAssertTrue(acc.contains("BIND_OK"), "PTY 输出穿透绑定（得到: \(acc.debugDescription)）")
    }

    func testLocalPTYTimeoutIsNotEOF() throws {
        let t = try Transport.openPTY(executable: "/bin/sh", argv: ["-c", "sleep 5"],
                                      kind: .direct)
        switch try t.read(timeoutMilliseconds: 100) {
        case .timeout: break
        default: XCTFail("无数据应为 .timeout，不得折叠成 .eof")
        }
        XCTAssertEqual(t.kind, .localPTY)
    }

    /// SFTP e2e：绑定 → list → stat → upload → download 全链路（真 sshd）。
    func testSFTPEndToEnd() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let pemPath = env["GHOSTERM_SSH_KEY"],
              let portStr = env["GHOSTERM_SSH_PORT"], let port = UInt16(portStr),
              let pem = try? String(contentsOfFile: pemPath, encoding: .utf8) else {
            throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过 SFTP e2e")
        }
        let work = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("sftp-e2e-\(UUID().uuidString)").path
        try FileManager.default.createDirectory(atPath: work,
                                                withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: work) }

        let session = try SSHSession(SSHOptions(
            host: host, port: port, username: user,
            knownHostsPath: env["GHOSTERM_SSH_KNOWN_HOSTS"]
        ))
        session.setHostKeyDecider { _, _, _, _ in .accept }
        try session.authenticate(privateKeyPEM: pem)
        try session.connect()
        let sftp = try session.sftp()

        // upload job → 等结果（事件三态不折叠）。
        let payload = String(repeating: "GHOSTERM_SFTP_", count: 512)
        let localSrc = work + "/src.txt"
        try payload.write(toFile: localSrc, atomically: true, encoding: .utf8)
        let remote = work + "/remote-uploaded.txt"
        let upJob = try sftp.upload(local: localSrc, remote: remote)
        XCTAssertNotEqual(upJob, 0, "upload job id 非 0")
        XCTAssertTrue(waitForJob(upJob, .ok, timeout: 15),
                      "upload job 应以 ok 结束")

        // stat：远端尺寸正确（字节级）。
        let st = try sftp.stat(path: remote)
        XCTAssertEqual(st.size, UInt64(payload.utf8.count), "远端尺寸等于上传尺寸")

        // list：目录里能看到刚上传的文件（name / fullPath 都对）。
        let entries = try sftp.list(path: work)
        XCTAssertTrue(entries.contains {
            $0.name == "remote-uploaded.txt" && $0.fullPath == remote
        }, "列目录应含刚上传文件（得到 \(entries.map(\.name))）")

        // download → 内容一致。
        let localDst = work + "/downloaded.txt"
        let downJob = try sftp.download(remote: remote, local: localDst)
        XCTAssertNotEqual(downJob, 0)
        XCTAssertTrue(waitForJob(downJob, .ok, timeout: 15), "download job 应以 ok 结束")
        XCTAssertEqual(try String(contentsOfFile: localDst, encoding: .utf8), payload,
                       "下载内容逐字节一致")

        // 失败 job 三态：不存在的远端 → failed（不折叠成 ok）。
        let badJob = try sftp.download(remote: work + "/no-such-file", local: localDst)
        XCTAssertTrue(waitForJob(badJob, .failed, timeout: 15),
                      "失败 job 必须以 failed 上报（三态不折叠）")
    }

    /// 轮询事件队列直到目标 job 出现期望结果（或超时）。
    private func waitForJob(_ jobID: UInt64, _ want: CoreEvent.JobResult,
                            timeout: TimeInterval) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            for ev in Ghosterm.pollEvents() {
                if ev.kind == .jobDone && ev.jobID == jobID {
                    return ev.jobResult == want
                }
            }
            Thread.sleep(forTimeInterval: 0.02)
        }
        return false
    }

    func testSSHAbortStopsOwnedSFTP() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let key = env["GHOSTERM_SSH_KEY"], let port = UInt16(env["GHOSTERM_SSH_PORT"] ?? "") else {
            throw XCTSkip("SSH fixture required")
        }
        let session = try SSHSession(SSHOptions(host: host, port: port, username: user,
            knownHostsPath: env["GHOSTERM_SSH_KNOWN_HOSTS"]))
        session.setHostKeyDecider { _, _, _, _ in .accept }
        try session.authenticate(privateKeyPEM: String(contentsOfFile: key, encoding: .utf8))
        try session.connect()
        defer { session.close() }
        let files = try session.sftp(timeoutMilliseconds: 3000)
        defer { files.close() }
        let directory = (key as NSString).deletingLastPathComponent
        XCTAssertEqual(try files.browse(path: directory, timeoutMilliseconds: 3000).path, directory)
        try session.abort()
        XCTAssertThrowsError(try files.browse(path: directory, timeoutMilliseconds: 3000)) { error in
            guard case GhostermError.status(1, _) = error else { return XCTFail("Aborted owner must be unavailable") }
        }
        XCTAssertNoThrow(try session.abort())
    }

    func testSFTPDirectoryCreationEndToEnd() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let key = env["GHOSTERM_SSH_KEY"], let port = UInt16(env["GHOSTERM_SSH_PORT"] ?? "") else {
            throw XCTSkip("SSH fixture required")
        }
        for mode in ["normal", "conflict", "missing"] {
            let session = try SSHSession(SSHOptions(host: host, port: port, username: user,
                knownHostsPath: env["GHOSTERM_SSH_KNOWN_HOSTS"]))
            session.setHostKeyDecider { _, _, _, _ in .accept }
            try session.authenticate(privateKeyPEM: String(contentsOfFile: key, encoding: .utf8))
            try session.connect(); defer { session.close() }
            let files = try session.sftp(timeoutMilliseconds: 3000); defer { files.close() }
            let parent = (key as NSString).deletingLastPathComponent + "/mkdir-" + UUID().uuidString
            let name = "目录 ☃", target = parent + "/" + name
            if mode != "missing" { try FileManager.default.createDirectory(atPath: parent, withIntermediateDirectories: false) }
            defer { try? FileManager.default.removeItem(atPath: parent) }
            if mode == "conflict" { try Data("original".utf8).write(to: URL(fileURLWithPath: target)) }
            XCTAssertThrowsError(try files.createDirectory(parent: parent, name: "bad\0name", timeoutMilliseconds: 3000))
            let invalid = try files.createDirectory(parent: parent, name: name, timeoutMilliseconds: 0)
            XCTAssertEqual(invalid.status, 1); XCTAssertEqual(invalid.state, .notAttempted)
            let cancellation = SSHCancellation(); cancellation.cancel()
            let cancelled = try files.createDirectory(parent: parent, name: name, timeoutMilliseconds: 3000, cancellation: cancellation)
            XCTAssertEqual(cancelled.status, 8); XCTAssertEqual(cancelled.state, .notAttempted)
            let result = try files.createDirectory(parent: parent, name: name, timeoutMilliseconds: 3000)
            XCTAssertEqual(result.status, mode == "normal" ? 0 : 3)
            XCTAssertEqual(result.state, mode == "normal" ? .complete : mode == "conflict" ? .conflict : .notAttempted)
            XCTAssertEqual(result.remoteStatus, mode == "missing" ? 2 : 0)
            if mode == "normal" {
                XCTAssertEqual(try files.list(path: target).count, 0)
                XCTAssertEqual(try files.createDirectory(parent: target, name: "nested", timeoutMilliseconds: 3000).state, .complete)
                XCTAssertEqual(try files.list(path: target).map(\.fullPath), [target + "/nested"])
            } else if mode == "conflict" {
                XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: target)), Data("original".utf8))
            }
        }
    }

    func testSFTPPublishEndToEnd() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let key = env["GHOSTERM_SSH_KEY"], let port = UInt16(env["GHOSTERM_SSH_PORT"] ?? "") else {
            throw XCTSkip("SSH fixture required")
        }
        for mode in ["file", "empty", "directory", "conflict", "missing"] {
            let session = try SSHSession(SSHOptions(host: host, port: port, username: user,
                knownHostsPath: env["GHOSTERM_SSH_KNOWN_HOSTS"]))
            session.setHostKeyDecider { _, _, _, _ in .accept }
            try session.authenticate(privateKeyPEM: String(contentsOfFile: key, encoding: .utf8))
            try session.connect(); defer { session.close() }
            let files = try session.sftp(timeoutMilliseconds: 3000); defer { files.close() }
            let source = (key as NSString).deletingLastPathComponent + "/publish-" + UUID().uuidString
            let target = source + "-target", directory = mode == "directory"
            let payload = Data((mode == "empty" ? "" : "data").utf8)
            if directory { try FileManager.default.createDirectory(atPath: source, withIntermediateDirectories: false) }
            else if mode != "missing" { try payload.write(to: URL(fileURLWithPath: source)) }
            if mode == "conflict" { try Data("original".utf8).write(to: URL(fileURLWithPath: target)) }
            defer { try? FileManager.default.removeItem(atPath: source); try? FileManager.default.removeItem(atPath: target) }
            let size: UInt64 = directory ? 0 : UInt64(payload.count)
            let invalid = try files.publish(source: source, destination: target, directory: directory,
                expectedSize: size, timeoutMilliseconds: 0)
            XCTAssertEqual(invalid.status, 1); XCTAssertEqual(invalid.state, .notAttempted)
            let cancellation = SSHCancellation(); cancellation.cancel()
            let cancelled = try files.publish(source: source, destination: target, directory: directory,
                expectedSize: size, timeoutMilliseconds: 3000, cancellation: cancellation)
            XCTAssertEqual(cancelled.status, 8); XCTAssertEqual(cancelled.state, .notAttempted)
            let result = try files.publish(source: source, destination: target, directory: directory,
                expectedSize: size, timeoutMilliseconds: 3000)
            XCTAssertEqual(result.status, mode == "conflict" || mode == "missing" ? 3 : 0)
            XCTAssertEqual(result.state, mode == "conflict" ? .conflict : mode == "missing" ? .notAttempted : .published)
            XCTAssertEqual(result.remoteStatus, mode == "missing" ? 2 : 0)
            if mode == "conflict" {
                XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: target)), Data("original".utf8))
                XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: source)), payload)
            } else if mode != "missing" {
                XCTAssertFalse(FileManager.default.fileExists(atPath: source))
                if directory { XCTAssertEqual(try files.list(path: target).count, 0) }
                else { XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: target)), payload) }
            }
        }
    }

    func testSFTPReceiveEndToEnd() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let key = env["GHOSTERM_SSH_KEY"], let port = UInt16(env["GHOSTERM_SSH_PORT"] ?? "") else {
            throw XCTSkip("SSH fixture required")
        }
        for size in [0, 70001] {
            let session = try SSHSession(SSHOptions(host: host, port: port, username: user,
                knownHostsPath: env["GHOSTERM_SSH_KNOWN_HOSTS"]))
            session.setHostKeyDecider { _, _, _, _ in .accept }
            try session.authenticate(privateKeyPEM: String(contentsOfFile: key, encoding: .utf8))
            try session.connect()
            defer { session.close() }
            let files = try session.sftp(timeoutMilliseconds: 3000)
            defer { files.close() }
            let source = (key as NSString).deletingLastPathComponent + "/receive-" + UUID().uuidString
            let target = source + "-local"
            let payload = Data((0..<size).map { UInt8(truncatingIfNeeded: $0 * 31 + 7) })
            try payload.write(to: URL(fileURLWithPath: source))
            try Data().write(to: URL(fileURLWithPath: target))
            let output = try FileHandle(forUpdating: URL(fileURLWithPath: target))
            defer { try? output.close(); try? FileManager.default.removeItem(atPath: source); try? FileManager.default.removeItem(atPath: target) }
            try output.seek(toOffset: 7)
            XCTAssertThrowsError(try files.receive(path: source, destinationDescriptor: output.fileDescriptor,
                maxBytes: UInt64(size), timeoutMilliseconds: 0))
            let cancellation = SSHCancellation(); cancellation.cancel()
            XCTAssertThrowsError(try files.receive(path: source, destinationDescriptor: output.fileDescriptor,
                maxBytes: UInt64(size), timeoutMilliseconds: 3000, cancellation: cancellation))
            let count = try files.receive(path: source, destinationDescriptor: output.fileDescriptor,
                maxBytes: UInt64(size), timeoutMilliseconds: 3000)
            XCTAssertEqual(count, UInt64(size))
            XCTAssertEqual(try output.offset(), 7)
            XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: target)), payload)
            XCTAssertThrowsError(try files.receive(path: source + "-absent", destinationDescriptor: output.fileDescriptor,
                maxBytes: UInt64(size), timeoutMilliseconds: 3000))
        }
    }

    func testSFTPExclusiveCreateEndToEnd() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let key = env["GHOSTERM_SSH_KEY"], let port = UInt16(env["GHOSTERM_SSH_PORT"] ?? "") else {
            throw XCTSkip("SSH fixture required")
        }
        for size in [0, 70001] {
            let session = try SSHSession(SSHOptions(host: host, port: port, username: user,
                knownHostsPath: env["GHOSTERM_SSH_KNOWN_HOSTS"]))
            session.setHostKeyDecider { _, _, _, _ in .accept }
            try session.authenticate(privateKeyPEM: String(contentsOfFile: key, encoding: .utf8))
            try session.connect()
            defer { session.close() }
            let files = try session.sftp(timeoutMilliseconds: 3000)
            defer { files.close() }
            let source = (key as NSString).deletingLastPathComponent + "/create-" + UUID().uuidString
            let target = source + "-remote"
            let payload = Data((0..<size).map { UInt8(truncatingIfNeeded: $0 * 31 + 7) })
            try payload.write(to: URL(fileURLWithPath: source))
            let input = try FileHandle(forReadingFrom: URL(fileURLWithPath: source))
            defer { try? input.close(); try? FileManager.default.removeItem(atPath: source); try? FileManager.default.removeItem(atPath: target) }
            try input.seek(toOffset: 7)
            let zero = try files.createExclusive(path: target, sourceDescriptor: input.fileDescriptor,
                expectedSize: UInt64(size), timeoutMilliseconds: 0)
            XCTAssertEqual(zero.status, 1); XCTAssertEqual(zero.state, .notAttempted)
            let cancellation = SSHCancellation(); cancellation.cancel()
            let cancelled = try files.createExclusive(path: target, sourceDescriptor: input.fileDescriptor,
                expectedSize: UInt64(size), timeoutMilliseconds: 3000, cancellation: cancellation)
            XCTAssertEqual(cancelled.status, 8); XCTAssertEqual(cancelled.state, .notAttempted)
            let created = try files.createExclusive(path: target, sourceDescriptor: input.fileDescriptor,
                expectedSize: UInt64(size), timeoutMilliseconds: 3000)
            XCTAssertEqual(created.status, 0); XCTAssertEqual(created.state, .complete)
            XCTAssertEqual(created.bytesAcknowledged, UInt64(size))
            XCTAssertEqual(try input.offset(), 7)
            XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: target)), payload)
            let conflict = try files.createExclusive(path: target, sourceDescriptor: input.fileDescriptor,
                expectedSize: UInt64(size), timeoutMilliseconds: 3000)
            XCTAssertEqual(conflict.status, 3); XCTAssertEqual(conflict.state, .rejected)
            XCTAssertEqual(conflict.bytesAcknowledged, 0)
            XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: target)), payload)
        }
    }

    func testSFTPPreviewEndToEnd() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let key = env["GHOSTERM_SSH_KEY"], let port = UInt16(env["GHOSTERM_SSH_PORT"] ?? "") else {
            throw XCTSkip("SSH fixture required")
        }
        let session = try SSHSession(SSHOptions(host: host, port: port, username: user,
            knownHostsPath: env["GHOSTERM_SSH_KNOWN_HOSTS"]))
        session.setHostKeyDecider { _, _, _, _ in .accept }
        try session.authenticate(privateKeyPEM: String(contentsOfFile: key, encoding: .utf8))
        try session.connect()
        defer { session.close() }
        let files = try session.sftp(timeoutMilliseconds: 3000)
        defer { files.close() }
        let path = (key as NSString).deletingLastPathComponent + "/preview-数据"
        let payload = Data([0, 255, 65, 10])
        try payload.write(to: URL(fileURLWithPath: path))
        defer { try? FileManager.default.removeItem(atPath: path) }
        XCTAssertThrowsError(try files.preview(path: path, timeoutMilliseconds: 0)) { error in
            guard case GhostermError.status(1, _) = error else { return XCTFail("Preview zero budget must be INVALID_ARG") }
        }
        XCTAssertThrowsError(try files.preview(path: path + "\0ignored", timeoutMilliseconds: 3000))
        let cancelled = SSHCancellation(); cancelled.cancel()
        XCTAssertThrowsError(try files.preview(path: path, timeoutMilliseconds: 3000, cancellation: cancelled)) { error in
            guard case GhostermError.status(8, _) = error else { return XCTFail("Preview cancellation must be CANCELLED") }
        }
        XCTAssertEqual(try files.preview(path: path, timeoutMilliseconds: 3000, maxBytes: 4), payload)
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: path)), payload)
        XCTAssertThrowsError(try files.preview(path: path, timeoutMilliseconds: 3000, maxBytes: 3)) { error in
            guard case GhostermError.status(3, _) = error else { return XCTFail("Preview size limit must fail with IO") }
        }
        XCTAssertNoThrow(try session.abort())
        XCTAssertNoThrow(try session.abort())
    }

    func testSFTPBrowseEndToEnd() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let key = env["GHOSTERM_SSH_KEY"], let port = UInt16(env["GHOSTERM_SSH_PORT"] ?? "") else {
            throw XCTSkip("SSH fixture required")
        }
        let session = try SSHSession(SSHOptions(host: host, port: port, username: user,
            knownHostsPath: env["GHOSTERM_SSH_KNOWN_HOSTS"]))
        session.setHostKeyDecider { _, _, _, _ in .accept }
        try session.authenticate(privateKeyPEM: String(contentsOfFile: key, encoding: .utf8))
        try session.connect()
        defer { session.close() }
        let files = try session.sftp(timeoutMilliseconds: 3000)
        defer { files.close() }
        let directory = (key as NSString).deletingLastPathComponent
        let empty = URL(fileURLWithPath: directory).appendingPathComponent("browse-zero")
        try Data().write(to: empty)
        defer { try? FileManager.default.removeItem(at: empty) }
        XCTAssertThrowsError(try files.browse(path: directory, timeoutMilliseconds: 0)) { error in
            guard case GhostermError.status(1, _) = error else { return XCTFail("Browse zero budget must be INVALID_ARG") }
        }
        let cancelled = SSHCancellation(); cancelled.cancel()
        XCTAssertThrowsError(try files.browse(path: directory, timeoutMilliseconds: 3000, cancellation: cancelled)) { error in
            guard case GhostermError.status(8, _) = error else { return XCTFail("Browse cancellation must be CANCELLED") }
        }
        let result = try files.browse(path: directory, timeoutMilliseconds: 3000)
        XCTAssertEqual(result.path, directory)
        let entry = try XCTUnwrap(result.entries.first { $0.name == "browse-zero" })
        XCTAssertTrue(entry.hasSize)
        XCTAssertEqual(entry.size, 0)
        XCTAssertTrue(entry.isRegularFile)
        XCTAssertEqual(entry.fullPath, empty.path)
        XCTAssertTrue(try files.realpath(".").hasPrefix("/"))
    }

    func testSFTPControlledEndToEnd() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let key = env["GHOSTERM_SSH_KEY"], let port = UInt16(env["GHOSTERM_SSH_PORT"] ?? "") else {
            throw XCTSkip("SSH fixture required")
        }
        let session = try SSHSession(SSHOptions(host: host, port: port, username: user,
            knownHostsPath: env["GHOSTERM_SSH_KNOWN_HOSTS"]))
        session.setHostKeyDecider { _, _, _, _ in .accept }
        try session.authenticate(privateKeyPEM: String(contentsOfFile: key, encoding: .utf8))
        try session.connect()
        XCTAssertThrowsError(try session.sftp(timeoutMilliseconds: 0)) { error in
            guard case GhostermError.status(1, _) = error else { return XCTFail("Spent SFTP budget was not INVALID_ARG") }
        }
        let cancelled = SSHCancellation(); cancelled.cancel()
        XCTAssertThrowsError(try session.sftp(timeoutMilliseconds: 3000, cancellation: cancelled)) { error in
            guard case GhostermError.status(8, _) = error else { return XCTFail("SFTP cancellation was not CANCELLED") }
        }
        let files = try session.sftp(timeoutMilliseconds: 3000, cancellation: SSHCancellation())
        defer { files.close() }
        let directory = (key as NSString).deletingLastPathComponent
        XCTAssertTrue(try files.realpath(".").hasPrefix("/"))
        XCTAssertTrue(try files.list(path: directory).contains { $0.name == "clientkey.pub" })
    }

    func testSSHControlledEndToEnd() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let key = env["GHOSTERM_SSH_KEY"], let port = UInt16(env["GHOSTERM_SSH_PORT"] ?? "") else {
            throw XCTSkip("SSH fixture required")
        }
        let pem = try String(contentsOfFile: key, encoding: .utf8)
        func connected() throws -> SSHSession {
            let value = try SSHSession(SSHOptions(host: host, port: port, username: user,
                knownHostsPath: env["GHOSTERM_SSH_KNOWN_HOSTS"]))
            value.setHostKeyDecider { _, _, _, _ in .accept }
            try value.authenticate(privateKeyPEM: pem); try value.connect()
            return value
        }
        let session = try connected()
        XCTAssertThrowsError(try Transport.openSSHShell(session, timeoutMilliseconds: 0)) { error in
            guard case GhostermError.status(1, _) = error else { return XCTFail("Spent budget did not return INVALID_ARG") }
        }
        let cancelled = SSHCancellation(); cancelled.cancel()
        let cancelledSession = try connected()
        XCTAssertThrowsError(try Transport.openSSHShell(cancelledSession, timeoutMilliseconds: 3000, cancellation: cancelled)) { error in
            guard case GhostermError.status(8, _) = error else { return XCTFail("Cancellation did not return CANCELLED") }
        }
        let transport = try Transport.openSSHShell(session, cols: 71, rows: 23,
            timeoutMilliseconds: 3000, cancellation: SSHCancellation())
        try transport.write("stty -echo; PS1=''; stty size; printf 'CONTROL_%s\\n' READY\n")
        var output = ""
        for _ in 0..<60 {
            if case .data(let bytes) = try transport.read(timeoutMilliseconds: 100) {
                output += String(decoding: bytes, as: UTF8.self)
                if output.contains("CONTROL_READY") { break }
            }
        }
        XCTAssertTrue(output.contains("23 71") && output.contains("CONTROL_READY"), "Controlled shell lost PTY or stream")
    }

    func testSSHEndToEnd() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let pemPath = env["GHOSTERM_SSH_KEY"],
              let portStr = env["GHOSTERM_SSH_PORT"], let port = UInt16(portStr),
              let pem = try? String(contentsOfFile: pemPath, encoding: .utf8) else {
            throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过 SSH e2e")
        }
        // known_hosts 显式落在测试工作目录：绝不碰用户真实 ~/.ssh/known_hosts
        // （decider 接受后核心会 TOFU 落盘，污染真实文件是不可接受的副作用）。
        let khPath = env["GHOSTERM_SSH_KNOWN_HOSTS"]
        let session = try SSHSession(SSHOptions(host: host, port: port, username: user,
                                                knownHostsPath: khPath))
        session.setHostKeyDecider { _, _, _, _ in .accept }
        try session.authenticate(privateKeyPEM: pem)
        try session.connect()

        let t = try Transport.openSSHShell(session, cols: 80, rows: 24)
        XCTAssertEqual(t.kind, .sshShell)
        try t.write("echo SWIFT_$((6*7))\n")
        var acc = ""
        for _ in 0..<60 {
            if case .data(let bytes) = try t.read(timeoutMilliseconds: 100) {
                acc += String(decoding: bytes, as: UTF8.self)
                if acc.contains("SWIFT_42") { break }
            }
        }
        XCTAssertTrue(acc.contains("SWIFT_42"), "SSH shell 回显穿透绑定（得到: \(acc.debugDescription)）")
        try t.resize(cols: 100, rows: 40)
        try t.write("stty size\n")
        var acc2 = ""
        for _ in 0..<60 {
            if case .data(let bytes) = try t.read(timeoutMilliseconds: 100) {
                acc2 += String(decoding: bytes, as: UTF8.self)
                if acc2.contains("40 100") { break }
            }
        }
        XCTAssertTrue(acc2.contains("40 100"), "resize 生效（得到: \(acc2.debugDescription)）")
    }
}

final class KnownHostsSnapshotTests: XCTestCase {
    private func fixtureDirectory() throws -> URL {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent()
        let directory = root.appendingPathComponent("tmp/known-hosts-swift-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }
    func testRemovalPreservesRevisionAndReportsConflict() throws {
        let directory = try fixtureDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("中文 🔑")
        let original = "# 保留\r\n a,b ssh-rsa AAAAAAAAAAAAAAAA\r\n\t\nother ssh-rsa BBBBBBBBBBBBBBBB"
        let expected = "# 保留\r\n\t\nother ssh-rsa BBBBBBBBBBBBBBBB"
        try Data(original.utf8).write(to: file)
        let result = KnownHosts.remove(path: file.path, expected: original, index: 0)
        XCTAssertTrue(result.succeeded); XCTAssertTrue(result.committed); XCTAssertFalse(result.conflict)
        XCTAssertEqual(result.message, "")
        XCTAssertEqual(try String(contentsOf: file, encoding: .utf8), expected)
        let backup = try XCTUnwrap(FileManager.default.contentsOfDirectory(at: directory,
            includingPropertiesForKeys: nil).first { $0.pathExtension == "bak" })
        XCTAssertEqual(try String(contentsOf: backup, encoding: .utf8), original)
        let stale = KnownHosts.remove(path: file.path, expected: original, index: 0)
        XCTAssertFalse(stale.succeeded); XCTAssertFalse(stale.committed); XCTAssertTrue(stale.conflict)
        XCTAssertFalse(stale.message.isEmpty)
        XCTAssertEqual(try String(contentsOf: file, encoding: .utf8), expected)
    }
    func testRemovalRejectsRulesAndIoFailure() throws {
        let directory = try fixtureDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("known_hosts")
        let original = "@revoked a ssh-rsa AAAAAAAAAAAAAAAA\n"
        try Data(original.utf8).write(to: file)
        let readonly = KnownHosts.remove(path: file.path, expected: original, index: 0)
        XCTAssertEqual(readonly.status, 7); XCTAssertFalse(readonly.committed)
        XCTAssertEqual(try String(contentsOf: file, encoding: .utf8), original)
        let failure = KnownHosts.remove(path: directory.appendingPathComponent("absent/known_hosts").path, expected: "", index: 0)
        XCTAssertEqual(failure.status, 3); XCTAssertFalse(failure.committed); XCTAssertFalse(failure.conflict)
        XCTAssertFalse(failure.message.isEmpty)
    }
    func testRecordsCopyMarkersAndByteRanges() throws {
        let prefix = "# 中文\r\n"
        let line = " @revoked [host]:2222,other ssh-ed25519 AAAAAAAAAAAAAAAA\r\n"
        let text = prefix + line + "|1|salt|hash ssh-rsa AAAAAAAAAAAAAAAA\nbroken"
        let rows = try KnownHosts.records(text)
        XCTAssertEqual(rows.count, 3)
        XCTAssertEqual(rows[0].marker, "@revoked")
        XCTAssertEqual(rows[0].hosts, "[host]:2222,other")
        XCTAssertEqual(rows[0].keyType, "ssh-ed25519")
        XCTAssertEqual(rows[0].keyBase64, "AAAAAAAAAAAAAAAA")
        XCTAssertEqual(rows[0].sourceLine, 2)
        XCTAssertEqual(rows[0].byteStart, prefix.utf8.count)
        XCTAssertEqual(rows[0].byteEnd, (prefix + line).utf8.count)
        XCTAssertTrue(rows[0].recognized)
        XCTAssertFalse(rows[0].hashed)
        XCTAssertTrue(rows[1].hashed)
        XCTAssertFalse(rows[2].recognized)
        XCTAssertEqual(rows[2].hosts, "broken")
        XCTAssertEqual(rows[2].byteEnd, text.utf8.count)
    }
    func testEmptyAndInvalidAreDistinct() throws {
        XCTAssertEqual(try KnownHosts.records("# nothing\n\t\n"), [])
        XCTAssertThrowsError(try KnownHosts.records("a\0b"))
        XCTAssertThrowsError(try KnownHosts.records(String(repeating: "x", count: 1024 * 1024 + 1)))
        XCTAssertThrowsError(try KnownHosts.records(String(repeating: "broken\n", count: 4097)))
    }
}
