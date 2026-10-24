// swift-tools-version:5.9
// GhostermTerminal — Apple 端 TerminalSurface：full libghostty（Metal）+ host IO 后端。
//
// 与 GhostermCore 的分工（AGENTS.md 铁律 #1）：
//   - 本包**只**把字节流变成屏幕（TerminalSurface），并实现 transport 接缝的
//     surface 一侧（输入 → transport / transport → 输出）；
//   - 它**不**知道背后是 SSH 还是本地 shell：字节流由调用方（App 层）接上
//     GhostermCore 的 gt_transport_*。
//
// libghostty 来自 patches/libghostty/ 打完补丁后的 GhosttyKit.xcframework
// （见 scripts/build-ghostty.sh）。xcframework 在 .gitignore 内，不入库；
// 缺产物时本包**编译不过**（而非静默降级）——这正是我们要的失败方式。
import PackageDescription
import Foundation

let repoRoot = URL(fileURLWithPath: #filePath)
    .deletingLastPathComponent()   // GhostermTerminal
    .deletingLastPathComponent()   // apple
    .deletingLastPathComponent()   // repo root
let xcfw = repoRoot
    .appendingPathComponent("build/GhosttyKit.xcframework")
    .appendingPathComponent("macos-arm64")
let staticLib = xcfw.appendingPathComponent("libghostty-internal.a").path
let headers = xcfw.appendingPathComponent("Headers").path

// libghostty 内部静态库带 C++（glslang 等）与一堆系统框架依赖。
let systemFrameworks = [
    "CoreFoundation", "Foundation", "AppKit", "Metal", "MetalKit",
    "QuartzCore", "CoreText", "CoreGraphics", "IOKit", "Carbon",
    "UniformTypeIdentifiers", "UserNotifications", "CoreServices", "Security",
]
let linkFlags = ["-L", xcfw.path, "-lghostty-internal", "-lc++"]
    + systemFrameworks.flatMap { ["-framework", $0] }

let package = Package(
    name: "GhostermTerminal",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "GhostermTerminal", targets: ["GhostermTerminal"]),
    ],
    targets: [
        .target(
            name: "CGhostty",
            path: "Sources/CGhostty",
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("include"),
                .unsafeFlags(["-I", headers, "-I", repoRoot.appendingPathComponent("core/include").path]),
            ],
            linkerSettings: [
                .unsafeFlags(linkFlags),
            ]
        ),
        .target(
            name: "GhostermTerminal",
            dependencies: ["CGhostty"],
            path: "Sources/GhostermTerminal"
        ),
        .testTarget(
            name: "GhostermTerminalTests",
            dependencies: ["GhostermTerminal"],
            path: "Tests/GhostermTerminalTests"
        ),
    ]
)