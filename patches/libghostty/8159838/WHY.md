# WHY — libghostty host IO 后端（embedder 拥有字节流）

## 1. 为什么要改

上游 libghostty 只有**一个** termio 后端：`exec`（`src/termio/backend.zig`
的 `Kind = enum { exec }`）。它**必然 fork 一个子进程并分配 pty**（`termio.Exec`
→ `Subprocess` → `Pty`）。C 嵌入面（`ghostty_surface_new`）因此只有两种用法：

- `command != null`：让 libghostty `/bin/sh -c` 起你给的命令；
- `command == null`：让 libghostty 起一个本地 shell。

**没有第三种**：把「已经在别处的字节流」交给 surface。对所有自带 transport 的
嵌入者（[Termini](https://github.com/hudsonkit/Termini) 式 SSH、[ghostty-web](https://github.com/coder/ghostty-web) 的 socket、本项目的 libssh2 会话）这都是硬
阻塞——没有它，就只能走 `libghostty-vt + 自绘` 那一档，等于放弃了 Metal 渲染。

失败现象（未打补丁时）：Ghosterm 的 macOS surface 拿不到 SSH 会话的输出通道，
`ghostty_surface_*` 没有任何一个入口能喂字节进去；surface 会安静地自己起一个
本地 shell，终端里显示的是本机 shell 而不是远端会话。

## 2. 补丁做了什么（最小面）

新增 `src/termio/Host.zig`：一个**无子进程、无 pty、无 read 线程**的 termio 后端，
把「谁拥有字节流」这件事外移给嵌入者。

```
嵌入者  --ghostty_surface_process_output-->  Termio.processOutput   （网格）
网格    --mailbox write-->  Host.queueWrite  --host_write 回调-->  嵌入者
嵌入者  --ghostty_surface_set_size-->  Host.resize --host_resize--> 嵌入者
```

C 面新增：

| 符号 | 作用 |
|---|---|
| `ghostty_surface_config_s.host` | 该 surface 不起子进程/pty |
| `ghostty_surface_config_s.host_write` | 终端编码后的用户输入 → 嵌入者 transport |
| `ghostty_surface_config_s.host_resize` | 网格尺寸变化 → 嵌入者 transport |
| `ghostty_surface_process_output()` | 嵌入者把 transport 读到的字节喂进网格 |

线程纪律与 `Exec` 完全同构，没有引入新锁：

- `processOutput` 与 `Exec` 的 pty read 线程一样，取 **同一个 renderer mutex**
  （`Termio.processOutputLocked`），故嵌入者的会话线程可直接调用，可与读写并发；
- 用户输入仍走原 mailbox → termio 线程（`Termio.Thread`），`Host.queueWrite`
  在 termio 线程内同步回调 `host_write`，字节流**不过 UI 线程、不进事件队列**。

## 3. 上游对照与可撤性

- 基线 commit：`81598385d608dda4946093d6d6a234d204a2f225`（submodule pin，2026-10-13；
  `build.zig.zon` 版本 `1.3.2-dev`，`minimum_zig_version = 0.16.0` —— 与上一版一致，
  故 zig 工具链要求未变）。
- 上游**尚无**对应能力（pin `8159838`：`termio/backend.zig` 只有 `Kind = enum { exec }`）。
  同类接缝已有独立先例：[libghostty-spm](https://github.com/Lakr233/libghostty-spm) 的
  `Patches/ghostty/0002-host-managed-io.patch`（pin `35a81a9`：枚举
  `GHOSTTY_SURFACE_IO_BACKEND_HOST_MANAGED` + `termio.Kind = { exec, host_managed }`）。
  命名与本补丁不同（它用 backend 枚举，本补丁用布尔 `host` + `host_write`/`host_resize`
  回调），可作未来对齐上游命名的参考。
- 若上游合入同类接缝（如把 `Kind` 扩成 `exec | host`），本补丁即可撤；撤掉后只需把
  `ghostty_surface_config_s.host*` 三个字段的默认值走回 `false`，嵌入侧改调上游入口即可，
  **core 侧零改动**（transport 是独立接缝）。
- 补丁用 `git diff` 生成，路径相对 submodule 根。

### 版本演进（补丁基线的移动）

| 日期 | submodule pin | 本补丁改动 |
|---|---|---|
| 2026-09-16 | `d4c88d8`（M6-1 引入 submodule） | 0001 初版 |
| 2026-10-14 | `8159838`（2026-10-13 的 upstream main HEAD） | 0001 **内容不变**，仅按新 pin 重生（上下文偏移 8/9/24 行） |

升 pin 的动机：把 OSC 7501（Program Status Protocol）的**解析层**纳入 vendor
（`src/terminal/osc/parsers/program_status.zig` + `terminal.StreamAction.program_status`），
为 `patches/libghostty/8159838/0002-program-status.patch` 提供解析前置。
**注意**：上游只把 7501 做进 libghostty-vt 层，**app 路径（termio/apprt/C ABI）仍是 no-op**
（`src/termio/stream_handler.zig` 里 `.program_status => {}`），故 0002 存在。

## 4. 验证方式

```bash
bash scripts/build-ghostty.sh          # 干净树 → 打补丁 → 产 GhosttyKit.xcframework
bash scripts/gate-ghostty-patch.sh     # 断言补丁可应用 + 4 个符号真的导出
```

`gate-ghostty-patch.sh` 用 `nm` 对拍 **4 个必须存在的导出符号**，缺一即红——
这防的是「补丁没打上却用旧 xcframework 静默出绿」。

## 5. 已知边界

- `Host.getProcessInfo` 对 `foreground_pid` / `tty_name` 一律返回 `null`：
  这两者是 pty 概念，宿主 transport 未必有。**诚实报「没有」，不编造**。
- `child_exited` 由嵌入者自己判定（它才知道 SSH 会话何时断），本后端不合成。
---

# WHY — 0002 OSC 7501（Program Status Protocol）接到嵌入者

## 1. 为什么要改

OSC 7501 让**终端里跑的程序**汇报它在干什么（`idle/working/done/blocked/
error`，可带 `id/app/progress/title/message`），好让终端在标签页等 UI 上
显示。上游（`bae2c3cdbf` / `9cd19b40de`，spec rev 0.4）只做到
**libghostty-vt** 层：

- `src/terminal/osc/parsers/program_status.zig`（解析 + 校验）
- `terminal.StreamAction.program_status`
- `include/ghostty/vt/terminal.h` 的 `GHOSTTY_TERMINAL_OPT_PROGRAM_STATUS`

**full libghostty 的 app 路径仍是 no-op**：`src/termio/stream_handler.zig` 里
`.program_status => {}`，`apprt/action.zig`、`apprt/surface.zig`、`Surface.zig`、
`include/ghostty.h` 都没有对应物。所以 macOS/iOS（我们走 full libghostty）
**升 pin 之后仍然收不到 7501**——这段接线得我们自己写。

## 2. 补丁做了什么

把解析出来的报告交给嵌入者，并回答问题（协议规定：**不回 = 视为不支持**）。

数据流（与 `host_write` 同一条接缝、同一线程纪律）：

```
程序字节 → OSC 解析 → StreamAction.program_status
   ├─ report → stream_handler.programStatus
   │           → apprt.Surface.hostProgramStatus（termio 线程，同步）
   │           → 宿主 C 回调（report 只在回调期间有效）
   └─ query(?) → stream_handler 直接 write_stable 回包
                 → termio mailbox → backend.queueWrite → host_write → transport
```

改动 4 个上游文件：

| 文件 | 改动 |
|---|---|
| `src/terminal/osc/parsers/program_status.zig` | 新增 `C`（extern POD，含 `String`），sized struct |
| `src/termio/stream_handler.zig` | 接 `.program_status`：report→回调、query→回包 |
| `src/apprt/embedded.zig` | `Options`/`Surface` 加 `host_program_status` + 访问器 |
| `include/ghostty.h` | C 面：state/kind 枚举、`ghostty_program_status_s`、config 字段 |

**刻意不碰 `apprt/action.zig`**：它的 `Key` 被 `checkGhosttyHEnum(Key,
"GHOSTTY_ACTION_")` 与 `assert(@sizeOf(CValue) == 24)` 锁着，是最容易在升 pin
时冲突的地方；而且 7501 的载荷（msg ≤ 2048B）塞进 surface mailbox 的
`Message` union 会把整条消息通道撑大约 7 倍。走同步回调两个问题都没有。

**语义与上游对齐**：`state`/`kind` 用上游 C 枚举的序数
（`GHOSTTY_PROGRAM_STATUS_STATE_*`、`_KIND_*`；Zig 的 `Kind` 没有 `none`，
故 +1 平移）；`title`/`message` 在 Zig 侧按 base64 解码后再交付（与 vt 层的
「回调拿到已解码文本」一致）；字符串**只在回调期间有效**，嵌入者要留就自己拷。

**诚实口径（铁律 #6）**：只有宿主注册了回调，才会回答支持查询；没人接就
保持沉默，程序据此判定 OSC 7501 不支持——不假装支持。

## 3. 上游对照与可撤性

- 基线：与 0001 同一 pin（`8159838`，2026-10-13）。
- **撤除条件**：上游若把 7501 接进 apprt action / `include/ghostty.h`
  （很可能，因为协议就是为了终端 UI），就撤 0002，把嵌入侧改接上游入口；
  我们自己的 `ProgramStatusReport`（Swift 值类型）与记录/UI 层**不用动**。
- 补丁用 `git diff` 生成，路径相对 submodule 根；与 0001 **叠加**（0002 改的
  就是 0001 刚加的上下文），故 `gate-ghostty-patch.sh` 的检查口径改成
  「在干净 pin 树上按序应用」。

## 4. 验证方式

- `bash scripts/gate-ghostty-patch.sh`：按序应用 + 断言 C 面
  （`host_program_status`、`ghostty_program_status_s`）真的在头里；
- `bash scripts/terminal-gate.sh`：真 libghostty 网格 6 项；
- `bash scripts/mutation-check-ghostty-patch.sh`：符号断言有牙；
- **行为验证（临时探针，未入库）**：给 host surface 喂
  `ESC ] 7501 ; state=working:app=cargo:progress=40 ST` → 宿主回调收到
  `working/cargo/40`；`state=blocked:kind=permission` → `blocked/permission`；
  喂 `ESC ] 7501 ; ? ST` → **transport 侧**收到
  `ESC ] 7501 ; ?:states=idle,working,done,blocked,error:kinds=permission,question,auth ST`。

---

# WHY — 0003 OSC 133（shell 集成）透给嵌入者

## 1. 为什么要改

OSC 133（semantic prompt）是 shell 集成协议：shell 用它告诉终端「我要画提示符
了 / 用户可以输入了 / 命令开始跑了 / 命令结束了」。OSC 7501 的规范要求终端
在**提示符（重新）开始**时清掉 `working`/`blocked` 记录，所以我们需要这条信号。

上游在 app 路径里**只转了两个事件**（`src/termio/stream_handler.zig`）：

- `end_input_start_output` (C) → `.start_command`
- `end_command` (D) → `.stop_command`

`prompt_start` / `fresh_line_new_prompt` / `new_command` / `end_prompt_start_input`
等落在「Handled by Terminal, no special handling by us」里，**永远不会到 apprt**；
`apprt/action.zig`、`apprt/surface.zig`、`include/ghostty.h` 里也没有对应物。
上游 VT 层（`stream_terminal.Handler.effects.semantic_prompt`）则是全的。

## 2. 补丁做了什么

与 0002 同一接缝、同一线程纪律：`stream_handler.semanticPrompt` 里把事件转给
宿主回调；映射**照抄上游 VT 层**（`stream_terminal.Handler.semanticPrompt`）：

| OSC 133 动作 | 透出的 kind |
|---|---|
| `fresh_line` (L) | 不报（上游 VT 也不报） |
| `fresh_line_new_prompt` (A) / `new_command` (N) / `prompt_start` (P) | `PROMPT_START`（带 prompt_kind） |
| `end_prompt_start_input` (B) / `..._terminate_eol` (I) | `INPUT_START` |
| `end_input_start_output` (C) | `OUTPUT_START`（带**解码后**的命令行） |
| `end_command` (D) | `COMMAND_END`（带退出码与可选说明） |

改动 4 个上游文件：`semantic_prompt.zig`（+C POD，含 `String`）、
`stream_handler.zig`（`semanticPromptHost`）、`apprt/embedded.zig`
（`host_semantic_prompt` + 访问器）、`include/ghostty.h`（C 面）。

`kind` / `prompt_kind` 的**取值与序数照抄**上游
`GhosttySemanticPromptKind` / `GhosttySemanticPromptPromptKind`
（含 `INVALID=0`：零值不得被当成真事件）。

## 3. 上游对照与可撤性

- 基线：与 0001/0002 同一 pin（`8159838`）。
- **撤除条件**：上游若把 OSC 133 全量接进 apprt/C ABI，撤 0003，
  嵌入侧改接上游入口；我们的 `SemanticPromptEvent`（Swift 值）与
  `TerminalPane.handleSemanticPrompt` 不用动。

## 4. 验证方式

- `gate-ghostty-patch.sh`：三份补丁在干净 pin 上**按序**应用 + 头文件里有
  `host_semantic_prompt` / `ghostty_semantic_prompt_s`；
- `terminal-gate.sh`：`TerminalSurfaceTests` 喂真 OSC 133 序列（`P`/`B`/`C`/`D`）
  断言 kind/prompt_kind/命令行解码/退出码，12 tests / 0 failures；
- `mutation-check-program-status.sh` M3：把「只有 prompt_start 才清记录」的
  判别改坏 → 对应断言变红，prompt_start 用例仍绿（红有指向性）。
