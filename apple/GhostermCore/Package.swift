// swift-tools-version:5.9
// GhostermCore — Apple 端对 core/include/ghosterm.h 的 Swift 绑定包。
//
// 单一事实来源是 core/include/ghosterm.h（C ABI）；本包不复制头，而是用
// 符号链接把它接进 SPM 的 C 目标（Cghosterm），从而 **既是绑定又是对拍**：
// 头变了这里编译不过，等价于「绑定未同步」（配合 scripts/check-abi.sh）。
import PackageDescription
import Foundation

let repoRoot = URL(fileURLWithPath: #filePath)
    .deletingLastPathComponent()   // GhostermCore
    .deletingLastPathComponent()   // apple
    .deletingLastPathComponent()   // repo root
let coreBuild = repoRoot.appendingPathComponent("core/build").path
// libssh2 由 core 静态库引入；SPM 不做 pkg-config，故附上 Homebrew 常见库目录。
let brewLibDirs = ["/opt/homebrew/lib", "/usr/local/lib"].filter {
    FileManager.default.fileExists(atPath: $0)
}

let package = Package(
    name: "GhostermCore",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "GhostermCore", targets: ["GhostermCore"]),
    ],
    targets: [
        .target(
            name: "Cghosterm",
            path: "Sources/Cghosterm",
            publicHeadersPath: "include",
            linkerSettings: [
                .unsafeFlags(["-L", coreBuild, "-lghosterm_core", "-lssh2"]
                    + brewLibDirs.flatMap { ["-L", $0] }),
            ]
        ),
        .target(
            name: "GhostermCore",
            dependencies: ["Cghosterm"],
            path: "Sources/GhostermCore"
        ),
        .testTarget(
            name: "GhostermCoreTests",
            dependencies: ["GhostermCore"],
            path: "Tests/GhostermCoreTests"
        ),
    ]
)
