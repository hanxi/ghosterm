// Smoke — 无窗口冒烟（闸门与调试用）。
//
// 两个模式都**不弹任何 UI**，直接在进程里跑完整链路后打印判据：
//   --local [exe]            本地 PTY → libghostty 网格 → 读回文本
//   --ssh-smoke <cfg> <pat>  真 SSH 连接链路（AppModel 全链路，无 interaction）
//   --config-check <conf>    终端配置能否被引擎加载（出包后的**包内**断言：
//                            验配置 + 资源目录推导 + 主题解析一起）
//
// 为什么要留着它们：闸门跑在无窗口环境，NSAlert / 窗口都起不来；这两个模式
// 让「连不上会不会崩」这件事在没有 UI 时也能验收。
import AppKit
import GhostermAppModel
import GhostermTerminal
import Foundation

/// 无 UI 的 SSH 连接冒烟：模拟真实连接流（工作线程 + decider + 认证链），
/// 无 interaction。结果打到 stdout：STATUS[...]。
/// 成功判据：surface 建立过（SSH 会话 + 终端面装配完成）。
/// 收尾时 read 循环会因通道关闭报「失败：ssh_shell read」——那是断开
/// 的正常表现，不是连接失败。
enum SSHSmoke {
    @MainActor
    static func run(configPath: String, pattern: String) {
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
        let model = AppModel(configPath: configPath)
        model.interaction = nil
        model.connect(to: .host(pattern))
        var connected = false
        let deadline = Date().addingTimeInterval(30)
        while Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.1))
            if model.surface != nil {
                connected = true
                // 再等一会儿让会话跑起来（e2e 冒烟要网格有内容）。
                RunLoop.main.run(until: Date().addingTimeInterval(1.0))
                break
            }
            if model.status.hasPrefix("连接失败") { break }
        }
        let text = "STATUS[\(model.status)] surface=\(model.surface != nil)\n"
        FileHandle.standardOutput.write(Data(text.utf8))
        if let surface = model.surface, connected,
           let grid = surface.readText(), !grid.isEmpty {
            FileHandle.standardOutput.write(
                Data("GRID[\(grid.prefix(200))]\n".utf8))
        }
        model.disconnectForTesting()
        exit(connected ? 0 : (model.status.hasPrefix("连接失败") ? 3 : 2))
    }
}

/// 无窗口冒烟：终端配置能否被引擎接受（出包后跑在**包内**二进制上）。
///
/// 这条存在的理由：主题这类资源要靠「沿可执行文件路径推导资源目录」找到，
/// 而那是打包布局的函数（<App>/Contents/Resources/ghostty）。在测试进程里
/// 验不到，只有在真装配好的 .app 里跑才有意义。
enum ConfigSmoke {
    static func run(configPath: String) {
        do {
            let app = try TerminalApp.shared(configPath: configPath)
            let diags = app.configDiagnostics
            let out = "CONFIG_OK diagnostics=\(diags.count)\n"
            FileHandle.standardOutput.write(Data(out.utf8))
            if !diags.isEmpty {
                FileHandle.standardError.write(
                    Data("诊断：\(diags.joined(separator: "; "))\n".utf8))
            }
            exit(diags.isEmpty ? 0 : 3)
        } catch {
            FileHandle.standardError.write(Data("配置加载失败: \(error)\n".utf8))
            exit(2)
        }
    }
}

/// 无窗口冒烟：起本地 PTY → 喂进 libghostty → 从网格读回。
enum LocalSmoke {
    static func run(executable: String?) {
        do {
            let termApp = try TerminalApp.shared()
            let surface = try TerminalSurface(app: termApp)
            // `.shell`：这条冒烟就是替「本地 shell 标签页」把关的，走的得是
            // 生产那条路（含登录语义），否则打包后真实路径出问题它看不见。
            let session = try SessionController.localShell(
                surface: surface, executable: executable, kind: .shell
            )
            session.start()
            // 等网格真有内容，而不是固定睡 0.5s：新起的 shell 在负载高时
            // 可能还没输出（实测并发出包时这条冒烟偶发拿不到 GRID）。
            // 轮询到有内容或超时（超时也算失败——那时是真没起来，不是慢）。
            var text = ""
            let deadline = Date().addingTimeInterval(10)
            while Date() < deadline {
                Thread.sleep(forTimeInterval: 0.05)
                text = surface.readText() ?? ""
                if !text.isEmpty { break }
            }
            session.stop()
            FileHandle.standardOutput.write(Data("GRID[\(text)]\n".utf8))
            exit(text.isEmpty ? 1 : 0)
        } catch {
            FileHandle.standardError.write(Data("smoke 失败: \(error)\n".utf8))
            exit(1)
        }
    }
}

