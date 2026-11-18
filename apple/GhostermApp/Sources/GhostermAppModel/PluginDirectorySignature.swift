import CryptoKit
import Darwin
import Foundation

/// Host policy attestation for an immutable Core-checked directory snapshot.
/// This does not inspect a ZIP, install a package or grant execution permission.
public struct PluginDirectorySignature: Sendable, Equatable {
    public let pluginID: String
    public let packageSHA256: String
    public let channel: String
    public let keyID: String
    public let publisher: String
    public let policySHA256: String
    public let descriptorSHA256: String
    public let artifactSHA256: String
    public let artifactBytes: UInt64
}

public enum PluginDirectorySignatureVerifier {
    public struct Identity: Sendable {
        public let id: String, version: String, publisher: String, packageSHA256: String
        public init(id: String, version: String, publisher: String, packageSHA256: String) {
            self.id = id; self.version = version; self.publisher = publisher; self.packageSHA256 = packageSHA256
        }
    }

    public enum Failure: String, Error, Sendable, LocalizedError {
        case json, duplicateKey, limit, file, changed, policy, descriptor, unknownKey, revokedKey, scope, signature, identity
        case channelStatement, channelFreshness, channelHistory
        public var errorDescription: String? {
            switch self {
            case .json, .duplicateKey: return "签名描述或渠道策略不是有效且无重复字段的 JSON。"
            case .limit: return "签名材料超过读取或结构上限。"
            case .file: return "无法读取所选签名材料；须选择普通文件，不能是链接。"
            case .changed: return "所选签名材料在读取期间发生变化，请重新核对。"
            case .policy: return "渠道公钥策略无效。"
            case .descriptor: return "发布描述的格式无效。"
            case .unknownKey: return "所选渠道策略不认识此签名密钥。"
            case .revokedKey: return "此签名密钥已被所选策略撤销。"
            case .scope: return "渠道、发布者或插件不在所选密钥的授权范围内。"
            case .signature: return "发布签名不匹配。"
            case .identity: return "发布描述与当前已审查的目录包摘要或身份不一致。"
            case .channelStatement: return "渠道策略声明格式、签名或摘要无效。"
            case .channelFreshness: return "渠道策略尚未生效或已过期。"
            case .channelHistory: return "渠道根、策略代次或系统时间与已记录状态不一致。"
            }
        }
    }

    struct ChannelEnvelope: Codable, Sendable {
        let statement: Data
        let signature: Data
        let policy: Data
    }

    public struct VerifiedChannelPolicy: Sendable {
        public let channel: String
        public let generation: UInt64
        public let issuedAt: UInt64
        public let expiresAt: UInt64
        public let statementSHA256: String
        let rootSHA256: String
        let envelope: ChannelEnvelope
        var policyBytes: Data { envelope.policy }
    }

    public static func verifyChannelPolicy(statement suppliedStatement: Data, signature suppliedSignature: Data, policy suppliedPolicy: Data,
                                           channel: String, channelRoot: Data, now: UInt64) throws -> VerifiedChannelPolicy {
        guard suppliedStatement.count <= 65_536, suppliedPolicy.count <= 131_072 else { throw Failure.limit }
        guard channelRoot.count == 32, suppliedSignature.count == 64 else { throw Failure.channelStatement }
        let statement = Data(Array(suppliedStatement)), signature = Data(Array(suppliedSignature))
        let policyBytes = Data(Array(suppliedPolicy))
        let fields = try ReleaseJSON.parse(statement).object()
        guard exact(fields, ["schemaVersion", "channel", "generation", "issuedAt", "expiresAt", "policySHA256"]),
              try fields["schemaVersion"]?.integer(maximum: 1) == 1,
              try fields["channel"]?.string() == channel,
              try fields["policySHA256"]?.string() == hash(policyBytes),
              let generation = try positiveDecimal(fields["generation"]),
              let issuedAt = try positiveDecimal(fields["issuedAt"]),
              let expiresAt = try positiveDecimal(fields["expiresAt"]) else { throw Failure.channelStatement }
        guard try policy(policyBytes).channel == channel else { throw Failure.scope }
        var message = Data("ghosterm.channel-policy.v1\0".utf8)
        message.append(statement)
        let key = try Curve25519.Signing.PublicKey(rawRepresentation: channelRoot)
        guard key.isValidSignature(signature, for: message) else { throw Failure.channelStatement }
        guard issuedAt <= now, now < expiresAt else { throw Failure.channelFreshness }
        return VerifiedChannelPolicy(channel: channel, generation: generation, issuedAt: issuedAt,
                                     expiresAt: expiresAt, statementSHA256: hash(statement),
                                     rootSHA256: hash(key.rawRepresentation),
                                     envelope: ChannelEnvelope(statement: statement, signature: signature, policy: policyBytes))
    }

    public static func verify(descriptor: Data, signature: Data, channelPolicy: VerifiedChannelPolicy,
                              identity: Identity, now: UInt64, stateDirectory: URL? = nil) throws -> PluginDirectorySignature {
        guard channelPolicy.issuedAt <= now, now < channelPolicy.expiresAt else { throw Failure.channelFreshness }
        if let stateDirectory {
            guard let catalog = try PluginReleaseCatalog(directory: stateDirectory, exclusive: true) else {
                throw PluginReleaseCatalog.Failure.root
            }
            try catalog.recordChannelPolicy(channelPolicy, now: now)
        }
        return try verify(descriptor: descriptor, signature: signature, trustedPolicy: channelPolicy.policyBytes, identity: identity)
    }

    public static func refreshChannelPolicy(statement: Data, signature: Data, policy: Data,
                                            channel: String, channelRoot: Data, now: UInt64,
                                            stateDirectory: URL) throws -> VerifiedChannelPolicy {
        let verified = try verifyChannelPolicy(statement: statement, signature: signature, policy: policy,
            channel: channel, channelRoot: channelRoot, now: now)
        guard let catalog = try PluginReleaseCatalog(directory: stateDirectory, exclusive: true) else {
            throw PluginReleaseCatalog.Failure.root
        }
        try catalog.recordChannelPolicy(verified, now: now)
        return verified
    }

    public static func loadChannelPolicy(channel: String, channelRoot: Data, now: UInt64,
                                         stateDirectory: URL) throws -> VerifiedChannelPolicy {
        guard let catalog = try PluginReleaseCatalog(directory: stateDirectory, exclusive: true) else {
            throw PluginReleaseCatalog.Failure.root
        }
        return try loadChannelPolicy(channel: channel, channelRoot: channelRoot, now: now, catalog: catalog)
    }

    static func loadChannelPolicy(channel: String, channelRoot: Data, now: UInt64,
                                  catalog: PluginReleaseCatalog) throws -> VerifiedChannelPolicy {
        let cached = try catalog.readChannelEnvelope(channel)
        let envelope = cached.envelope
        let policy = try verifyChannelPolicy(statement: envelope.statement, signature: envelope.signature,
            policy: envelope.policy, channel: channel, channelRoot: channelRoot, now: now)
        guard policy.generation == cached.generation, policy.statementSHA256 == cached.digest else {
            throw Failure.channelHistory
        }
        try catalog.recordChannelPolicy(policy, now: now, requireHistory: true)
        return policy
    }

    private static func positiveDecimal(_ field: ReleaseJSON.Value?) throws -> UInt64? {
        guard let value = try field?.string(), matches(value, "^[1-9][0-9]{0,19}$") else { return nil }
        return UInt64(value)
    }

    public static func verify(descriptor: Data, signature: Data, trustedPolicy: Data,
                              identity: Identity) throws -> PluginDirectorySignature {
        let authenticated = try authenticate(descriptor: descriptor, signature: signature, trustedPolicy: trustedPolicy)
        return try authenticated.bind(identity)
    }

    struct Authenticated {
        let identity: Identity
        let proof: PluginDirectorySignature
        func bind(_ actual: Identity) throws -> PluginDirectorySignature {
            guard same(identity.id, actual.id), same(identity.version, actual.version),
                  same(identity.publisher, actual.publisher), identity.packageSHA256 == actual.packageSHA256 else { throw Failure.identity }
            return proof
        }
    }

    static func declaration(_ descriptor: Data) throws -> Identity {
        guard descriptor.count <= 65536 else { throw Failure.limit }
        let value = try ReleaseJSON.parse(descriptor).object()
        guard let id = try value["pluginId"]?.string(), pluginID(id),
              let version = try value["pluginVersion"]?.string(), text(version),
              let publisher = try value["publisher"]?.string(), text(publisher),
              let sha = try value["packageSHA256"]?.string(), digest(sha) else { throw Failure.descriptor }
        return Identity(id: id, version: version, publisher: publisher, packageSHA256: sha)
    }

    // Internal preflight used by the ZIP installer; a directory proof is only
    // returned publicly after binding the authenticated claims to Core metadata.
    static func authenticate(descriptor: Data, signature: Data, trustedPolicy: Data) throws -> Authenticated {
        // Own byte copies keep parsing, signature verification and receipt hashes
        // tied to the same input even when a caller used bridged mutable NSData.
        guard descriptor.count <= 65536, trustedPolicy.count <= 131072, signature.count == 64 else { throw Failure.limit }
        let payload = Data(Array(descriptor)), policyBytes = Data(Array(trustedPolicy)), signatureBytes = Data(Array(signature))
        let selectedPolicy = try policy(policyBytes)
        let channel = selectedPolicy.channel, keys = selectedPolicy.keys, revokedIDs = selectedPolicy.revokedIDs

        let release = try ReleaseJSON.parse(payload).object()
        guard exact(release, ["schemaVersion", "channel", "keyId", "pluginId", "pluginVersion", "publisher", "packageSHA256", "artifact"]),
              try release["schemaVersion"]?.integer(maximum: 1) == 1,
              let requestedChannel = try release["channel"]?.string(), matches(requestedChannel, "^[a-z0-9][a-z0-9.-]{0,63}$"),
              let keyID = try release["keyId"]?.string(), digest(keyID),
              let id = try release["pluginId"]?.string(), pluginID(id),
              let version = try release["pluginVersion"]?.string(), text(version),
              let publisher = try release["publisher"]?.string(), text(publisher),
              let package = try release["packageSHA256"]?.string(), digest(package),
              let artifact = try release["artifact"]?.object(), exact(artifact, ["format", "bytes", "sha256"]),
              try artifact["format"]?.string() == "zip-v1",
              let count = try artifact["bytes"]?.integer(maximum: 75497472), count >= 22,
              let artifactSHA = try artifact["sha256"]?.string(), digest(artifactSHA) else { throw Failure.descriptor }
        guard requestedChannel == channel else { throw Failure.scope }
        guard let key = keys[keyID] else { throw Failure.unknownKey }
        guard !revokedIDs.contains(keyID) else { throw Failure.revokedKey }
        guard same(publisher, key.publisher), key.plugins.contains(id) else { throw Failure.scope }
        var message = Data("GHOSTERM-PLUGIN-RELEASE-V1\0".utf8)
        message.append(payload)
        let publicKey = try Curve25519.Signing.PublicKey(rawRepresentation: key.raw)
        guard publicKey.isValidSignature(signatureBytes, for: message) else { throw Failure.signature }
        let proof = PluginDirectorySignature(pluginID: id, packageSHA256: package, channel: channel, keyID: keyID,
            publisher: publisher, policySHA256: hash(policyBytes), descriptorSHA256: hash(payload),
            artifactSHA256: artifactSHA, artifactBytes: count)
        return Authenticated(identity: .init(id: id, version: version, publisher: publisher, packageSHA256: package), proof: proof)
    }

    struct Policy {
        let channel: String
        let keys: [String: Key]
        let revokedIDs: [String]
    }
    static func policy(_ policyBytes: Data) throws -> Policy {
        guard policyBytes.count <= 131072 else { throw Failure.limit }
        let policy = try ReleaseJSON.parse(policyBytes).object()
        guard exact(policy, ["schemaVersion", "channel", "keys", "revokedKeyIds"]),
              try policy["schemaVersion"]?.integer(maximum: 1) == 1,
              let channel = try policy["channel"]?.string(), matches(channel, "^[a-z0-9][a-z0-9.-]{0,63}$"),
              let entries = try policy["keys"]?.array(), (1...64).contains(entries.count),
              let revoked = try policy["revokedKeyIds"]?.array(), revoked.count <= 256 else { throw Failure.policy }
        var keys: [String: Key] = [:]
        for entry in entries {
            let object = try entry.object()
            guard exact(object, ["id", "algorithm", "publicKey", "publisher", "pluginIds"]),
                  try object["algorithm"]?.string() == "ed25519",
                  let id = try object["id"]?.string(), digest(id), keys[id] == nil,
                  let encoded = try object["publicKey"]?.string(), let raw = Data(base64Encoded: encoded),
                  raw.count == 32, raw.base64EncodedString() == encoded, hash(raw) == id,
                  let publisher = try object["publisher"]?.string(), text(publisher),
                  let plugins = try object["pluginIds"]?.array(), (1...64).contains(plugins.count) else { throw Failure.policy }
            let ids = try plugins.map { try $0.string() }
            guard Set(ids).count == ids.count, ids.allSatisfy(pluginID) else { throw Failure.policy }
            keys[id] = Key(raw: raw, publisher: publisher, plugins: Set(ids))
        }
        let revokedIDs = try revoked.map { try $0.string() }
        guard Set(revokedIDs).count == revokedIDs.count, revokedIDs.allSatisfy(digest) else { throw Failure.policy }

        return Policy(channel: channel, keys: keys, revokedIDs: revokedIDs)
    }
    struct Key { let raw: Data; let publisher: String; let plugins: Set<String> }
    private static func hash(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    private static func same(_ a: String, _ b: String) -> Bool { a.utf8.elementsEqual(b.utf8) }
    private static func exact(_ object: [String: ReleaseJSON.Value], _ keys: Set<String>) -> Bool { Set(object.keys) == keys }
    private static func matches(_ value: String, _ pattern: String) -> Bool {
        value.range(of: pattern, options: .regularExpression) == value.startIndex..<value.endIndex
    }
    private static func digest(_ value: String) -> Bool { matches(value, "^[0-9a-f]{64}$") }
    private static func pluginID(_ value: String) -> Bool { value.utf8.count <= 128 && matches(value, "^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+$") }
    private static func text(_ value: String) -> Bool {
        (1...128).contains(value.unicodeScalars.count) && same(value, value.precomposedStringWithCanonicalMapping) &&
            value.unicodeScalars.allSatisfy { $0.value >= 32 && $0.value != 127 }
    }
}

/// Small bounded host-policy decoder. Preserve number lexemes and decoded keys:
/// Foundation dictionaries alone discard duplicate keys and round large numbers.
struct ReleaseJSON {
    typealias Failure = PluginDirectorySignatureVerifier.Failure
    indirect enum Value {
        case object([String: Value]), array([Value]), string(String), number(String), boolean(Bool), other
        func object() throws -> [String: Value] { guard case let .object(v) = self else { throw Failure.json }; return v }
        func array() throws -> [Value] { guard case let .array(v) = self else { throw Failure.json }; return v }
        func string() throws -> String { guard case let .string(v) = self else { throw Failure.json }; return v }
        func bool() throws -> Bool { guard case let .boolean(v) = self else { throw Failure.json }; return v }
        func integer(maximum: UInt64) throws -> UInt64? {
            guard case let .number(raw) = self else { return nil }
            var value = raw.lowercased()
            let negative = value.hasPrefix("-")
            if negative { value.removeFirst() }
            let parts = value.split(separator: "e", omittingEmptySubsequences: false)
            let exponent = parts.count == 2 ? Int(parts[1]) : 0
            guard let exponent, (-1_000_000...1_000_000).contains(exponent) else { return nil }
            let decimal = parts[0].split(separator: ".", omittingEmptySubsequences: false)
            var digits = Array(decimal.joined().utf8)
            guard let nonzero = digits.firstIndex(where: { $0 != 48 }) else { return 0 }
            digits.removeFirst(nonzero)
            guard !negative else { return nil }
            let scale = exponent - (decimal.count == 2 ? decimal[1].utf8.count : 0)
            if scale < 0 {
                guard -scale <= digits.count, digits.suffix(-scale).allSatisfy({ $0 == 48 }) else { return nil }
                digits.removeLast(-scale)
            } else {
                guard digits.count + scale <= 20 else { return nil }
                digits.append(contentsOf: repeatElement(48, count: scale))
            }
            guard digits.count <= 20 else { return nil }
            var result: UInt64 = 0
            for digit in digits {
                guard result <= maximum / 10 else { return nil }
                result = result * 10 + UInt64(digit - 48)
                guard result <= maximum else { return nil }
            }
            return result
        }
    }
    private let bytes: [UInt8]
    private var at = 0, nodes = 0
    static func parse(_ data: Data) throws -> Value {
        guard data.count <= 131072, String(data: data, encoding: .utf8) != nil else { throw Failure.json }
        var parser = ReleaseJSON(bytes: Array(data))
        let result = try parser.value(depth: 0)
        parser.space()
        guard parser.at == parser.bytes.count else { throw Failure.json }
        return result
    }
    private mutating func space() { while at < bytes.count && [9, 10, 13, 32].contains(bytes[at]) { at += 1 } }
    private mutating func take(_ byte: UInt8) -> Bool {
        space(); guard at < bytes.count, bytes[at] == byte else { return false }; at += 1; return true
    }
    private mutating func string() throws -> String {
        space(); let start = at
        guard at < bytes.count, bytes[at] == 34 else { throw Failure.json }; at += 1
        while at < bytes.count {
            let byte = bytes[at]; at += 1
            if byte == 34 {
                guard let result = try JSONSerialization.jsonObject(with: Data(bytes[start..<at]), options: .fragmentsAllowed) as? String else { throw Failure.json }
                return result
            }
            if byte == 92 { guard at < bytes.count else { throw Failure.json }; at += 1 }
            else if byte < 32 { throw Failure.json }
        }
        throw Failure.json
    }
    private mutating func value(depth: Int) throws -> Value {
        nodes += 1
        guard depth <= 32, nodes <= 16384 else { throw Failure.limit }
        space(); guard at < bytes.count else { throw Failure.json }
        if bytes[at] == 34 { return .string(try string()) }
        if take(123) {
            var object: [String: Value] = [:]
            if take(125) { return .object(object) }
            repeat {
                let key = try string()
                guard object[key] == nil, !["__proto__", "prototype", "constructor"].contains(key) else { throw Failure.duplicateKey }
                guard take(58) else { throw Failure.json }
                object[key] = try value(depth: depth + 1)
                if take(125) { return .object(object) }
                guard take(44) else { throw Failure.json }
            } while true
        }
        if take(91) {
            var array: [Value] = []
            if take(93) { return .array(array) }
            repeat {
                array.append(try value(depth: depth + 1))
                if take(93) { return .array(array) }
                guard take(44) else { throw Failure.json }
            } while true
        }
        for word in ["true", "false", "null"] {
            let encoded = Array(word.utf8)
            if bytes[at...].starts(with: encoded) {
                at += encoded.count
                if word == "true" { return .boolean(true) }
                if word == "false" { return .boolean(false) }
                return .other
            }
        }
        let start = at
        if bytes[at] == 45 { at += 1 }
        guard at < bytes.count else { throw Failure.json }
        if bytes[at] == 48 { at += 1 }
        else {
            guard (49...57).contains(bytes[at]) else { throw Failure.json }
            while at < bytes.count && (48...57).contains(bytes[at]) { at += 1 }
        }
        if at < bytes.count && bytes[at] == 46 {
            at += 1; let digits = at
            while at < bytes.count && (48...57).contains(bytes[at]) { at += 1 }
            guard at > digits else { throw Failure.json }
        }
        if at < bytes.count && [69, 101].contains(bytes[at]) {
            at += 1
            if at < bytes.count && [43, 45].contains(bytes[at]) { at += 1 }
            let digits = at
            while at < bytes.count && (48...57).contains(bytes[at]) { at += 1 }
            guard at > digits else { throw Failure.json }
        }
        return .number(String(decoding: bytes[start..<at], as: UTF8.self))
    }
}

/// Bounded snapshots of explicitly selected public files. No package-relative
/// policy discovery: a policy supplied by the package is not a trust anchor.
enum PluginSignatureFile {
    static func read(_ url: URL, limit: Int) throws -> Data {
        typealias Failure = PluginDirectorySignatureVerifier.Failure
        guard url.isFileURL, !url.path.isEmpty, url.path.utf8.count <= 4096,
              !url.path.utf8.contains(0) else { throw Failure.file }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let fd = url.path.withCString { open($0, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC) }
        guard fd >= 0 else { throw Failure.file }
        defer { _ = Darwin.close(fd) }
        var before = stat(), after = stat()
        guard fstat(fd, &before) == 0, before.st_mode & S_IFMT == S_IFREG else { throw Failure.file }
        guard before.st_size >= 0, before.st_size <= limit else { throw Failure.limit }
        var result = Data(), buffer = [UInt8](repeating: 0, count: 8192)
        while true {
            let count = buffer.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
            if count < 0 && errno == EINTR { continue }
            guard count >= 0 else { throw Failure.file }
            if count == 0 { break }
            guard result.count <= limit - count else { throw Failure.limit }
            result.append(contentsOf: buffer.prefix(count))
        }
        guard fstat(fd, &after) == 0,
              before.st_dev == after.st_dev, before.st_ino == after.st_ino,
              before.st_size == after.st_size, result.count == before.st_size,
              before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec,
              before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec,
              before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec,
              before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec else { throw Failure.changed }
        return result
    }
}
