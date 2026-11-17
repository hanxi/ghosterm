// Creates/reuses the deployment key in Keychain. Secret output is restricted to
// an explicitly inherited pipe; stdout contains only the public key.
import CryptoKit
import Foundation
import Security

let arguments = CommandLine.arguments
let account = arguments[1]
let privateFD = Int32(arguments[2])!
let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
    kSecAttrService as String: "https://sparkle-project.org",
    kSecAttrAccount as String: account]
var item: CFTypeRef?
var read = query
read[kSecReturnData as String] = true
let status = SecItemCopyMatching(read as CFDictionary, &item)
let seed: Data
if status == errSecItemNotFound {
    seed = Curve25519.Signing.PrivateKey().rawRepresentation
    var save = query
    save[kSecAttrLabel as String] = "Ghosterm update signing (Ed25519)"
    save[kSecAttrDescription as String] = "Private update key; not an Apple code signing certificate"
    save[kSecValueData as String] = seed.base64EncodedString().data(using: .utf8)!
    let saved = SecItemAdd(save as CFDictionary, nil)
    guard saved == errSecSuccess else {
        fputs("Unable to store update key in Keychain (status \(saved))\n", stderr)
        exit(1)
    }
} else if status == errSecSuccess, let encoded = item as? Data,
          let text = String(data: encoded, encoding: .utf8), let data = Data(base64Encoded: text) {
    seed = data
} else {
    fputs("Unable to access update key in Keychain (status \(status))\n", stderr)
    exit(1)
}
let key = try Curve25519.Signing.PrivateKey(rawRepresentation: seed)
if privateFD >= 3 {
    let pipe = FileHandle(fileDescriptor: privateFD, closeOnDealloc: false)
    try pipe.write(contentsOf: seed.base64EncodedString().data(using: .utf8)!)
    try pipe.close()
}
print(key.publicKey.rawRepresentation.base64EncodedString())
