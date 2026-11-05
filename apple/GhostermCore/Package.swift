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
// Use the same patched dependency as the shared Core; no Homebrew fallback.
// Match Core's dependency lookup for isolated copies living under tmp/.
var dependencyRoot = repoRoot
while !FileManager.default.fileExists(atPath: dependencyRoot.appendingPathComponent("patches/libssh2/1.11.1/WHY.md").path) {
    let parent = dependencyRoot.deletingLastPathComponent()
    precondition(parent.path != dependencyRoot.path, "Pinned libssh2 dependency root missing")
    dependencyRoot = parent
}
let ssh2Lib = dependencyRoot.appendingPathComponent("build/host-native/lib").path
// Explicit Core conformance profile; ordinary App builds keep the backend OFF.
let pluginJS = ProcessInfo.processInfo.environment["GHOSTERM_CORE_TEST_PLUGIN_JS"]
precondition(pluginJS == nil || pluginJS == "0" || pluginJS == "1", "Invalid Core JS profile")
let runtimeLink: [String] = pluginJS == "1" ? ["-lghosterm_quickjs", "-lm"] : []
if pluginJS == "1" {
    precondition(FileManager.default.fileExists(atPath: coreBuild + "/libghosterm_quickjs.a"), "Build enabled Core JS archive first")
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
                .unsafeFlags(["-L", coreBuild, "-lghosterm_core",
                              "-L", ssh2Lib, "-lssh2"] + runtimeLink),
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
            path: "Tests/GhostermCoreTests",
            exclude: pluginJS == "1" ? [] : ["ExtensionRuntimeTests.swift"]
        ),
    ]
)
