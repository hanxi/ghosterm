// FilePaths — 本地落点的约定路径（面板 / 传输队列共用）。
import Foundation

public enum FilePaths {
    /// 下载落点的默认建议：~/Downloads/<name>。
    public static func defaultDownloadPath(for name: String) -> String {
        let downloads = (NSHomeDirectory() as NSString).appendingPathComponent("Downloads")
        return (downloads as NSString).appendingPathComponent(name)
    }
}
