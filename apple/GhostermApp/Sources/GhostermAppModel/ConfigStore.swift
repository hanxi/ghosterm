// ConfigStore — 「文件即真相」的写侧（铁律 #4）。
//
// 职责（严格）：
//   - 写回前强制备份（同一目录，config.bak）；
//   - 冲突检测：load 时记录 mtime/size，save 前核对——外部改过就拒绝写
//     （GT_ERR 冲突显式报错，不静默覆盖别人）；
//   - 原子写：同目录临时文件 + rename（半份配置永远不会出现在磁盘上）。
//
// 编辑语义（稀疏补丁、无损写回）在 core 的 gt_config_edit；本类型只管
// 「把编辑结果安全落盘」这一件事。
import Foundation
import GhostermCore

public enum ConfigStoreError: Error, CustomStringConvertible {
    case conflict(String)
    case io(String)

    public var description: String {
        switch self {
        case .conflict(let detail):
            return "配置文件已被外部修改（\(detail)）——请重新加载后再编辑"
        case .io(let detail):
            return "写入失败：\(detail)"
        }
    }
}

public final class ConfigStore {
    private let path: String
    private var loadedStat: (mtime: TimeInterval, size: Int)?

    public init(path: String) {
        self.path = path
        loadedStat = ConfigStore.statOf(path)
    }

    /// 当前文件状态快照（文件不存在 = 空）。
    private static func statOf(_ path: String) -> (mtime: TimeInterval, size: Int)? {
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: path),
              let mtime = attrs[.modificationDate] as? Date,
              let size = attrs[.size] as? Int else { return nil }
        return (mtime.timeIntervalSince1970, size)
    }

    /// 文件自 load 后是否被外部修改。
    public func isExternallyModified() -> Bool {
        guard let loaded = loadedStat else {
            return ConfigStore.statOf(path) != nil // 原来不存在，现在有了
        }
        guard let now = ConfigStore.statOf(path) else { return true }
        return now.mtime != loaded.mtime || now.size != loaded.size
    }

    /// 安全写回：冲突检测 → 备份 → 原子替换。成功后刷新内部快照。
    public func save(_ text: String) throws {
        if isExternallyModified() {
            throw ConfigStoreError.conflict("mtime/size 与加载时不一致")
        }
        let fm = FileManager.default
        let dir = (path as NSString).deletingLastPathComponent
        do {
            // 1. 备份（首次编辑前留一份原文件；备份失败不阻断——旧备份总比没有强）。
            if fm.fileExists(atPath: path) {
                let backup = (path as NSString).deletingPathExtension + ".bak"
                try? fm.removeItem(atPath: backup)
                try? fm.copyItem(atPath: path, toPath: backup)
            }
            // 2. 原子写：同目录临时文件 + rename。
            let tmp = dir + "/.\((path as NSString).lastPathComponent).tmp-\(UUID().uuidString)"
            try text.write(toFile: tmp, atomically: true, encoding: .utf8)
            // rename 不覆盖已存在目标（Darwin），故目标存在才需要先移除。
            if fm.fileExists(atPath: path) {
                try fm.removeItem(atPath: path)
            }
            try fm.moveItem(atPath: tmp, toPath: path)
            loadedStat = ConfigStore.statOf(path)
        } catch {
            throw ConfigStoreError.io("\(error)")
        }
    }
}
