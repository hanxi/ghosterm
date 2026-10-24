// Trusted application payload metadata, loaded/hashed off UI. No terminal data
// or credentials are read here. Runtime paths are immutable release directories.
import CryptoKit
import Darwin
import Foundation

public struct SessionRuntimeCatalog {
    public enum Failure: Swift.Error, LocalizedError {
        case unavailable, corrupt, unsupported
        public var errorDescription: String? {
            switch self {
            case .unavailable: return "会话运行环境缺失或无法读取，请重新安装应用"
            case .corrupt: return "会话运行环境校验失败，请重新安装应用"
            case .unsupported: return "此会话的后端版本或平台不受当前应用支持"
            }
        }
    }
    private struct Index: Decodable { let format: Int; let contract, local: String; let linux: [String: String] }
    private struct Mac: Decodable { let format: Int; let platform, arch, contract, release: String; let files: [String: String] }
    private static let localFiles: Set<String> = ["ghosterm-sessiond", "ghosterm-session-proxy", "lib/libghostty-vt.dylib", "LICENSE", "SOURCE-INFO.json"]
    private static let linuxFiles: Set<String> = ["ghosterm-sessiond", "ghosterm-session-proxy", "lib/libghostty-vt.so.0", "LICENSE", "SOURCE-INFO.json"]
    public let contract: String
    public let localDirectory: URL
    public let linuxBundles: [String: URL]
    public var compatibility: [UInt8] {
        stride(from: 0, to: contract.count, by: 2).map { n in
            let start = contract.index(contract.startIndex, offsetBy: n)
            return UInt8(contract[start..<contract.index(start, offsetBy: 2)], radix: 16)!
        }
    }
    public func requireCompatible(_ record: SessionRecord) throws {
        guard record.endpoint.contract == contract else { throw Failure.unsupported }
    }
    private static func token(_ value: String) -> Bool {
        !value.isEmpty && value.utf8.count <= 128 && value != "." && value != ".." &&
        value.utf8.allSatisfy { (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || [45, 46, 95].contains($0) }
    }
    private static func hex(_ value: String) -> Bool {
        value.utf8.count == 64 && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
    private static func bytes(_ path: URL, limit: Int) throws -> Data {
        let fd = Darwin.open(path.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
        guard fd >= 0 else { throw Failure.unavailable }; defer { Darwin.close(fd) }
        var st = stat()
        guard fstat(fd, &st) == 0, (st.st_mode & mode_t(S_IFMT)) == mode_t(S_IFREG), st.st_size >= 0,
              st.st_size <= limit, (st.st_uid == 0 || st.st_uid == geteuid()), st.st_mode & 0o022 == 0 else { throw Failure.corrupt }
        var data = Data(), chunk = [UInt8](repeating: 0, count: 65536)
        while true {
            let n = chunk.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
            if n < 0 { if errno == EINTR { continue }; throw Failure.unavailable }
            if n == 0 { break }
            guard data.count <= limit - n else { throw Failure.corrupt }
            data.append(contentsOf: chunk.prefix(n))
        }
        return data
    }
    private static func verify(_ path: URL, sha: String, size: Int? = nil) throws {
        guard hex(sha) else { throw Failure.corrupt }
        // Payload is application-signed, not network input; stream hashes to
        // avoid loading any runtime file as one UI-sized allocation.
        let fd = Darwin.open(path.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
        guard fd >= 0 else { throw Failure.unavailable }; defer { Darwin.close(fd) }
        var st = stat()
        guard fstat(fd, &st) == 0, (st.st_mode & mode_t(S_IFMT)) == mode_t(S_IFREG), st.st_size >= 0,
              st.st_size <= 512 * 1024 * 1024, (st.st_uid == 0 || st.st_uid == geteuid()), st.st_mode & 0o022 == 0,
              size == nil || st.st_size == size! else { throw Failure.corrupt }
        var hash = SHA256(), count = 0, chunk = [UInt8](repeating: 0, count: 65536)
        while true {
            let n = chunk.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
            if n < 0 { if errno == EINTR { continue }; throw Failure.unavailable }
            if n == 0 { break }
            guard count <= 512 * 1024 * 1024 - n else { throw Failure.corrupt }
            hash.update(data: Data(chunk.prefix(n))); count += n
        }
        let actual = hash.finalize().map { String(format: "%02x", $0) }.joined()
        guard actual == sha, count == st.st_size else { throw Failure.corrupt }
    }
    public static func load(index: URL, macosRoot: URL, linuxRoot: URL) throws -> SessionRuntimeCatalog {
        precondition(!Thread.isMainThread)
        let decoder = JSONDecoder()
        let table: Index, native: Mac
        do { table = try decoder.decode(Index.self, from: bytes(index, limit: 8192)) }
        catch let error as Failure { throw error } catch { throw Failure.corrupt }
        guard table.format == 1, hex(table.contract), token(table.local),
              table.linux.keys.allSatisfy({ ["aarch64", "x86_64"].contains($0) }), table.linux.values.allSatisfy(token) else { throw Failure.unsupported }
        let directory = macosRoot.appendingPathComponent(table.local)
        do { native = try decoder.decode(Mac.self, from: bytes(directory.appendingPathComponent("runtime.json"), limit: 16384)) }
        catch let error as Failure { throw error } catch { throw Failure.corrupt }
        guard native.format == 1, native.platform == "macos", native.arch == "arm64", native.contract == table.contract,
              native.release == table.local, Set(native.files.keys) == localFiles else { throw Failure.unsupported }
        for (name, sha) in native.files { try verify(directory.appendingPathComponent(name), sha: sha) }
        var bundles: [String: URL] = [:]
        for (arch, release) in table.linux {
            let bundle = linuxRoot.appendingPathComponent(arch).appendingPathComponent(release)
            let text = String(decoding: try bytes(bundle.appendingPathComponent("manifest.tsv"), limit: 8192), as: UTF8.self)
            let lines = text.split(separator: "\n"), header = lines.first?.split(separator: "\t", omittingEmptySubsequences: false)
            guard lines.count == 6, let header, header.count == 6, header[0] == "GTB1", header[1] == Substring(release),
                  header[2] == Substring(arch), header[5] == Substring(table.contract) else { throw Failure.unsupported }
            var seen = Set<String>()
            for line in lines.dropFirst() {
                let row = line.split(separator: "\t", omittingEmptySubsequences: false)
                guard row.count == 3, linuxFiles.contains(String(row[0])), seen.insert(String(row[0])).inserted,
                      let length = Int(row[1]), length >= 0 else { throw Failure.corrupt }
                try verify(bundle.appendingPathComponent(String(row[0])), sha: String(row[2]), size: length)
            }
            bundles[arch] = bundle
        }
        return SessionRuntimeCatalog(contract: table.contract, localDirectory: directory.resolvingSymlinksInPath(), linuxBundles: bundles)
    }
    public static func loadDefault() throws -> SessionRuntimeCatalog {
        precondition(!Thread.isMainThread)
        let app = Bundle.main.bundleURL
        if app.pathExtension == "app" {
            let resources = app.appendingPathComponent("Contents/Resources/sessiond")
            return try load(index: resources.appendingPathComponent("catalog.json"), macosRoot: resources.appendingPathComponent("macos-arm64"), linuxRoot: resources.appendingPathComponent("linux"))
        }
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        let runtime = root.appendingPathComponent("build/runtime")
        return try load(index: runtime.appendingPathComponent("catalog.json"), macosRoot: runtime.appendingPathComponent("macos-arm64"), linuxRoot: runtime.appendingPathComponent("linux"))
    }
}
