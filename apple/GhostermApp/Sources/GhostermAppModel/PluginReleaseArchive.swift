import CryptoKit
import Darwin
import Foundation
import zlib

/// Candidate ZIP32 profile shared with the offline developer command. Parse all
/// bounds and paths before writing; never delegate traversal to an unzip tool.
enum PluginReleaseArchive {
    static let limit = 75_497_472
    enum Failure: String, Error, LocalizedError {
        case artifact, directory, layout, path, content, budget, io
        var errorDescription: String? {
            switch self {
            case .artifact: return "ZIP 字节数或摘要与签名描述不一致。"
            case .directory, .layout: return "ZIP 结构不受支持或存在未声明范围。"
            case .path: return "ZIP 含非法路径、重复成员、大小写冲突或非普通文件。"
            case .content: return "ZIP 内容、CRC 或压缩流校验失败。"
            case .budget: return "ZIP 条目或解压大小超过上限。"
            case .io: return "无法写入安装暂存目录。"
            }
        }
    }
    struct Member {
        let path: String
        let offset: Int, compressed: Int, count: Int
        let method: Int, crc: UInt32
        let executable: Bool
    }
    static func inspect(_ data: Data, proof: PluginDirectorySignature) throws -> [Member] {
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        guard data.count <= limit, UInt64(data.count) == proof.artifactBytes, digest == proof.artifactSHA256 else { throw Failure.artifact }
        let input = Bytes(data: data), end = data.count - 22
        guard end >= 0, try input.u32(end) == 0x06054b50,
              try input.u16(end + 4) == 0, try input.u16(end + 6) == 0,
              try input.u16(end + 20) == 0 else { throw Failure.directory }
        let count = try input.u16(end + 10), centralSize = try input.int32(end + 12), central = try input.int32(end + 16)
        guard (1...1024).contains(count), try input.u16(end + 8) == count,
              count * 46 <= centralSize, centralSize <= count * (46 + 4096), central + centralSize == end else { throw Failure.budget }
        var at = central, headers: [(Member, Int, Data, Int)] = [], folded = Set<String>(), total = 0
        for _ in 0..<count {
            guard try input.u32(at) == 0x02014b50 else { throw Failure.directory }
            let flags = try input.u16(at + 8), method = try input.u16(at + 10), crc = try input.u32(at + 16)
            let compressed = try input.int32(at + 20), bytes = try input.int32(at + 24), namesize = try input.u16(at + 28)
            let external = try input.u32(at + 38), offset = try input.int32(at + 42)
            guard (1...4096).contains(namesize), try input.u16(at + 30) == 0,
                  try input.u16(at + 32) == 0, try input.u16(at + 34) == 0 else { throw Failure.directory }
            let raw = try input.span(at + 46, namesize)
            guard flags & ~0x800 == 0, flags & 0x800 != 0 || raw.allSatisfy({ $0 < 128 }),
                  let name = String(data: raw, encoding: .utf8), validPath(name),
                  folded.insert(name.lowercased()).inserted, external & 16 == 0,
                  [UInt32(0), UInt32(S_IFREG)].contains((external >> 16) & UInt32(S_IFMT)) else { throw Failure.path }
            guard [0, 8].contains(method), bytes <= 16_777_216, compressed <= limit else { throw Failure.budget }
            total += bytes
            guard total <= 67_108_864 else { throw Failure.budget }
            headers.append((Member(path: name, offset: offset, compressed: compressed, count: bytes,
                method: method, crc: crc, executable: (external >> 16) & 0o111 != 0), flags, raw, offset))
            at += 46 + namesize
        }
        guard at == end else { throw Failure.directory }
        // A member may not also be a parent directory. Compare normalized names
        // before extraction so file/dir case collisions cannot become IO races.
        for name in folded {
            let components = name.split(separator: "/")
            for length in 1..<components.count {
                guard !folded.contains(components.prefix(length).joined(separator: "/")) else { throw Failure.path }
            }
        }
        var cursor = 0, result: [Member] = []
        for (member, flags, raw, offset) in headers.sorted(by: { $0.3 < $1.3 }) {
            guard offset == cursor, try input.u32(offset) == 0x04034b50,
                  try input.u16(offset + 6) == flags, try input.u16(offset + 8) == member.method,
                  try input.u32(offset + 14) == member.crc,
                  try input.int32(offset + 18) == member.compressed, try input.int32(offset + 22) == member.count,
                  try input.u16(offset + 26) == raw.count, try input.u16(offset + 28) == 0,
                  try input.span(offset + 30, raw.count) == raw else { throw Failure.layout }
            let start = offset + 30 + raw.count
            cursor = start + member.compressed
            guard cursor <= central else { throw Failure.layout }
            result.append(Member(path: member.path, offset: start, compressed: member.compressed,
                count: member.count, method: member.method, crc: member.crc, executable: member.executable))
        }
        guard cursor == central else { throw Failure.layout }
        return result
    }

    static func validPath(_ path: String, maxDepth: Int = 32) -> Bool {
        guard !path.isEmpty, path.utf8.count <= 4096, !path.hasPrefix("/"),
              !path.contains("\\"), !path.contains(":"),
              path.utf8.elementsEqual(path.precomposedStringWithCanonicalMapping.utf8),
              path.unicodeScalars.allSatisfy({ $0.value >= 32 && $0.value != 127 }) else { return false }
        let parts = path.split(separator: "/", omittingEmptySubsequences: false)
        return parts.count <= maxDepth && parts.allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." && !$0.hasSuffix(".") && !$0.hasSuffix(" ") }
    }

    static func contents(_ data: Data, member: Member) throws -> Data {
        let input = try Bytes(data: data).span(member.offset, member.compressed)
        var output: Data
        if member.method == 0 {
            guard input.count == member.count else { throw Failure.content }
            output = input
        } else {
            var stream = z_stream()
            guard inflateInit2_(&stream, -MAX_WBITS, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else { throw Failure.content }
            defer { inflateEnd(&stream) }
            var bytes = [UInt8](repeating: 0, count: member.count + 1)
            let status = input.withUnsafeBytes { source in
                bytes.withUnsafeMutableBytes { target in
                    stream.next_in = UnsafeMutablePointer(mutating: source.bindMemory(to: Bytef.self).baseAddress)
                    stream.avail_in = uInt(source.count)
                    stream.next_out = target.bindMemory(to: Bytef.self).baseAddress
                    stream.avail_out = uInt(target.count)
                    return inflate(&stream, Z_FINISH)
                }
            }
            guard status == Z_STREAM_END, stream.total_in == member.compressed,
                  stream.total_out == member.count else { throw Failure.content }
            output = Data(bytes.prefix(member.count))
        }
        let checksum = output.withUnsafeBytes { crc32(0, $0.bindMemory(to: Bytef.self).baseAddress, uInt($0.count)) }
        guard UInt32(checksum) == member.crc else { throw Failure.content }
        return output
    }

    private struct Bytes {
        let data: Data
        func span(_ at: Int, _ count: Int) throws -> Data {
            guard at >= 0, count >= 0, at <= data.count, count <= data.count - at else { throw Failure.layout }
            return data.subdata(in: at..<(at + count))
        }
        func u16(_ at: Int) throws -> Int {
            let b = try span(at, 2); return Int(b[0]) | Int(b[1]) << 8
        }
        func u32(_ at: Int) throws -> UInt32 {
            let b = try span(at, 4)
            return UInt32(b[0]) | UInt32(b[1]) << 8 | UInt32(b[2]) << 16 | UInt32(b[3]) << 24
        }
        func int32(_ at: Int) throws -> Int { Int(try u32(at)) }
    }
}
