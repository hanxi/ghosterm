// QuickLookPaths — 预览临时副本的路径（纯函数，跨平台可断言）。
import Foundation

public enum QuickLookPaths {
    public static func tempPreviewPath(for name: String,
                                       token: String = UUID().uuidString) -> String {
        var dir = NSTemporaryDirectory()
        if dir.hasSuffix("/") { dir.removeLast() }
        return dir + "/ghosterm-preview/" + token + "/" + name
    }
}
