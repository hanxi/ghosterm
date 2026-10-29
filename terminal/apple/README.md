# terminal/apple — full libghostty（Metal）

## 依赖

- `libghostty`（vendored 于 `vendor/ghostty`，锁 pin）
- 上游补丁：`patches/libghostty/`（嵌入所需 API，见各 `WHY.md`）

## 已知必需的嵌入面（参考 [Termini](https://github.com/hudsonkit/Termini) 的实践）

- 输出：`ghostty_surface_process_output(surface, bytes, len)`
- 输入：runtime callback `write_to_host_cb(userdata, bytes, len)`
- 配置：宿主生成 Ghostty 配置并经 shim 的 app reload 下发已有 surface；不是只改 UI 字号
- surface 以 `NSView` 为平台锚点；Metal 层由 libghostty 渲染

`patches/libghostty/` 里必须记录每个补丁的**为什么**与**上游对照 commit**。

## 现状（M1 已完成）

实现落在 **`apple/GhostermTerminal`**（SPM 包），本目录只保留说明：

| 项 | 位置 |
|---|---|
| libghostty C 桥（runtime/宿主回调，无业务语义） | `Sources/CGhostty/shim.c` |
| TerminalSurface（feed / onInput / onResize / sendKey / preedit / imePoint / sendCommittedText） | `Sources/GhostermTerminal/TerminalSurface.swift` |
| SwiftUI 视图桥（NSViewRepresentable）+ 键盘/输入法（`NSTextInputClient`） | `Sources/GhostermTerminal/TerminalSurfaceView.swift` |
| 上游补丁（host IO 后端） | `patches/libghostty/8159838/`（附 `WHY.md`） |
| 构建产物 | `build/GhosttyKit.xcframework`（gitignore，由 `scripts/build-ghostty.sh` 生成） |

接线（与 `core` 的 transport 对接）在 `apple/GhostermApp/Sources/GhostermAppModel/SessionController.swift`：

```
session 线程：transport.read → surface.feed       （字节流直连，不过 UI 线程）
termio 线程：surface.onInput/onResize → 待办队列 → 会话线程执行 transport.write/resize
```

> **注意**：输入/尺寸回调**不能**在 libghostty 的 termio 线程上直接写 transport：
> core 的契约是 transport 由会话线程独占（libssh2 非线程安全），实测直接写会
> 让 resize 回调发出后永不返回（termio 线程卡死）。故回调只压待办队列。

### 键盘与输入法（macOS）

`TerminalHostView` 实现 **`NSTextInputClient`**，`keyDown` 走 `interpretKeyEvents`——
这是中文/日文/韩文等 IME 能被唤起的前提（候选窗 + 组合串）。输入法经回调上来后：

- 组合串（`setMarkedText`）→ `ghostty_surface_preedit`（**内联显示，不发给程序**）；
- 提交（`insertText`）→ 以**键入语义**下发（`ghostty_surface_committed_text`），
  **不是**粘贴——否则对端开了 bracketed paste 会把中文当成一次粘贴；
- 候选窗锚点（`firstRect`）→ `ghostty_surface_ime_point`。

观测与变异见 `TerminalIMETests` 与 `scripts/mutation-check-terminal.sh`（含 T5~T8）；
坑与取舍记在 `docs/reference/apple-ui-pitfalls.md` 第 7 条。

### 渲染面注意

> UI 层的坑（`Menu` 会静默吃掉视图、抓帧取位图的顺序）另记在
> `docs/reference/apple-ui-pitfalls.md`。

libghostty 的 Metal 渲染器要求 surface 的 NSView 是 **layer-hosting**：它先给
自己的 layer、再置 `wantsLayer`（见 `vendor/ghostty/src/renderer/Metal.zig`）。
嵌入侧**不要**提前设 `view.wantsLayer = true`，否则它会走另一条分支。

## 待办

- [ ] iOS full libghostty 构建与 UIKit 宿主：当前 SwiftPM 依赖 AppKit，不能只加一个 `xcframework_target` 就交付 iOS。
- [x] 自进程视图像素验证：`PixelRenderTests` 与 `mutation-check-pixel.sh` 已覆盖背景/前景颜色及对照，不需要屏幕录制权限；真实系统激活/全屏另见标签界面计划的桌面证据。
