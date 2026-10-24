// TerminalIMETests — 证明「macOS 输入法（中文等）真的接上了终端」。
//
// 用户报的缺陷：终端敲不出中文、输入法不弹。根因是宿主视图**没实现
// `NSTextInputClient`**、`keyDown` 也没调 `interpretKeyEvents`——系统输入法
// 压根没被唤起（没有候选窗、没有组合）。这里按上游 Ghostty 的口径补齐，并用
// **能观测到的行为**钉住这条链路：
//   1. 视图就是文本输入客户端（输入法能被唤起的前提）；
//   2. 组合中的串（preedit）真的进了引擎——观测的是 libghostty 自己的
//      `ime_point`（候选窗锚点宽度按预编辑串算），不是我们自己的账本；
//   3. 提交的中文真的到了 transport，且是**键入**语义（不是粘贴）。
//
// 为什么用 `TerminalApp.shared()`：libghostty 的 app 释放会停掉全局状态
// （见 `TerminalApp` 的注释），故凡是挂 host view 的用例都沿用仓库既有口径。
import AppKit
import GhostermTerminal
import XCTest

final class TerminalIMETests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    private func makeHost() throws -> (TerminalSurface, TerminalHostView) {
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        let host = TerminalHostView(surface: surface)
        return (surface, host)
    }

    private func waitUntil(_ timeout: TimeInterval = 5, _ cond: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if cond() { return true }
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        return cond()
    }

    private func debugBytes(_ data: Data) -> String {
        "[" + data.map { String(format: "%02x", $0) }.joined(separator: " ") + "]"
    }

    /// 1. 视图必须**是**文本输入客户端——这是 macOS 唤起输入法的前提。
    func testHostViewIsTextInputClient() throws {
        let (_, host) = try makeHost()
        XCTAssertTrue(host is NSTextInputClient,
                      "视图必须是 NSTextInputClient，否则系统输入法（中文）不会被唤起")
        XCTAssertTrue(host.responds(to: #selector(NSTextInputClient.insertText(_:replacementRange:))),
                      "必须响应 insertText（输入法提交入口）")
        XCTAssertTrue(host.responds(to: #selector(NSTextInputClient.setMarkedText(_:selectedRange:replacementRange:))),
                      "必须响应 setMarkedText（组合入口）")
        XCTAssertTrue(host.responds(to: #selector(NSTextInputClient.firstRect(forCharacterRange:actualRange:))),
                      "必须响应 firstRect（候选窗定位）")
        XCTAssertTrue(host.acceptsFirstResponder,
                      "终端视图要能当第一响应者，否则收不到键盘事件")
        XCTAssertNotNil(host.inputContext,
                      "系统应给文本输入客户端建立 NSTextInputContext——这是输入法挂载点")
    }

    /// 端到端：一次真实的 `keyDown`（合成 NSEvent）必须经 `interpretKeyEvents`
    /// 落到 `insertText`，再以**键入语义**到达 transport。
    ///
    /// 这条最接近用户操作：它走的正是「敲一个键」的完整路径。若 `keyDown` 里
    /// 漏掉 `interpretKeyEvents`（早先的写法），输入法链路整条断掉。
    @MainActor
    func testKeyDownRoutesThroughTextInputClient() throws {
        let (surface, host) = try makeHost()
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 300),
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = host
        window.makeFirstResponder(host)

        var received = Data()
        surface.onInput = { received.append($0) }

        let event = NSEvent.keyEvent(
            with: .keyDown, location: .zero, modifierFlags: [],
            timestamp: ProcessInfo.processInfo.systemUptime,
            windowNumber: window.windowNumber, context: nil,
            characters: "a", charactersIgnoringModifiers: "a",
            isARepeat: false, keyCode: 0)
        XCTAssertNotNil(event)
        host.keyDown(with: event!)

        XCTAssertTrue(waitUntil { !received.isEmpty },
                      "keyDown 必须把按键编成字节送到 transport（得到 \(debugBytes(received))）")
        XCTAssertEqual(String(decoding: received, as: UTF8.self), "a",
                      "普通字母应原样到达（得到 \(debugBytes(received))）")
    }

    /// 普通 ASCII 提交**不得**被 bracketed paste 包裹（同样是键入语义）。
    func testPrintableKeyIsNotBracketed() throws {
        let (surface, host) = try makeHost()
        var received = Data()
        surface.onInput = { received.append($0) }
        surface.feed(Data("\u{1b}[?2004h".utf8))
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        received.removeAll()

        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 300),
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = host
        window.makeFirstResponder(host)
        host.keyDown(with: NSEvent.keyEvent(
            with: .keyDown, location: .zero, modifierFlags: [],
            timestamp: ProcessInfo.processInfo.systemUptime,
            windowNumber: window.windowNumber, context: nil,
            characters: "b", charactersIgnoringModifiers: "b",
            isARepeat: false, keyCode: 11)!)
        XCTAssertTrue(waitUntil { String(decoding: received, as: UTF8.self) == "b" },
                      "字母 b 应与粘贴近（原样）到达，得到 \(debugBytes(received))")
    }

    /// 2. 组合串（preedit）必须进引擎做内联显示。
    ///
    /// 观测口径：libghostty 的 `ime_point` 宽度 = 预编辑串宽度 × 字宽；
    /// 组合串没进引擎（或没下发 preedit）就恒为 0。
    func testMarkedTextShowsAsEnginePreedit() throws {
        let (surface, host) = try makeHost()
        XCTAssertEqual(surface.imePoint.width, 0, "初始无组合，锚点宽度应为 0")

        host.setMarkedText("ni hao",
                           selectedRange: NSRange(location: 6, length: 0),
                           replacementRange: NSRange(location: 0, length: 0))
        XCTAssertTrue(host.hasMarkedText(), "设置 marked text 后应处于组合态")
        XCTAssertGreaterThan(surface.imePoint.width, 0,
            "组合串必须进入引擎做内联显示（ime_point 宽度按预编辑算）；"
            + "恒为 0 说明 preedit 没下发，用户就看不到正在打什么")

        host.unmarkText()
        XCTAssertFalse(host.hasMarkedText(), "取消组合后不应再有 marked text")
        XCTAssertEqual(surface.imePoint.width, 0, "取消组合后引擎里的预编辑应被清除")
    }

    /// 3. 输入法**提交**的中文必须到达 transport，且是**键入**语义（非粘贴）。
    ///
    /// 之所以难造假：先打开 bracketed paste（DEC 2004）——粘贴语义会被
    /// `ESC[200~ … ESC[201~` 包裹，而提交应当原样出来。
    func testCommittedChineseReachesTransportAsTypedInput() throws {
        let (surface, host) = try makeHost()
        var received = Data()
        surface.onInput = { received.append($0) }

        surface.feed(Data("\u{1b}[?2004h".utf8))   // 打开 bracketed paste
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        received.removeAll()

        host.insertText("中文", replacementRange: NSRange(location: 0, length: 0))
        XCTAssertTrue(waitUntil { String(decoding: received, as: UTF8.self) == "中文" },
                      "输入法提交的中文应原样到达 transport（得到 \(debugBytes(received))）")
        XCTAssertFalse(String(decoding: received, as: UTF8.self).contains("200~"),
                       "提交不能是粘贴语义（不该被 bracketed paste 包裹）")
    }

    /// 回归（对照上游 Ghostty discussion #14451）：输入法**提交的控制字符**
    /// 必须原样到达 transport——tmux 前缀不能被吞。
    ///
    /// 上游的 bug：切换输入法（如 Caps Lock 英→中）后按下的第一个键，macOS
    /// 的输入源切换是异步传播的，可能正好落在这次 keyDown 中间；上游 keyDown
    /// 里「键盘布局 ID 变了 ⇒ 假定被输入法吃掉」的 guard 会把整键丢掉——哪怕
    /// 输入法其实**产出了**文本 "\u{0002}"（Ctrl+B）。Ghosterm 的 keyDown
    /// 没有那个 guard（有提交文本就下发；只有组合态才吞键），这条钉住该语义。
    func testCommittedControlCharReachesTransport() throws {
        let (surface, host) = try makeHost()
        var received = Data()
        surface.onInput = { received.append($0) }

        // 输入法把 Ctrl+B 交给文本输入链的方式：提交控制字符 "\u{0002}"。
        host.insertText("\u{0002}",
                        replacementRange: NSRange(location: 0, length: 0))
        XCTAssertTrue(waitUntil { !received.isEmpty },
                      "输入法提交的控制字符必须到达 transport（得到 \(debugBytes(received))）")
        XCTAssertEqual(Array(received), [0x02],
                       "Ctrl+B 应以裸字节 0x02 到达（得到 \(debugBytes(received))）")
    }

    /// 回归：走输入法链路后，**普通按键的编码不能变**。
    ///
    /// 这些键不会被输入法接手（`interpretKeyEvents` 只会给它们 `doCommand`，
    /// 我们不在那里处理），必须落到 `keyDown` 末尾的普通按键路径、按物理键 +
    /// 修饰键编码。若哪天 `keyDown` 变成「一律由输入法出文本」，回车 / Ctrl+C /
    /// 方向键就会**静默失效**——这条就是钉它的。
    @MainActor
    func testSpecialKeysStillEncodeThroughKeyDown() throws {
        let (surface, host) = try makeHost()
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 300),
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = host
        window.makeFirstResponder(host)
        var received = Data()
        surface.onInput = { received.append($0) }

        func press(_ code: UInt16, _ chars: String,
                   _ mods: NSEvent.ModifierFlags = []) -> [UInt8] {
            received.removeAll()
            let ev = NSEvent.keyEvent(
                with: .keyDown, location: .zero, modifierFlags: mods,
                timestamp: ProcessInfo.processInfo.systemUptime,
                windowNumber: window.windowNumber, context: nil,
                characters: chars, charactersIgnoringModifiers: chars,
                isARepeat: false, keyCode: code)!
            host.keyDown(with: ev)
            _ = waitUntil { !received.isEmpty }
            return Array(received)
        }

        XCTAssertEqual(press(36, "\r"), [0x0D], "回车应编码为 CR")
        XCTAssertEqual(press(51, "\u{7f}"), [0x7F], "退格应编码为 DEL")
        XCTAssertEqual(press(8, "\u{03}", .control), [0x03], "Ctrl+C 应编码为 ETX（中断）")
        XCTAssertEqual(press(11, "\u{0002}", .control), [0x02],
                       "Ctrl+B 应编码为 STX（tmux 前缀；#14451 同场景的兜底路径）")
        XCTAssertEqual(press(126, "\u{F700}"), [0x1B, 0x5B, 0x41],
                       "方向键应编码为 CSI A（不被 PUA 字符干扰）")
    }

    /// 对照：证明「粘贴语义会被 bracketed paste 包裹」——上面那条断言因此有区分度
    /// （若把提交错写成熟粘路径，包裹标记就会出现）。
    func testPasteSemanticsAreBracketedSoTypedPathIsDistinguishable() throws {
        let (surface, _) = try makeHost()
        var received = Data()
        surface.onInput = { received.append($0) }
        surface.feed(Data("\u{1b}[?2004h".utf8))
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        received.removeAll()

        surface.sendText("中文")   // 粘贴语义
        XCTAssertTrue(waitUntil {
            String(decoding: received, as: UTF8.self).contains("200~")
        }, "对照不成立：粘贴语义未被 bracketed paste 包裹（得到 \(debugBytes(received))）")
    }
}
