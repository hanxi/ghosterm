import Foundation

/// Swift 测试宿主会重新设置系统临时目录；测试数据显式锚定工程 tmp。
let workspaceTestTmp = URL(fileURLWithPath: #filePath)
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("tmp", isDirectory: true).path + "/"
