// swift-tools-version:5.9
// GhostermApp — macOS 宿主（SwiftUI）。这是 M1 竖切的顶端。
//
// 依赖方向（AGENTS.md 铁律 #1）：
//   GhostermApp ──► GhostermTerminal（字节 ⇄ 屏幕）
//                └► GhostermCore（transport / ssh_config）
// 本包**不**直接调 libghostty，也**不**直接调 core 的 C 符号：两端都经各自的
// Swift 绑定。SSH 与本地 shell 在 App 层分叉，surface 侧零分叉。
import PackageDescription
import Foundation

let repoRoot = URL(fileURLWithPath: #filePath)
    .deletingLastPathComponent()   // GhostermApp
    .deletingLastPathComponent()   // apple
    .deletingLastPathComponent()   // repo root
let coreBuild = repoRoot.appendingPathComponent("core/build").path
let brewLibDirs = ["/opt/homebrew/lib", "/usr/local/lib"].filter {
    FileManager.default.fileExists(atPath: $0)
}

let package = Package(
    name: "GhostermApp",
    platforms: [.macOS(.v13)],
    dependencies: [
        .package(path: "../GhostermCore"),
        .package(path: "../GhostermTerminal"),
    ],
    targets: [
        // 把业务逻辑放进 library 目标，这样测试与可执行都复用同一份，
        // 且闸门可以在无窗口环境下驱动整条竖切。
        .target(
            name: "GhostermAppModel",
            dependencies: [
                .product(name: "GhostermCore", package: "GhostermCore"),
                .product(name: "GhostermTerminal", package: "GhostermTerminal"),
            ],
            path: "Sources/GhostermAppModel",
            linkerSettings: [
                .unsafeFlags(["-L", coreBuild, "-lghosterm_core", "-lssh2"]
                    + brewLibDirs.flatMap { ["-L", $0] }),
            ]
        ),
        .executableTarget(
            name: "GhostermApp",
            dependencies: ["GhostermAppModel"],
            path: "Sources/GhostermApp"
        ),
        // 测试依赖**可执行目标**：顶栏 / 侧栏是 SwiftUI 视图，几何与高亮
        // 只有把真视图渲染成位图才验得到（PixelRenderTests 同口径）。
        .testTarget(
            name: "GhostermAppTests",
            dependencies: ["GhostermAppModel", "GhostermApp"],
            path: "Tests/GhostermAppTests"
        ),
    ]
)
