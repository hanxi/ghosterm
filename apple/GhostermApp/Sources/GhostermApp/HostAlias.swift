// HostAlias — sheet(item:) 需要 Identifiable；直接给 String 加扩展是
// retroactive conformance（Swift 会警告，且未来可能冲突），所以包一层。
import Foundation

struct HostAlias: Identifiable, Hashable {
    let name: String
    var id: String { name }
    init(_ name: String) { self.name = name }
}
