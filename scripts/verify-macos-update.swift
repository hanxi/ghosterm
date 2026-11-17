// Independent Ed25519 verification against the key embedded in the app.
import CryptoKit
import Foundation

let args = CommandLine.arguments
let key = try Curve25519.Signing.PublicKey(rawRepresentation: Data(base64Encoded: args[1])!)
let signature = Data(base64Encoded: args[2])!
let archive = try Data(contentsOf: URL(fileURLWithPath: args[3]), options: .mappedIfSafe)
guard key.isValidSignature(signature, for: archive) else {
    fputs("Update signature does not match the embedded public key\n", stderr)
    exit(1)
}
