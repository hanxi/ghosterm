import Cghosterm
import Foundation

/// Crypto-only C ABI adapter. Success does not validate policy or package identity.
public enum ReleaseSignature {
    public static func verify(publicKey: Data, signature: Data, descriptor: Data) -> Int32 {
        guard publicKey.count == 32, signature.count == 64, (1...65536).contains(descriptor.count) else { return 1 }
        let key = Data(Array(publicKey)), signed = Data(Array(signature)), bytes = Data(Array(descriptor))
        return key.withUnsafeBytes { k in signed.withUnsafeBytes { s in bytes.withUnsafeBytes { d in
            Int32(gt_ext_release_verify(k.bindMemory(to: UInt8.self).baseAddress, k.count,
                s.bindMemory(to: UInt8.self).baseAddress, s.count, d.bindMemory(to: UInt8.self).baseAddress, d.count).rawValue)
        } } }
    }
    public static func verifyChannelPolicy(publicKey: Data, signature: Data, statement: Data) -> Int32 {
        guard publicKey.count == 32, signature.count == 64, (1...65536).contains(statement.count) else { return 1 }
        let key = Data(Array(publicKey)), signed = Data(Array(signature)), bytes = Data(Array(statement))
        return key.withUnsafeBytes { k in signed.withUnsafeBytes { s in bytes.withUnsafeBytes { d in
            Int32(gt_ext_channel_policy_verify(k.bindMemory(to: UInt8.self).baseAddress, k.count,
                s.bindMemory(to: UInt8.self).baseAddress, s.count, d.bindMemory(to: UInt8.self).baseAddress, d.count).rawValue)
        } } }
    }
}
