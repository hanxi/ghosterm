# 会话运行时 ADR：权威终端状态与恢复

状态：**引擎恢复候选架构；P0 未通过**。初次核实：2026-10-17；远端部署边界更新：2026-10-20。
范围：[会话 daemon 重构计划](../reference/plan-session-daemon.md) 的引擎边界，
不修改 AGENTS.md 的平台分档，不代表 daemon 或新的 C ABI 已实现。

## 决策与依赖

候选设计由 daemon 中的无 UI 引擎保存权威终端状态并回应终端查询；
客户端 surface 导入状态后消费有序增量，负责渲染与交互编码；Apple/Linux 使用
full libghostty，Android/HarmonyOS/Web 使用 VT + 自绘，Windows 引擎仍待验证。
core runtime 只调用私有 C 函数表，daemon 组合根注入引擎实现；core 不依赖 terminal。
进程内会话使用同一 session/attachment 契约，由平台接线层组合引擎与 runtime。

这条路线必须同时证明：状态准确、唯一应答来源、输入编码一致、资源有界和不依赖 UI。
当前证据只支持继续做 P0，尚不足以选择最终恢复协议或进入 P4/P5。
不保留旧 ABI/协议兼容层，不以输出尾巴、ANSI 重画或隐式退回进程内 shell 代替恢复。

core 的公开头与私有 backend 拆分已先行落地（`e6f8f19`、`942b6b0`），
当时尚无 session/attachment owner 或 daemon；后续私有原型进展见下文，
生产 daemon 仍未交付。该拆分不依赖本 ADR 的恢复路线。

## 远端部署与证据边界（2026-10-20）

部署方向确定为[跨平台客户端 → SSH exec 流 → 远端连接代理 → 用户级 sessiond](remote-session.md)。
远端 sessiond 保存原 PTY/程序和本文的权威终端状态；SSH 只承载 attachment 协议。
连接代理退出、GUI 关闭或网络断开不结束 session，重连新建 SSH 连接后执行同一
S/L 恢复事务，不直接依赖 tmux，不要求额外公网端口或本机 daemon 常驻。

这调整部署和交付主线，不放宽引擎闸门。macOS socketpair/Metal/SIGKILL 原型仅是
引擎与进程恢复证据；真实 Linux 宿主、SSH/静默丢包、原 PTY 保留、第二种客户端
及跨 OS/CPU 快照均未交付。快照兼容按引擎 revision/补丁集、资源格式与能力
检查，不能要求不同平台二进制 SHA 相同；首版仍拒绝不兼容版本，不自动结束旧会话。
本地 daemon 与远端 sessiond 均须交付，共用同一个 runtime/管理/恢复逻辑；传输
入口不同不允许复制权威状态/owner 规则。共用帧封装见[协议说明](../reference/session-ipc.md)。
现有私有 worker/attachment/manager 是基础组件，不能据此宣称生产 runtime 已交付。
下文按日期保留的引擎实验记录不构成远端支持声明。

## 精确版本证据

审计和运行基于 gitlink `81598385d608dda4946093d6d6a234d204a2f225`，
上游版本 `1.3.2-dev`、Zig `0.16.0`，未应用 Ghosterm 的 full host-IO 补丁。
以下路径均相对此精确 pin；上游文件中的“complete”描述不能替代资源覆盖检查。

| 事项 | 源码证据 | 结论 |
|---|---|---|
| VT 快照 | `include/ghostty/vt/snapshot.h`；`src/terminal/c/snapshot.zig` | 有同步 encode 和一次性/增量 decode，CRC 和 READY/FINISH 标记 |
| 未完成解析状态 | `terminal.h` 的 continuation 配置；`src/terminal/stream_continuation.zig` | 必须在输入前启用有界跟踪；超限/过晚启用时可能不可导出，不能无限等 ground |
| 快照并发 | `snapshot.h` 的 encode 契约 | 编码期间调用者禁止修改 terminal；没有凭该接口即可后台无锁捕获的证据 |
| Kitty 图像 | `src/terminal/snapshot/snapshot.zig` 的 `complete snapshot preserves Kitty virtual placeholders` 测试 | 占位网格保留，但 image/placement 注册表明确省略，恢复后数量为 0 |
| full surface 导入 | `include/ghostty.h` 与 `src/termio/Termio.zig` | 公开嵌入 API 未提供 snapshot 导入；Termio 自有 terminal 和不同的 StreamHandler，不能直接转换 VT opaque handle |
| 查询输出 | VT `GHOSTTY_TERMINAL_OPT_WRITE_PTY`；Ghosterm `0001`/`0002` 补丁 | VT effects 可独立关闭；full host_write 合并用户编码输入和查询应答，需要引擎产生处隔离 |
| feed 错误 | `terminal.h` 的 `GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR`；`c/terminal.zig` 的 getter | 虽然 feed 返回 void，VT 仍能查询 sticky 语义失败；适配器必须在 feed 后读取并停止失效会话，不能仅检查调用是否返回 |

## 本机运行证据

环境：macOS arm64，AppleClang 21.0.0，Python 3.14.5，Zig 0.16.0。
后续已安装 Xcode 27.1 与 Metal Toolchain 27A266a，使用
`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` 选择完整工具链。
full libghostty 构建、终端 23 项测试、Apple 绑定及真实 SSH/SFTP、上游补丁闸门 9/9
已通过；这证明既有 full surface 可构建运行，不代表 snapshot 导入已经实现。
工具会话没有图形认证权限，曾出现 Keychain -25308；桌面登录会话下重新验证
宿主 257 项为 0 失败、1 项既有开发态资源跳过，认证变异 11/11 通过。
该全测曾因跨 PTY 继承 master 而卡住；修复描述符隔离后，清理宿主构建缓存、重链核心
并重新运行通过。当前核心闸门 9/9，新增描述符回归的两组定向变异通过。
Zig 官方下载索引与下载包 SHA256 一致：
`b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489`。

用 [探针脚本](../../scripts/probe-session-state.py) 构建原始 VT 静态库，
关闭 xcframework 打包，避免无 UI 引擎验证依赖完整 Xcode：

```bash
ZIG_LOCAL_CACHE_DIR="$ROOT/tmp/zig-cache-vt" \
ZIG_GLOBAL_CACHE_DIR="$ROOT/tmp/zig-global" \
"$ZIG" build -Demit-lib-vt=true -Demit-xcframework=false \
  -Doptimize=ReleaseFast -p "$ROOT/tmp/ghostty-vt"
```

上面从 `vendor/ghostty` 运行，`ROOT` 与 `ZIG` 都须为绝对路径。
完整执行方式见 [探针说明](../../terminal/session-state/README.md)。结果：

- 六类 continuation 的导出/导入/补续/主备用切换/resize 逐字节状态对比通过。
- VT 权威实例与禁用 effects 的 VT 副本只产生一份 cursor report，通过。
- CRC 损坏与截断 FINISH 拒绝测试通过。
- 真实 POSIX PTY 中的最小 TUI：控制终端/前台进程组、半截 CSI 恢复、停收期间
  权威查询应答、实读增量重放、唯一应答及子进程观察到 51×17 resize，通过。
- 真实 1×1 RGBA 图像在源实例存在，解码后丢失；资源探针返回 3，**阻止 P0 通过**。
- 七类定向变异均目标红、对照绿，回滚复绿；其中三类护 PTY 增量、重复应答和尺寸传递。

上述基础 VT 探针没有将快照导入 full surface，没有运行 vim/tmux 等外部 TUI，
也没有测量吞吐、峰值内存或 GUI；最小 TUI 的握手边界不能替代生产恢复事务及并发验证。
快照字节相同只覆盖编码器实际包含的状态；图像用例特意通过独立 image API 检查，
证明“相同快照字节”不能单独充当完整状态相等的证据。

### 分配失败与预算证据

`scripts/probe-session-allocation.py --mutations` 通过原始 VT C allocator 注入故障。
终端标题更新分配失败后，`VT_PROCESSING_ERROR` 为 true，reset 后仍为 true；
释放 terminal 后 callback 所管分配全部释放。隐藏错误标记、reset 后换成无错误的新实例、
绕过预算检查、遗漏 decoder 清理四类变异均目标红、独立对照绿、回滚复绿。
这修正了此前仅根据 void feed 就判断错误不可观测的结论。

同一夹具在正常 callback 预算下解码并逐字节对拍；把上限降到正常解码峰值减一字节，
解码返回 `GHOSTTY_OUT_OF_MEMORY`，输出 terminal 保持 NULL，已有 callback 分配全部回收。
本机夹具的 callback 峰值为 11,368 字节，失败上限为 11,367 字节；这是这份夹具的运行值，
不能作为会话的固定预算。allocator 对象、vtable 和 ctx 必须活到 decoder/terminal 都释放之后。

**该预算不是完整内存限额**：`terminal/page.zig` 在原生端直接 mmap/VirtualAlloc
页面，绕过 C allocator；malloc 元数据、renderer、压缩及其他线程也不在 callback 计数内。
后续必须把页面映射的计费与拒绝并入会话资源策略，不能把此探针的成功说成 RSS 已有界。
标记也不覆盖所有 parser 缓冲、外部效果或可优雅处理的协议失败，仍需逐类验证。

### full surface 构造时导入原型

`scripts/probe-session-surface.py` 在独立源码 archive 中按序应用现有 host 补丁和
`patches/libghostty/8159838/experimental/0004-replica-surface.patch`，构建真正的
full libghostty Metal 库。实验补丁不在生产构建的 glob 中，不改变现有 Swift 绑定。

恢复在新 surface 构造期间、renderer/IO 线程启动之前完成。Terminal 先放到最终地址，
再建立 readonly TerminalStream、补 continuation 并逐字节核验；既有 full renderer
和输入编码路径继续消费同一个 terminal。parser 先于 terminal 销毁，借用快照不逃逸。
这避免在运行中的 renderer/search 引用尚未撤销时替换旧 terminal。

ground 加 CSI/UTF-8/OSC/ESC/DCS/APC query 六类续接的导入前后及增量后规范快照对拍，
主/备用屏幕、历史、宽/组合字符、超链接均通过。副本查询输出、剪贴板和通知回调为零；
用户粘贴仍按恢复的 bracketed-paste 模式编码。绘制调用与 surface 销毁通过，
尚无像素一致性断言。另有真实 PTY 最小 TUI：GUI 不存在期间仅 VT 权威实例应答，
实读增量连同半截 CSI 快照随后导入 full surface，与继续演进的权威状态对拍通过。
Vim 另以禁用用户配置/插件/持久化的系统版本在真实 PTY 中运行，编辑测试文本后通过
VT formatter 验证网格内容，再将快照及实读增量导入 full surface；状态和输入模式通过。
tmux 3.6a 也在唯一 socket、空配置下运行，实际输入进入 pane 后捕获状态与增量供
full surface 对拍，客户端 detach、server 清理及后续 `has-session` 拒绝均通过。
八组变异验证 continuation 丢失、重新启用副本应答、丢弃用户输入、PTY/Vim/tmux 增量丢失、
实际 Vim 编辑缺失及 tmux server 清理缺失，
同一变异 full 库上的独立对照仍绿，回滚复绿。

该原型没有发布热导入 API、序号 barrier 或生产 session/attachment。只读 parser
不代替 daemon 的带序号宿主事件路由，也未验证图像装载副作用、动画 owner、配置/焦点/
resize 的统一事件顺序、页面内存预算和持续输出。完整 P0 仍未通过；
实验接口的边界及重现方式见对应 [WHY.md](../../patches/libghostty/8159838/experimental/WHY.md)。

## 图像补丁的实际状态边界

### 实时资源 I/O 也必须有唯一 owner

`scripts/probe-session-resource-io.py --mutations` 证明了不依赖 snapshot 图像缺失的
另一处问题：两边先从无图像的同一快照开始，并显式配置相同图像/临时目录策略。
权威 VT 执行 Kitty `t=t` 临时文件命令后，像素与 placement 已创建，文件已被消费删除；
副本重放同一输出后，规范快照（含末尾 cursor 标记）仍相同，但图像不存在。
两边的 `VT_PROCESSING_ERROR` 都为 false，因为读取失败被协议优雅处理。
所以 readonly effects 和 sticky 失败标记都不能单独保证实时副本准确。

对照用从权威图像 API 读取的真实 RGBA 像素构造受控 inline 载荷，副本像素及 placement
坐标一致，且不再次打开路径。四组变异验证源文件夹具、真实重放、像素内容及 placement，
均目标红、inline 独立对照绿、回滚复绿。临时文件原始重放仍退出 3，不作为恢复成功。
该对照只覆盖单个受控图像，**不是通用转义流重写器或生产资源协议**。

候选路线因此必须在引擎产生处捕获资源装载结果（含失败结果），带有序提交边界提供给
副本；副本按同一边界使用已解析资源，不能再次做文件/临时文件/共享内存 I/O。
事件走 transport→surface 的直接数据接缝，不进入 UI 线程事件队列。
不能只在客户端识别 APC 并删掉它：命令还会影响 cursor、placement、分块和动画，
也不能在后续文本已经解析后才随意补图。引擎 hook、异步完成与失败事件、预算和原生
full surface 集成尚待实现；在此之前不能固定仅含原始 OUTPUT 的恢复协议。

实验 `0005-resource-io-hooks.patch` 已推进到引擎 hook：`LoadingImage.init` 的
observer 导出真实外部载荷或读取拒绝；resolver 在同一命令的装载点提供这些结果，
不回退文件系统。真实临时文件仅由权威实例消费，副本在禁用临时文件装载时，仍得到
相同像素、placement 和规范状态。源读取失败保留协议失败语义；回调拒绝、事件不匹配
及重复消费设置 sticky 语义失败。五类用例及四组 native 变异通过。
observer/resolver 都在原生 feed 内同步调用；借用数据和 ctx 的生命周期见实验 WHY。

该原型传递的是**原始 I/O 载荷与读取结果**，不是最终解码/配额/提交结果。
无 UI VT 默认还要求显式注入 PNG decoder；不能把共享原始 PNG 字节当成已共享解码资源。
实验 0006 现可显式注入 full 引擎同一 Wuffs decoder，不依赖 AppKit/Metal。
真实 PNG 经资源 hook 后的像素与 placement 一致；坏图像和分配拒绝均清空输出并释放资源，
三组变异通过。这未解决不同预算下的最终提交一致性。
序号与 command token、异步完成、源/副本解码与资源拒绝的一致性、full surface 接线、
图像 loading/animation snapshot 及页面预算均未交付，P0 仍未通过。

继续审计同一 pin 的 `kitty/graphics_storage.zig`、`kitty/graphics_image.zig`、
`kitty/graphics_animation.zig` 和 `renderer/generic.zig` 后，确认补丁不能只添加 PNG/RGBA 字节：

| 状态 | 必须保留或重建的内容 | 必须验证的边界 |
|---|---|---|
| ImageStorage | 主/备用屏幕各自的 image/placement 映射、自动 ID 计数器、限额与装载策略 | 同 ID 替换、无 placement 的 image、淘汰顺序及源限额不能丢 |
| Image | id/number、尺寸、像素格式与载荷、transient/implicit_id、animation、pending 状态 | 解码后的格式/长度和总预算一致；缓存 generation 应重新生成并保持引用关系 |
| Placement | pin/virtual/relative 三种位置、父 placement、偏移、裁剪、格数和 z | pin 通过已恢复页面的坐标重建，不能复制指针；相对父链与删除语义保持一致 |
| LoadingImage | 已收分块、初始 display、frame、quiet/response 及装载上下文 | 多个完整 APC 组成的 `m=1` 传输仍可能未完成，此时 VT parser 可以在 ground；continuation 不能代替该对象 |
| Animation | root/后续帧载荷与间隔、当前帧、循环次数、播放状态和时钟关联 | detach 期间继续演进；恢复后帧控制与删除仍正确，不能重置为静态首帧 |

`renderer/generic.zig` 当前会调用 `ImageStorage.animationTick` 修改图像状态。
候选架构须把会话动画时钟归权威 worker 驱动，并解决 GUI renderer 的重复推进，
或证明一个同样保持权威状态一致的渲染副本方案；尚无这部分运行证据。

`snapshot/screen.zig` 恢复时重新应用调用者的 image policy，但当前完整 snapshot decoder
构造 screen options 时没有传入源会话的图像限额/装载策略。会话恢复应在资源分配前提供
经验证的策略与预算，不能先按默认限额分配，再把源策略补上。

图像异步解码的 pending 载荷与分块 loading 是两种不同状态。前者仅有预期长度时，
仍需与持有实际输入的解码任务建立捕获边界；后者需要保存已有分块与控制参数。
不得把二者编码成空图或完成状态。涉及文件/共享内存的装载策略由创建时能力决定，
恢复不能因重放历史命令再次读取路径或重做外部副作用。

补丁验收必须包含两屏幕、无 placement 图像、普通/虚拟/相对 placement、滚动历史锚点、
同 ID 替换、多 APC 分块、动画帧控制、pending 取消/完成、限额拒绝，以及恢复后的后续命令。
这些是尚未实现的验收要求，不属于现有 1×1 图像探针已经覆盖的范围。

## P0 继续实施的接口范围

1. **图像资源扩展**：快照涵盖主/备用屏幕的 image、placement、锚点和未完成传输，
   配置严格尺寸/字节/计数上限；明确重新生成的渲染缓存与不能丢的协议状态。
   测试比较真实像素载荷及 placement 几何，不能只检查 ID 或虚拟占位字符。
   不做跨版本解码兼容，扩展只服务当前 pin 的精确构建。
2. **full surface 导入**：先在隔离源码副本实现导入，明确终端最终地址、allocator 所有权、
   parser continuation、renderer 缓存失效以及 renderer/termio mailbox 同步。
   不能直接把 opaque VT handle 塞入 full surface，不能释放 renderer 仍引用的状态。
3. **副作用边界**：区分引擎查询应答与用户输入，在副本解析器产生处禁用前者；
   宿主动作使用 session event sequence 和 attachment generation 路由。
   只在 LIVE 执行当前动作；导入及历史不执行剪贴板、通知、URL 等动作。
   已排队的 mailbox 应答也要隔离，不能只修改将来解析的回调。
4. **状态同步 barrier**：确认 surface 已应用到序号 S，而非仅 socket 已读到 S；
   resize、影响查询的配色与焦点变化和输出有共同顺序。
   运行中的并发模式切换仍按真实终端语义验证，不能用序号宣称消除了传输延迟。
5. **捕获与限额**：选择并验证有界 freeze/clone 或 COW 捕获，记录 worker 停顿和瞬时双份内存。
   同步 encode 不能直接等同于后台一致快照。VT 适配器消费现有 sticky 失败标记，
   full surface 补齐同步失败观测；页面映射也要纳入预算。权威状态可能失效时显式失败，
   不能依赖用户读取 stderr 日志。

所有正式上游修改进入 `patches/libghostty/8159838/` 并补 WHY.md；
P0 接口原型运行通过后才固定生产适配 ABI 与 IPC 恢复字段。
现有探针是基础证据，将来补丁实现图像恢复后要将资源用例升级为完整成功验收，
不能继续以“预期资源丢失”让生产恢复闸门通过。

## 通过与否决条件

以上五项全部有运行证据后，使用真实 PTY 和新的 full surface 验证 vim/less/tmux、
宽/组合字符、备用屏幕、图像、查询、剪贴板隔离、持续输出、resize、输入模式和取消恢复。
同时验证无 display 的运行和新旧实例边界，以及队列/快照预算超限的显式结果。

若必须改成自绘、共享引擎状态的渲染协议或仅进程保活，先记录替代成本和范围/规则变化，
再调整设计；不能隐式切换既定平台引擎。P0 未通过不阻止与恢复策略无关的 backend 拆分，
但不能据此宣布准确重连或固定一个无法实现的恢复协议。

### full surface 资源接线实验

实验 0007 将 0005 的 resolver 接到 0004 的 full Metal replica 构造路径；
在只读 stream 完成最终地址绑定之后、continuation 重放之前安装 hook。
独立无 UI 权威实例先消费临时文件，再由 full surface 处理实际原始命令；
RGBA 与真实 PNG 的红色像素、viewport pin (4,2) 和 canonical state 一致，
源读取失败维持无图像，副本不应答且用户 bracketed-paste 输入仍可用。
缺失或重复事件触发 sticky failure，snapshot 必须失败且不返回可恢复长度。
这只覆盖 checkpoint 后新增的外部资源；checkpoint 内既有资源、loading/动画、
最终提交预算、持续并发恢复及生产 IPC 仍未完成，P0 保持未通过。

### 配额拒绝分歧的运行证据

`probe-session-resource-budget.py` 验证：同一 4 字节 RGBA 临时文件，权威总额度为
3 字节、副本为 4 字节，原始 I/O observer 仍报告成功，源文件只由权威消费。
权威最终拒绝图像，副本却产生正确红色像素及 placement；两边 canonical snapshot
仍相同，sticky semantic error 都为 false。两边额度同为 3（均拒绝）或 4（均成功）
的对照通过，三组变异验证源拒绝、副本差异和像素判据及回滚。
因此恢复元数据至少必须在资源解析前应用源会话额度；仅同步额度仍不能代表
allocator 失败、不同解码器、异步完成及最终 image/placement 提交结果一致。
资源事件的权威边界必须覆盖最终决策，不能将这些差异交给规范快照或错误位侥幸发现。

实验 0008 为 full replica 显式导入源图像额度：配置 resolver 时必须提供额度，
零额度有独立有效标记；snapshot 的 screen 解码前应用额度，配置重载时继续使用它。
重载原本还会重置 image loading limits 并丢失 resolver，实验补丁为副本保留
resolver 与 direct-only 策略。新夹具针对同一 4 字节图像在 3 字节额度下的拒绝，
分别验证首次增量和配置重载后的结果。重载其他配置仍会改变终端状态，不能据此声称
重载本身已和权威排序同步；该切片也未解决 allocator/decoder/最终提交分歧。

### 静态资源快照实验

0009 增加独立原生资源图 codec：主/备用 screen 的完整 RGB/RGBA、image 元数据、
按年龄重建的缓存 generation、image/placement ID 计数器与 pinned/virtual 几何。
pin 以完整 screen 坐标重建，不跨进程传指针。输入有 magic/版本/长度/CRC32C，
并限制 wire 大小和记录数。完整 preflight 先于 pin 池分配，再暂存资源图，校验后发布；
晚期非法输入、分配失败和 wire 超限都必须显式失败并释放资源。
loading、动画、pending、relative 尚未支持，导出拒绝而非降级丢弃。
此 codec 未绑定 canonical checkpoint 的 generation，也未形成生产恢复事务；
C allocator 统计不包含页面 mmap，总内存预算仍未解决。

0010 将静态资源图导入接到 full Metal 构造、stream 绑定之前，核对构造元数据与资源图
中的源额度后再分配图像。在增量前检查已有 RGB/RGBA/PNG 像素和 pin，再验证双屏资源
与后续真实装载。拒绝路径必须返回 NULL 且允许正常释放 App；该测试找到并修正了实验
Surface.init 的字体引用 errdefer 缺口。静态导入仍未绑定 checkpoint generation，
loading/animation/relative、持续并发恢复、页面预算及真实 GUI 崩溃重连均未解决。

0011 增加 relative placement 的无 UI codec：GTIR0002 显式保留父 image/placement
键（含 internal/external 命名空间）及有符号偏移，导入前检查全链存在、无环且深度
不超过引擎上限。父节点可后置；恢复后删除父节点必须继续清理相对后代，不能仅把
坐标展开为 pin。探针包含静态对照、单层、多层、后置、内部父 ID、缺失父节点和
循环七类用例，以及四组目标红/独立对照绿的变异。此处尚无 full surface relative
导入证据，亦未覆盖 loading/animation/pending 或生产恢复事务，P0 保持未通过。

0012 用 renderer mutex 下的实验导出入口观察 full surface 的真实资源图：先在无 UI
夹具通过 VT API 检查相对位置，再比较 surface 导入前后、App tick/绘制调用后及
删除父节点后的完整资源图与 canonical 状态。覆盖单层、多层、父节点后置、内部 ID
及静态对照；资源图对拍不等于屏幕像素验收。

首次运行该夹具发现恢复网格会被 GUI 首次 resize 从 80×24 改成 99×35；原有只在
App tick 前对拍的 full surface 夹具看不见这一点。0012 保留 backend resize 请求，
但禁止 replica 据 GUI 尺寸直接 reflow 逻辑网格。尺寸 owner 是权威实例；当前未有
接受尺寸后的有序回传/应用接口。四组变异覆盖偏移、父命名空间、提前 reflow 和
删除，其中 reflow 变异用不运行 tick 的 import-only 作对照。loading/animation、
checkpoint 绑定、页面预算和生产恢复事务仍未实现，P0 不因本切片通过而完成。

0013 补充同步有序事件原型：output 与显式权威 resize 共用 renderer mutex，
严格接收从 1 开始的连续序号，在 parser/grid 实际应用完成后才发布 applied
sequence。输入 paste 在同一锁下读模式；夹具在查询序号后立即输入，验证
bracketed-paste 开/关改变真实写出字节。GUI 的本地 resize 请求仍不能直接 reflow。
乱序/重复/非法尺寸拒绝后不消耗序号且状态不变；激活有序应用后，绕过序号的旧
原始输出入口显式使副本失效，不能继续导出/查询序号/接受事件。

该序号只属于当前实验 surface，初始为 0，没有 attachment generation 或 checkpoint
序号绑定；输入请求也未携带 applied sequence。resize 内部分配失败的失效路径
尚无故障注入证据；输出/尺寸的局部串行验证不等于并发捕获、IPC 恢复事务、GPU
绘制确认或真实 GUI 重连。loading/animation 与总内存预算仍未解决，P0 保持未通过。

0014 在 surface 构造时验证 bound checkpoint 描述：Python hashlib 独立计算的
SHA256 覆盖域、generation、起始 sequence、源图像额度、canonical/资源图的长度
及原字节。另一份合法 canonical 或 CRC 有效的蓝色资源不能混入原描述；蓝色资源
重算摘要后的成功对照排除“一律拒绝”的假证据。generation 非零且必须显式启用，
否则拒绝元数据；绑定的起始 S=40 会传入最终 Termio，之后接受 41/42。

事件与 applied sequence 查询在状态锁内检查 generation，无 generation 的旧实验
入口无法绕过；错误 identity 不消费序号、不污染有效状态。bound S=0 时也会阻止
原始输出绕过。八组 native 变异与独立对照覆盖这些边界。

该 generation 仍由实验调用方指定，没有 daemon attachment 管理或来源认证；摘要
只是内容/元数据的成对校验。夹具在无并发的同线程选取状态并指定 S=40，不能据此
宣称权威 worker 的原子 checkpoint 或事件日志已实现。输入还未在编码时标记
generation/applied sequence；loading/animation、总内存预算与生产恢复事务仍待
完成，P0 保持未通过。

输入标记不能在 host_write 时追补：当前 pin 的 `Surface.completeClipboardPaste`
在锁内读取 paste 模式后释放锁，随后才编码并 `queueIo`；`encodeKey` 的写请求还
可能进入 `keyboard.sequence_queued`，延后到 `endKeySequence` 才发送。队列 drain
时的 applied sequence 可能已晚于实际编码点，linefeed 转换也必须沿用对应模式。
后续输入证据必须覆盖编码时保存元数据、异步/延迟发送仍保留该元数据，以及普通
键盘、粘贴、按键序列等所有写入来源；仅给 Host.queueWrite 添加序号参数不算完成。

0015 增加实验输入信封：按键在读取 key options 时、粘贴在读取 paste options 时，
于同一状态锁内保存 generation/sequence/linefeed；延迟按键队列与每个粘贴分片
保留原标记。鼠标等已持锁写入在该锁内保存，cursor 动作将读模式与排队合并在锁内。
IO drain 校验身份与 checkpoint 起点/已应用序号范围，但不按 drain 时的新模式
重编码；Host 根据保存的 linefeed 转换，每个分片携带同一元数据，回调在锁外。

确定性探针在编码后、尚未排队时将 S=40 推进至 41，再观察写出字节和旧标记；
覆盖普通键、延迟序列、短/长粘贴、多缓冲区换行转换、鼠标和原始绑定。缺失回调、
未标记、错误 generation、超前/早于 checkpoint 的请求使副本显式失效，不降级到
普通 callback；宿主回复来源拒绝而原始用户 CSI 输入保留。只读释放用上游 testing
allocator 检查，内部消息大小实测从 40 增到 72 字节并显式更新预算。十五组 native
变异与两组过滤单测变异均要求目标红、独立对照绿、回滚复绿。

该证据不是 daemon worker 的 INPUT 验证，也没有实现 attachment 生命周期、
原子捕获、Kitty clipboard 授权桥接、所有宿主副作用或真实 GUI 接线。不能因上述
输入路径已有元数据而宣称全量副作用/输入恢复完成；loading/animation、总内存
预算与生产恢复事务仍未交付，P0 保持未通过。

0016 修复 bound UI reset 对有序状态的绕过：旧路径直接 fullReset，清掉输入模式
却仍报告 applied=40，后续 USER 只有 4 字节而非原 bracketed 模式的 16 字节。
现在 GUI 只发送 generation/已应用序号/kind=1 的低频请求；回调缺失、拒绝都不
修改副本。回调在锁外，权威 reset 事件在同一有序接缝中执行后才推进序号。

七类原生用例将无 UI ghostty_terminal_reset 的 canonical/资源图与副本对拍，
核验接受前仍用旧输入模式、接受后才用新模式；含同步回调中的状态查询/事件应用、
旧 generation、半截 CSI 延续和未绑定对照。七组变异覆盖提前 reset、事件短路、
错误序号、缺失回调假成功、generation 绕过、continuation 丢失及请求泄漏为输入。
这仍是实验动作接缝；clear_screen、配置等其他状态动作、worker/IPC、原子捕获、
loading/animation 与总内存预算尚未完成，P0 保持未通过。

### 权威清屏实验及页面删除问题（0017）

实验将 bound `clear_screen` 改为低频权威请求，并以带 history/outcome 的有序事件
应用状态；GUI 不重复发送提示符 FF。原生初测发现恢复后的页面含有历史前缀时，
上游 `eraseActive` 的 `chunk.start == 0` 假设不成立。0017 为部分页面保留前缀、
压紧后缀并调整 pins，允许删除中间整页。单页/跨多页内容及 pin 回归、既有页面
删除回归及十类原生基线已经通过。夹具独立验证活动图片删除与历史图片保留；
十组变异及最终回滚已通过，页面删除的 103 项现有回归亦通过。该证据仍限于
隔离引擎，未交付 worker/IPC、真实 GUI 重连或完整 P0。

### Kitty 分块加载状态（0018）

新增 loading codec 处理 parser 已在 ground、图像却仍在等待后续 APC 的状态。
GTIR0003 为每个 screen 保存加载元数据及已收字节，与已有 image/placement 图
统一 preflight、暂存和发布，失败可回收重试。路径和回调指针不进入记录，目标
创建策略提供临时目录上下文。隐式 ID 的已分配身份和下一候选计数均需保存。

无 UI 基线已覆盖原始/zlib/PNG、quiet、无 display、parser continuation、双屏、
隐式 ID、限制及分配失败；含隐式 ID 的十一类 full surface 基线通过。十一组
无 UI 和四组原生变异及最终回滚复验均通过。该格式不兼容旧实验 GTIR0002，亦未支持动画帧
loading/animation/pending；它不能证明并发原子快照、页面预算或生产重连。

### 加载记录身份与 preflight（0019）

上游在保存 LoadingImage 时已分配非零 ID，恢复记录必须维持这一不变量。0019
拒绝零 ID，并用已完成图片/placement 加非法加载字段的复合记录检查完整 preflight：
拒绝前不得产生 allocator 调用，且保持目标 canonical/资源状态。十三类畸形记录、
静态对照、full surface direct 控制和五组变异/最终回滚复验均通过；
这不扩大 P0 能力声明。

### 动画 checkpoint 与时钟所有权（0020）

GTIR0004 保存已组合动画帧、gap、当前帧、播放状态、循环预算/计数及权威时刻。
导入完整动画时 addImage 改为按 storageSize 计费，帧纳入存储限额；循环计数饱和
防回绕。GUI 导入策略禁止 renderer 自行推进动画，有序 tick 经 generation/sequence
接缝与输出共同应用，倒退时间不应用也不推进序号。

无 UI 基线、八处分配失败回收/重试和十类原生用例通过；十一组无 UI/五组原生
变异及最终回滚复验全部通过。原生对拍包括双屏和半截 CSI 期间 output/tick 混排，仍不是渲染
像素验收。帧 loading/pending、真实 worker/IPC、原子捕获和总预算仍未交付。

### 帧分块资源身份（0021）

动画帧加载保存的 generation 属于源实例，恢复时只能重绑捕获时仍匹配的引用，
不能复活已被 tick/淘汰失效的上传。GTIR0005 记录匹配事实及完整帧命令，结合
已导入的 ID/number 表重建引用。按编号加载的原始 ID 为零是上游合法状态，
与普通 image loading 的已分配非零 ID 分别校验。最后分块继续由同一上游路径
产生成功或 ENOENT，拒绝在 checkpoint 导入时预先完成上传。

二十八类无 UI 基线、九处分配失败回收及畸形记录零 allocator 调用检查通过；
十四组无 UI 变异及最终回滚通过，十五类 full surface 续传用例、六组原生
变异及最终回滚也均通过。此切片尚无生产 IPC 或并发捕获。

### pending 身份与失败路径（0022–0023）

GTIR0006 将 pending 的预留长度与 complete 像素区分，携带权威 producer origin，
导入时另分配本地缓存 generation。完成/取消按 origin 定位原 screen，再核对图片
ID；两屏可有同 ID 图片，但存活 origin 必须唯一。副本有序事件核对 applied 结果，
不符则显式失效且不推进序号。0022 的十六类无 UI/九类原生，0023 的九类无 UI/
七类原生及各自定向变异均通过；这是隔离引擎证据，正常 stream 解码器尚未异步化。

pending-failure 探针将自定义 allocator 用于资源导入及像素提交，检查逐分配点 OOM、
重试、回收与 malformed 预检。它不覆盖 PageList 的 OS page allocator；C allocator
计数清零不能证明总 RSS 有界。生产 worker、并发原子捕获、总预算和 GUI 重连仍缺失。

pending-failure 的十五类基线、八处分配失败回收/重试、四组定向变异和最终回滚
全量复验通过。十二类畸形记录在移除 preflight 时分别变红，合法 OOM 对照仍绿；
其余变异分别验证 slot 泄漏、OOM 假成功及过期取消意外分配。核心 gate 9/9、
57 个公开符号和快速构建通过。

### 页面 allocator 的宿主策略（0024）

PageList 的原生映射此前不经 C allocator。新增可选 OS page provider，将创建、
快照的临时 TERMINAL/SCREEN/history、备用屏和克隆纳入可观测的页面供给路径。
它要求页对齐、零填充、支持页内独立 decommit/recommit 的 OS 映射；普通 malloc
不能满足这个合同。策略仅从宿主传入，不序列化到快照，也不更改正式 core ABI。

decoder 在开始前配置，随后冻结；解码返回的 Terminal 和克隆继续借用 provider，
其 descriptor/context 因此必须一直存活。原生探针按实际 mmap 页粒度计费，预热池
容量也预留；这种保守额度不同于 RSS。完整终端内存上限还需将一般 allocator、
资源像素/动画、快照临时副本和页面额度合并，并接入 full surface 与 worker。
上游 resize 的备用屏失败恢复还需转成显式资源错误。此处不宣称 P0 通过。

0024 运行证据：八类无 UI 基线、十六个页面 OOM 点与克隆定向测试通过；六组
变异均完成目标红/独立对照绿、SHA256 复原及最终全量回滚复验。默认 full surface
七类恢复回归通过，核心 gate 9/9、57 个公开符号和快速构建通过。

### 共享映射额度与 owner 故障（POSIX 原型）

私有 `budget-posix` 适配器把一般 allocator 与 0024 页面 provider 接到同一全局
账本，每个会话或临时任务另有局部额度及首个故障。准入和释放在 mutex 内更新，
mmap/munmap 在账本锁外执行；live 包含映射中的预留，过度对齐的临时映射也必须
先预留。所有实际映射按 OS 页向上计费，小分配余量包含在额度中；这不是 RSS。

资源拒绝使申请 scope 失效，额度增加不清除故障，其他 scope 保持健康。feed/resize
前后检查故障，所有其他引擎操作由组合层检查 `p0_budget_result`，使内部恢复后
返回 success 的引擎也不能掩盖资源错误。失效后可回收，但不能继续处理输入。
释放 descriptor 前既要 live=0，也要由 owner 保证任务/回调已结束。

十二类基线和十一组定向变异/最终回滚通过，包括八线程争抢四份额度，以及新建
会话拒绝后原会话仍能输入。这是 POSIX 原型，还未接入真实 worker 或原生 GUI；
尚无快照专用 scratch allocator、并发原子捕获或性能校准，不能通过 P0。

### 快照任务 allocator 与发布边界（0025）

P0 canonical/资源编码现在接受显式 scratch allocator，包含 continuation、record
scratch、压缩页只读检查及输出。私有 checkpoint-budget 用同一账本下独立的任务
scope 构造 canonical/resources，任一失败回收全部暂存，只在完整 pair 与合计
wire 上限校验通过后交出所有权。错误阶段与原始编码结果保留，任务额度错误不
清除 source 的健康状态；失效 source 在编码前拒绝。

调用者必须排除源修改直到编码结束，这不是 COW 或并发原子捕获。0025 初版只有
按需 scope，当时尚未保护未来增长容量；后续预留机制见下节。原生 surface 预算
与生产 worker/IPC 仍需实现，P0 未通过。

0025 运行证据：十四类无 UI 基线与压缩活动页定向测试通过；五组任务额度阶梯
扫描覆盖 canonical 和 resources 两阶段拒绝，未发布半份输出。十组变异均完成
目标红/独立对照绿、SHA256 复原及最终全量回滚复验。默认 full surface 七类恢复
回归通过；核心 gate 9/9、57 个公开符号及快速构建通过。

### 峰值额度承诺与后续增长

预算现区分 live 与 committed。已接纳 owner 的整个峰值 grant 计入 committed，
按需 scope 只将实际 live 计入。新任务只能申请未承诺容量，已接纳会话在 grant
内分配只检查自己的剩余额度；普通任务不能占用该会话未使用的增长空间。
释放映射保留承诺，关闭 scope 才归还；扩额须重新准入，缩额不得低于当前用量，
全局额度不得降到总承诺之下。OS 映射本身仍在分配时进行，失败另行报告。

checkpoint source/task 均须接纳，两个 scope 的额度同时预留。十类基线及十二组
定向变异、SHA256 回滚与最终复验通过；包含检查点存活期间真实终端扩张与八线程
争抢四份额度。原共享账本十一组变异和十四类快照回归仍绿。原生 GUI、COW/原子
捕获、生产 worker/IPC 与性能校准仍缺失，不代表 P0 已通过。

### 不可变捕获与源生命周期（0026）

P0 冻结对象以原编码器取得完整小型元数据，复制自含 offset 的页面 backing 和
资源 payload；压缩页只复制压缩块与不含源 memory slice 的页元数据。后续解压、
逐页 wire 序列化及资源 CRC 移到后台，不再需要源 Terminal/节点或其 allocator。
它是独立副本，不是 Screen.clone 或 COW。捕获对象和后台输出可分别纳入任务额度。

捕获阶段仍要求 owner 排除源修改；这是引擎独立性接缝，还没有生产 worker 的
S 序号/事件锁边界。确定性并发探针把 encoder 停在分配回调，owner 在其间继续
输出、重置和销毁，恢复编码后须与边界前同步编码逐字节一致。owner 耗时与映射
峰值记录为性能数据，不能当作生产最坏延迟保证。任务调度/取消、GUI 与 IPC 的
恢复事务仍需实现，P0 未通过。

0026 运行证据：十七类无 UI 基线通过，含双屏同 ID 图片及原屏/备用屏历史顺序；
四组捕获/编码额度阶梯扫描验证回收与重试。十一组定向变异均完成目标红/独立
对照绿、SHA256 复原和最终全量回滚基线。默认 full surface 七类恢复回归、核心
gate 9/9、57 个公开符号及快速构建通过。

### 私有 session worker 与真实 PTY 边界

`core/src/session/worker` 已使用独立 pthread，在该线程创建、读取、写入、resize
和释放 backend，同时创建/释放状态适配器；输出先经 state-owner 应用，再直接
回调字节消费者。状态应用成功后才推进序号，capture 与输出/resize 在相同线程
串行执行。每轮最多一条命令和一次 10 ms 超时读取，不把输出放进控制队列。
当前使用有界轮询，没有 IPC 唤醒或生产 idle CPU 校准。

队列最多 16 条、64 KiB 拷贝 payload，额度包含正在执行的命令。入队原子接受
完整请求；backend 内部另有既有的 4 MiB 尾队列，完成回调中的写成功只表示
backend 接受，不表示对端已执行。投递失败终止 worker 并报告错误，不把已经
入队的命令伪装成未接受。stop 不占队列容量，未执行请求各获一次 CANCELLED；
创建期间已停止则不再启动 backend。join 只供非 UI 生命周期 owner 使用，调用
方须先收束提交者；回调与适配器必须有界，不能同步等待 GUI 或 join 自身。

四类基线用真实 PTY 验证 91×31 尺寸、输出/resize/capture 顺序、旧冻结对象独立、
请求数/字节背压、停止取消和状态故障。状态适配器目前是测试累积器，尚未接通
libghostty 0026、资源预算、查询应答路由、attachment generation 或公开生命周期。
因此这只是实际 worker 的执行框架，仍无生产 daemon 或准确重连的交付。
七组定向变异具有目标红/独立对照绿、SHA256 回滚与最终四类基线复验。

### worker 与真实 libVT 的组合证据

`daemon/probes/session-vt-owner.c` 将上述 worker 的工厂/状态回调接到真实
libghostty-vt 与已接纳的共享 scope。终端模块的 `frozen-budget` 只依赖引擎与
现有 budget/checkpoint 适配器，不引用 Core；Core 仍不知道 Ghostty。源、冻结、
编码 scope 不得折叠，编码不再访问已销毁的源或源 scope。

真实子进程在 CSI/UTF-8 中间停住，由 worker 捕获并报告实际应用序号 S。
后台编码停在 allocator 屏障时，源继续收到 resize/输出并销毁；放行编码后，
canonical/资源必须与边界前同步参考一致。另一实例从已知子进程前缀独立构造
期望，防止 worker 漏喂输出却与同一错误源的参考一起假绿。resize-before-capture
另验 S 覆盖尺寸变更；Kitty 资源在解码后检查真实红色 RGBA 像素。

五类基线及八组定向变异具有目标红/独立对照绿和 SHA256 回滚证据。额度耗尽
不发布部分结果，错误归属任务 scope；全部映射/承诺最终归零。此组合仍是 P0
探针：没有生产回复路由、generation/attachment、编码任务调度取消、IPC 或 GUI
预算，且同步参考编码只作测试 oracle，不是生产捕获工作量。P0 仍未完成。

### 权威查询应答的 worker 路由

worker 在状态工厂中提供私有 reply sink。只有 owner 线程的 apply 回调能写入，
且仅复制到独立 64 KiB 暂存区；返回引擎回调并完成状态应用后，worker 再调用
backend write，然后向字节消费者发布输出。回复不占用户命令队列，不经过 GUI，
也不从 parser 回调递归操作 backend。resize 的 apply 使用相同接缝。

超额会设置不可清除的 worker 故障，即使解析器忽略 sink 返回值也停止会话；
backend 写失败不发布该轮输出，不能继续排空并声称状态可恢复。跨线程调用拒绝，
工厂/捕获阶段越过 apply 窗口调用会显式失败。暂存区是 worker 的固定大小元数据，
尚未纳入生产 manager 的全局准入；不是引擎 allocator �������������射额度的一部分。

Core 新增应答送达、非递归写入、重复发送防护、超额、投递失败及调用归属测试；
十三组 worker 定向变异均有独立对照与 SHA256 回滚。真实 libVT 组合新增 query
基线：子进程检查 DSR 回答恰为六字节 `CSI 3;5R`，排查额外输入后才允许继续。
丢回复/双回复变异分别使子进程无法越过该检查，另一 UTF-8 用例保持绿；与已有
八组合计十组。六类基线最终回滚复验通过，核心闸门 20/20、57 符号一致。

当前仍是 daemon/probes 组合证据，尚无生产 session/attachment API、generation
校验、宿主副作用授权桥、编码任务调度/取消或 IPC/GUI 重连。P0 尚未通过。

### 有界编码任务执行器与取消

Core 的私有 `session/checkpoint-task` 提供一个编码线程和 1–16 个固定槽位。
QUEUED/RUNNING/READY、已领取结果及终态记录都占槽，只有 forget 才归还容量；
拒绝提交不接管输入。任务 context 与 allocator 描述符由组合层稳定持有，输入
和结果各有独立释放回调。回调不在 registry 锁内执行，也不在取消调用线程执行。

取消与 take 由同一锁排序：已接受取消后不能领取结果；执行中取消先返回
CANCELLING，等待原 encode 自然返回，再释放冻结对象和结果，最后发布 CANCELLED。
不使用 pthread cancellation，不中途撤销 allocator。READY 领取后成为 TAKEN，
此时 cancel/close 返回 BUSY，消费者静止读取后 release，再由编码线程释放并到
FINISHED。关闭池会取消剩余任务并 join，只能由非 UI 生命周期 owner 调用。
任务 ID 不复用，generation/S 随结果保留，错误或空成功不发布结果。

八类 Core 基线及十二组定向变异覆盖取消前后顺序、提前释放、排队任务不编码、
满槽与拒绝所有权、空成功、错误结果回收、generation、重复领取、旧 ID 和关闭
时的借用寿命。`daemon/probes/session-vt-owner.c` 现已使用该执行器，而非自行起
encoder 线程；真实 allocator 屏障期间取消仍保留捕获映射，编码结束后丢弃结果，
冻结/编码 scope 关闭，全部映射和额度归零。七类 native 基线及十一组变异通过
最终回滚；核心闸门 28/28、57 个公开符号通过。

这仍是内部执行框架和真实引擎组合探针，不是 attachment 生命周期的交付。
attachment owner 仍须串行处理 detach/generation 校验与 take；全局 manager 的
唯一池、通知/调度、输入/输出增量事务、GUI 预算与生产 IPC 尚未完成。取消不会
中断单次 encode，最坏耗时仍需按已预留的资源和实测校准，P0 未通过。

### attachment generation 与退休任务

Core 私有 `session/attachment` 是每个 session 的连接所有权控制器，借用 daemon
编码池，最多跟踪 16 个当前/退休任务。一个控制器只允许一个活动 attachment；
attach 分配不复用的 generation，每代最多接管一个 checkpoint，提交时覆盖调用
方提供的 generation。拒绝提交保留调用方输入所有权，池满可在回收后重试。

attach、提交、领取和 detach 共用控制器锁；detach 先撤销成员身份，再取消尚未
借出的任务。编码仍在运行时保留旧记录，允许新代附着；旧任务完成/回收不能
清除新代任务。已借出的结果在 detach 后仍存活，只允许用原 lease 释放；不把
内存寿命与连接授权折叠。reap 在回调结束后 forget 池槽并返回原 context，供调用
方在锁外销毁。关闭控制器必须已 detach 且全部 reap，不 join、不关闭编码池。
控制器没有 backend 引用，因此不会将 detach 实现成 terminate。

current 仅验证连接代际成员身份，不表示 LIVE 或用户输入权限。S/L 两阶段恢复
闸门仍待实现。IPC owner 也必须撤销该连接的待发帧并收束已有 lease 的读取；
控制器不能撤回已发送的网络字节，不能仅靠“查询 current 后异步发送”消除竞态。

八类 Core 基线与十二组变异验证并发 attach、旧代提交/领取、借出后 detach、
错代释放、满池重试、失败 context 回收和退休任务不干扰新代；SHA256 回滚通过。
真实 PTY/libVT 的 cancel 用例已改为 detach：encoder 屏障内撤销旧代并建立新代，
旧恢复任务仍保留额度，源继续 resize/输出；随后旧结果不交付新代，安全回收。
七类 native 基线及十一组变异最终回滚通过；核心闸门 36/36、57 符号一致。

这是 P0 的进程内所有权接线，尚未替换公开 Transport 生命周期；manager、
有界增量队列、完整恢复事务、输入授权、IPC 和 GUI 重连仍未完成，P0 未通过。

### 有界增量与两阶段 READY/LIVE

私有 `restore-stream` 为每个 attachment 预分配 byte arena 和事件槽，不在输出
热路径 malloc。捕获 owner 在 S 调用 begin，之后所有成功状态变更（含 resize）
经 worker 的直接 event 回调按连续序号复制。字节/事件任一额度耗尽即失败并清空
该恢复流、撤销 attachment、取消未借出的任务；权威会话继续运行，不能截掉中间
事件后假装 LIVE。零字节事件也占槽，短目标缓冲区只返回所需长度，不消耗事件。

领取 checkpoint 才能接受 READY(S)。该确认固定 L=当前已入队的最后事件，
DRAINING 只允许取出 S+1..L，随后 live_begin 生成 LIVE_BEGIN(L) 并转 WAIT_L。
WAIT_L 仍可继续发送 L+1，避免双方停读写；READY(L) 才转 LIVE。即便 S=L 也要
两个 READY 与中间 marker。input_allowed 同时验证活动 generation、LIVE 及
L<=GUI-applied-sequence<=已交给有序 writer 的序号。dequeued 不是 GUI 已应用；
应用/barrier 完成必须由客户端提供，IPC writer 还须保证帧和 marker 的发送顺序。

恢复中途掉线按整代作废：detach 清空队列并落 DETACHED 相位，这一代之后取事件、
live_begin、READY(L) 与输入一律 STALE，半恢复的屏幕没有任何通向 LIVE 的路径；
下一代从新的 S 从头重来，不继承任何残留事件。

Core 八类流基线与十四组变异覆盖回卷、字节/事件上限、短缓冲区、间隙/溢出、
过期代、两阶段、恢复中途掉线与未来输入；八类 ASan/UBSan 通过。attachment 现有十类基线与
十三组变异另验超额/提前 READY 会撤销成员身份并回收编码任务；worker 十三组
原有变异回归通过。核心闸门 45/45、57 个公开符号与快速构建通过。

真实 PTY/libVT 探针现已在 owner 捕获时启动流，恢复端保留 continuation、导入
资源并逐事件重放 output/resize，再执行 S/L 握手；最终 canonical 和资源 wire
与权威终端销毁前独立捕获逐字节一致。九类基线含 S=L 的 idle 与队列超额后
PTY 继续输出；十四组定向变异均完成目标红/独立对照绿、SHA256 回滚和最终基线。
新增重放 resize 变异使用 S=80×24、后续 91×31 的用例；图片变异的启动标记已
与载荷解耦，要求真正命中 RGBA 校验，避免以“没到检查点”冒充目标断言有牙。

这些是同步 VT 副本与进程内逻辑 marker 的证据，尚未接真实 full surface barrier
或 IPC framing/有序 writer。队列 arena 和出站复制缓冲区也尚未并入全局 manager
准入；timer/资源生产者等其余权威事件接线、GUI 预算、公开生命周期及崩溃重连
仍缺失，P0 不能据此通过。

### full Metal surface 的恢复应用屏障

`daemon/probes/session-surface-owner.m` 已把同一真实 PTY/VT owner、冻结任务、
attachment 队列接到 full libghostty Metal surface。AppKit 主线程只构造/绘制
surface 并发起输入；独立消费线程直接取出 output/resize 并应用到引擎，期间
主线程继续 tick，不把终端输出转入 UI 事件队列。每个事件取出后先观察引擎仍在
旧序号，应用后再查询 `applied_sequence_for(g)`；导入 S 和追平 L 均使用该观察值
发送 READY，不把 transport 的 dequeued 值冒充已应用。第二次 READY 后的用户
输入经 stamped callback 校验 attachment generation/sequence，再核对实际字节。
该回调仅做验证与记录，没有向已关闭的测试 PTY 投递用户输入。

full surface 的 resize 不接受零像素格尺寸。新增 `p0_budget_resize_cells`，旧
VT-only resize 委托零格尺寸路径，保留错误与额度契约；组合探针显式使用固定
10×20 像素格，初始快照和后续 resize 相同。生产协议仍须携带实际格尺寸/配置，
不能从这个固定测试值推导产品字体或窗口策略。VT/full 的头有重复枚举，分别
编译并经 test-only POD view 组合；VT 用生成的 dylib 避免两套静态 UBSan 运行时
重复定义，GUI 仍是 full Metal。生产目标仍为 daemon/GUI 分进程。

七个完整恢复场景（CSI/UTF-8、resize、RGBA、空增量、查询与快照后再次查询）及
一个独立导入对照通过。query-delta 由源 PTY 检查第二个 DSR 的真实应答，GUI
重放时 legacy write、stamped input、剪贴板和通知均保持空，直到显式用户输入。
最终 canonical/资源 wire 与权威端一致，draw/tick 后仍不漂移；RGBA 另经 full
surface 图像读取接口验证。此证据不是屏幕像素验收。

十一组 full-surface 定向变异含主线程消费、漏应用、格尺寸丢失、barrier 缺失、
取出/应用混淆、第二 READY 缺失、输入元数据及图像数据错误，均有独立对照绿、
SHA256 回滚和最终基线。原共享预算十二类基线/十一组变异、VT 十类基线/十四组
变异回归通过；核心闸门 45/45、57 符号与快速构建通过。原生构建使用精确 pin
及实验补丁链，在 Apple Silicon macOS + 完整 Xcode 下干净重建。

这是同进程组合探针：权威端在编码屏障期间继续工作，测试为取得最终 oracle
先关闭源，再消费恢复流；尚未证明真实 IPC 中持续输出时的完整 GUI 重连。
GUI 默认 allocator/renderer 预算、其他权威事件源、全局准入、生产 manager/IPC
及公开生命周期仍未完成，P0 保持未通过。

### 恢复 surface 的宿主内存预算（0027）

实验补丁 0027 让恢复用 full surface 接受宿主提供的 general allocator、原生页
供给器与 sticky health 回调：只有整套 provider 齐备才接受，缺页供给器
（`gui-partial`）或任一额度为零（`gui-denied`/`gui-pages`）时构造直接失败，
不产生半初始化 surface。构造后任何预算失败都是 sticky 的，使序号发布、快照、
资源快照、增量应用与输入 stamp 全部停摆（`gui-growth`/`gui-revoked`）。

预算只覆盖该 surface 自己的 CPU 与原生页内存。共享 Metal Device 由 App
allocator 管理；共享字体 fallback 缓存由 SharedGridSet 的 allocator 管理，
否则 surface allocator 分配、App allocator 释放，UTF-8 回退字形会直接 SIGSEGV。
这意味着 App、共享字体、系统与 GPU 内存都不在每 surface 预算内，它不是 GUI
进程的 RSS 上限。宿主还须知道释放不是同步归还：`ghostty_surface_free` 之后
必须继续 tick app，账本才回到 `live=0` 且 `mapped==unmapped`。

证据：二十五类 full surface 基线（十一类完整恢复、导入对照、两类默认 allocator
对照、五类预算故障、一类恢复中断、一类带活权威的恢复、一类慢客户端溢出、
一类 attach 竞争、两类真实 TUI）、二十二组 full surface 变异、十一组共享预算变异与八组引擎预算变异均通过，含字体缓存归属变异——目标 utf8 以信号中止而对照
`gui-default-utf8` 保持绿。核心闸门 45/45、57 符号、快速构建与补丁闸门 9/9
通过。GUI 全局预算、其余权威事件源、生产 manager/IPC 与崩溃重连仍缺失，
P0 保持未通过。

### 真实 TUI 的 detach→restore 与恢复页容量（0028）

`tui-less`/`tui-vim` 跑真实的 less/vim：先在主屏滚出 100 行 scrollback，再进
副屏整屏绘制，detach 之后继续 resize 重绘并接收按键，最后恢复进全新 surface。
这是第一组同时带真实 scrollback、副屏与程序自绘的用例，它暴露出恢复页的容量
问题：PAGE 头只带逻辑行数，恢复按它精确分配，页一开始就是满的，detach 期间
的 resize 必须拆页，而同样内容的原生页还有标准容量余量、原地 reflow 完成——
文字一致而 canonical 字节不等。0028 把行容量补到原生页在同列宽下会拿到的标准
容量，并让解码从 header 取逻辑尺寸；编码只写逻辑行数，wire 格式不变。代价是
恢复页按标准页大小分配，与原生页一致，仍计入 0027 的每 surface 原生页预算。

因为程序输出不是固定字节串，镜像 oracle 改用独立事件日志重放，并断言副屏已
激活且 `SCROLL001` 仍在 checkpoint 内（副屏自己没有 scrollback，它只能来自
主屏滚出去的历史）。tmux 不直接跑：本机只有 Homebrew 版，拿它当闸门会让闸门
依赖机器；它的查询覆盖由 `query-caps` 以确定性脚本补齐（见下）。

### 复用器能力探测（query-caps）

tmux/screen 开机时先问能力再决定怎么画。`query-caps` 用确定性 `/bin/sh` 脚本
发出同一串查询并逐字校验答复：DA1（`CSI c`）、DA2（`CSI > c`）、XTVERSION
（`CSI > q`）、DECRQM（`CSI ? 2004 $p`），四问四答共 48 字节，收完还要确认
没有多余字节——一问一答，不多答也不少答。

它暴露出一个真缺口：XTVERSION 与 DECRQM 由 libghostty-vt 自带答复，设备属性
却要宿主装 `GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES` 回调，不装就静默不答，
复用器会停在 DA1 上一直等。权威侧现在装上回调，答 `CSI ? 62;1;22 c` 与
`CSI > 1;10;0 c`。DECRQM 问的是 detach 之后才补完的 2004，覆盖「detach 期间
继续查询」。恢复出来的副本对这四类查询全程不答，宿主回写计数保持为 0，否则
程序会收到两份答复。夹具变异两组：拆掉回调、把固件号改一位，`query-caps` 均
红而 `query` 绿。

### 应答与宿主动作的归属（isolation）

`query-caps` 证明的是「副本不答查询」，`isolation` 补的是另一半：有副作用但
没有回复的宿主动作，归属同样只能是权威。脚本在 detach 前后各发一次 OSC 9
桌面通知与配色查询 `CSI ? 996 n`，权威侧装上
`GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION` 与
`GHOSTTY_TERMINAL_OPT_COLOR_SCHEME` 回调并计数：两次通知、两次配色询问，答复
逐字校验为 `CSI ? 997;1 n`，收完确认没有多余字节。

关键在于 detach 之后那一份：它落进增量流，被原样重放给恢复出来的 surface。
副本的 `host_write` 与宿主动作回调全程计数为 0，而同一段字节里的 `SECOND`
确实出现在副本最终 canonical 里（与权威逐字节相等），所以零不是「字节没到」。
机制来自 0004：副本模式下 `processOutputLocked` 把输出交给独立的
`replica_stream`，`colorSchemeReport`/`focusGained`/`sizeReport` 一律提前返回。
变异三组：配色回调不答、答成 light（`isolation` 红、`query` 绿），以及把
detach 后那次 OSC 9 从脚本里删掉——增量流里没有宿主动作，「副本一次都没触发」
就无从谈起，该路径同样红。

剪贴板（OSC 52）由 `clipboard` 单独一刀覆盖。它是唯一一类「宿主必须当场答复」
的动作：写要回 SUCCESS，读要把内容交回去、由引擎编成 OSC 52 答复——回调是同步
的，不回复就等于拒绝（写）或空剪贴板（读）。脚本在 detach 前后各做一轮写加读：
写 `HELLO`/`SECONDCLIP`，权威断言拿到的是解完 base64 的原文（宿主不该再碰协议
层编码）；读两次都答 `PASTE`，程序收到的 `OSC 52;c;UEFTVEU= ST` 共 17 字节逐字
校验。detach 后那一轮同样落进增量流，副本重放时剪贴板动作计数保持为 0——否则
用户的剪贴板会被恢复副本再覆写一次。变异两组：把答复内容换成等长的别的字符串
（`clipboard` 红、`query` 绿），以及把 detach 后那次写从脚本里删掉。

### 恢复后的用户输入（input）

前面几刀都在问「副本不许做什么」，`input` 问的是反面：用户在恢复出来的窗口里
真正打字点鼠标，字节有没有按**被恢复的终端状态**编对、并且只从 p0 输入通道
出去。脚本在 detach 前开 `?1000h`（按钮事件上报）与 `?1006h`（SGR 编码），这两
个模式进了权威快照，恢复时随快照一起回来——副本的编码不是 GUI 自己挑的默认值。

恢复后在同一个真实 Metal surface 上依次验五步，每步先清零再逐字对：

- 中文 IME 组合中：`ghostty_surface_preedit` 加上 `composing=true` 的按键，
  必须一个字节都不出去。拼音半成品漏进 PTY 是用户直接能看见的 bug。
- 中文 IME 提交：`composing=false`、`text` 为已定稿的「中文」，出去的是原始
  UTF-8 六字节，不带 bracketed paste 包裹——提交走按键路径，不是粘贴。
- 粘贴：`ghostty_surface_text` 按粘贴处理，是否包 `?2004` 由恢复出来的状态
  决定（开了 2004 的模式包、`idle`/`tui-less` 这些没开的不包）。
- 鼠标：位置落在左上角，按下得 `CSI <0;1;1 M`、抬起得 `CSI <0;1;1 m`。
- 焦点：脚本在 detach 前还开了 `?1004h`，切走得 `CSI O`、切回得 `CSI I`；
  surface 构造出来默认就是聚焦的，所以顺序是先切走、再重复设同一个值（这一步
  必须零字节，状态没变就不许报，否则窗口一晃程序就被刷屏）、最后切回。

五步全程 `surface_legacy_writes` 与宿主动作计数为 0：输入只经 p0 通道交给权威，
副本不直接写权威的 PTY。变异三组：把组合中的按键翻成已提交（那一步的零立刻
变成六字节）、把左键换成右键（SGR 第一个参数变 2），以及把重复设焦点那一次改
成相反值（该为零的一步冒出 `CSI I`），均 `input` 红、`continuation` 绿。

焦点这一项起初是真缺口：`Surface.focusCallback` 走的是 `io.focusGained`，而
0004 在副本模式下让它整个提前返回——鼠标报告是 `write_*` 消息，经 `queueIo`
被 0015 改道进了 p0 输入通道，焦点报告不是，于是被直接丢掉。补丁
`0029-replica-focus-input` 把「整条丢掉」改成「改道」：`focusGained` 在同一次
持锁里既读 `focus_event` 模式又取 stamp（两者必须来自同一刀），副本模式下把编
好的 `CSI I`/`CSI O` 包成 `ReplicaWrite` 交回权威的 p0 输入通道，并跳过
`backend.focusGained`（那才是宿主动作）。引擎变异把这条分支改回丢弃，`input`
红、`continuation` 绿。

### 恢复中途掉线（gui-interrupt）

`gui-interrupt` 在真实 Metal surface 上只放完第一条增量就 detach，断言这一代
就此终结：队列清空、相位落 DETACHED，取事件/live_begin/READY(L)/输入全部
STALE，`current` 也不再承认这一代。此时 surface 的 canonical 必然不等于权威
最后状态——半恢复的屏幕停在半路，而不是悄悄补齐。随后向该 surface 敲键，字节
被闸门就地丢弃且不缓存重放（探针按「字节有没有到」判，不是按返回码判），
legacy 写与副作用计数仍为零。权威侧的 attachment 槽位立即可被新一代占用。

两层各自有牙：生产变异让 `gt__restore_detach` 只清队列不落相位，`interrupt`
红而 `flow` 绿；夹具变异把 detach 推迟到增量排空之后（`if(false && interrupt)`），
`gui-interrupt` 红而 `continuation` 绿——后者证明这个用例确实中断在恢复中途，
而不是在队列已空之后补一刀。GUI 进程被 SIGKILL 仍未覆盖：那需要真正的跨进程
IPC，同进程夹具做不出来。

### 恢复期间权威继续跑（gui-live）

其余用例都在重放前就停掉了权威 worker，恢复面对的是一份不再变化的历史。
`gui-live` 不停：READY(S) 之后让 worker 再做一次 resize，权威线程在消费线程
排空增量的同时继续 push。断言三件事——L 冻在 READY(S) 那一刻的 last 上，不跟着
`last` 往前跑（否则增量永远追不上活着的会话）；越过 L 的事件留在队列里，
READY(L) 之后沿同一条流送达并上屏；最后由恢复线程停掉权威，surface 与权威
停机时的状态逐字节一致，worker 的收尾契约（finished、无多余权威回复）照常
成立。活着的程序什么时候写 PTY 不归恢复侧决定，所以停机前后各排空一轮，
断言只约束「至少有一个事件越过 L」而不是精确条数。

两组夹具变异有牙：把 emit 挪到 READY(S) 之前，事件就被算进 L，恢复中途再没有
东西越过 L；越过 L 的事件只出队不上屏，恢复出来的表面停在 L、追不上权威。
两者都是 `gui-live` 红而 `continuation` 绿。该模式下权威作用域与 GUI 作用域
同时在册，夹具总额相应放宽到 320 MiB。

### 慢客户端撑满恢复流（gui-slow）

`gui-slow` 同样保留活权威，但把恢复流的事件上限收到 8：真实 Metal surface 吃进
S 和第一条增量之后，消费线程就不再取事件，而权威继续提交 resize。有界队列不会
反压权威，也不会无限增长——撑满时作废的是这一代。断言相位落 FAILED 且 failure
是 LIMIT、队列清空，取事件/`live_begin`/READY(L) 全部返回 LIMIT，输入不被允许，
`current` 不再承认这一代；半恢复的 canonical 不等于权威停机时的状态，向该
surface 敲键字节被就地丢弃，legacy 写与副作用计数为零，槽位立即可被新一代占用。
会话本身没有被慢客户端拖死：队列满之后权威仍继续推进（基线里权威做到 27 个
事件，客户端停在 2），随后由恢复线程停掉它，收尾契约照常成立。

这与 VT 的 `overflow` 不是同一件事：那里的溢出发生在恢复开始之前、没有 surface
参与；这里屏幕已经导入快照并上了一条增量，溢出发生在排空途中。夹具变异让这一代
撑满后照常往下走，`live_begin` 立刻返回 LIMIT——`gui-slow` 红而 `continuation`
绿，说明拒绝来自核心而不是探针自己收手。

### 恢复全程的 attach 竞争（gui-race）

`gui-race` 在 `ghostty_surface_new` 导入快照之前就起一个抢注线程，直到 READY(L)
之后敲完键才停：整个恢复握手期间都有第二个客户端在抢同一个会话。在位的一代不会
被顶掉——抢注方每次 `attach` 都只拿到 BUSY 且不写出 generation，`take` 返回
INVALID 拿不到快照，输入闸门不认它，从增量队列取事件只得到 STALE。在位这一代
照常走到 LIVE，surface 与权威最终状态逐字节一致，输入照常送达；恢复结束后抢注方
仍是 BUSY，只有在位的一代 detach 之后它才接得上。

核心侧两线程同时 attach（恰好一个 OK、一个 BUSY）由 `core/tests/test_attachment.c`
的 `race` 覆盖，这里补的是 GUI 恢复全程这个窗口。夹具变异让抢注方改用在位那一代
的号，它立刻就能从同一条流里取到东西——`gui-race` 红而 `continuation` 绿，说明
用例确实在验证「抢不到的那一方什么都碰不到」，而不是在用一个显然非法的号自证。


### 恢复副本的 Metal 像素证据（pixels）

`pixels` 在真实 PTY 的 detach 前输出红底白字 `FIRST`，detach 后输出绿底
`SECOND`，分别进入快照和有界增量流。新 surface 导入时抓帧，必须有红色和
白色而没有绿色；到 LIVE 后再抓帧，必须保留红色并出现绿色。光标在源端隐藏，
避免白色光标充当文字。抓帧使用进程内 NSView 缓存，不依赖屏幕录制权限。
快照/资源逐字节比对和 S/L 屏障仍照常执行，像素仅补充渲染证据，不验证字形质量。
省去 CGContext 的图像绘制会抓到空帧，目标 `pixels` 失败、`continuation` 对照
通过；回滚后恢复。它补上 P0 验证顺序第 5 步的像素要求，仍不证明 GUI SIGKILL
或生产 IPC 重连。

### Unix attachment 连接死亡的生命周期接缝

`core/src/ipc/attachment-peer.{c,h}` 接管已连接 Unix stream socket；调用者必须先
完成身份/token 校验并取得唯一 generation。它不包含认证、framing 或固定恢复协议，
也不创建线程。单一非 UI I/O owner 定期调用 receive（空闲或发送背压时也必须调用），
才能及时观察对端死亡；这不是独立 watchdog，更没有按心跳超时杀会话。

接管后 socket 有 NONBLOCK/CLOEXEC；每次读写最多等待 0..1000 ms，EINTR 使用同一
单调 deadline。返回 DATA 的长度只表示 socket 读写进度，短写后缀仍归发送者，不能
等同整帧 ACCEPTED。WAIT 保留 attachment。观测到 HUP/EOF 或系统错误就关闭 socket，
先撤销本代并取消未借出的快照，再返回稳定的 CLOSED/ERROR；HUP 上未接受的输入不再
交给 framing。系统错误保留 errno，不能折叠成正常 EOF。旧 owner 的清理只携带旧
代号，不能撤销后来占用的 generation。已进入内核的发送不可撤回；frame buffer、
restore lease 的停止使用与释放仍归连接 owner，peer 不借用/销毁它们。

`test_attachment_peer` 有十三类基线：binary bytes、EOF/发送端关闭、HUP 未接受输入、
SIGKILL、旧代、背压与短写、信号打断、系统错误、CLOEXEC、非法参数、编码中取消、
已借出快照寿命。独立空闲客户端被 SIGKILL 后，下一次 receive 释放槽位；真实 PTY
随后仍接收 `go` 并输出 `SURVIVED`，新一代可 attach，旧 peer 销毁不能撤销它。
十五组隔离源码变异分别让目标失败、同库对照通过，并以 SHA256 复原和复跑基线。

这是跨进程断连回收的基础证据。尚未接生产 listener/manager、身份/token、消息分帧
和恢复 writer；客户端不是 Metal GUI，没有跨进程传送快照或重建 surface。因此
P0 的「GUI SIGKILL 后新进程恢复画面」仍未通过，不能把这里写成生产 daemon 已交付。

### 首个跨进程 Metal GUI SIGKILL 恢复证据

`daemon/probes/session-process-*` 将 P0 组合从共享指针改成独立进程：无 UI 的
`process-server` 持有同一个 worker/PTY、权威 VT、快照任务与 attachment；
posix_spawn/exec 创建的 Metal GUI 只持有继承的 socketpair。实验帧采用独立
`GTP0` magic、严格单版本/类型/flags/长度、显式大端字段与 16 KiB 分块，快照
总长最多 16 MiB。它仅验证恢复路线，不固定 P2 的生产 wire schema；没有监听
socket，因此不把它的继承式信任当作生产身份/token 认证。

第一 GUI 已导入、应用增量、完成 S/L、画出红底白字后，权威才 SIGKILL 它并
验证 waitpid 的终止信号。此时 PTY 空闲且 last 不变，peer 的 EOF/HUP 撤销本代，
所以没有借队列溢出回收槽位。随后同一 PTY 在无 GUI 时继续 resize/写绿底、
完成一次 DSR 的唯一回复并检查无多余字节。新代从更晚的 S 捕获，新 GUI 接收
快照、应用 S 后的 resize，基于引擎 applied sequence 完成两次 READY。颜色
抓帧证明它实际绘制，stamped 的十六字节 bracketed paste 经连接回到原 shell
并被逐字核对，才产生 `THIRD`。该 LIVE 输出继续上屏且最终抓帧出现蓝色像素；GUI 最终完整 canonical
与资源快照同权威停机时逐字节相等，副本 legacy 回写/宿主动作均为零。

数据面没有进 UI 队列：GUI I/O 线程接收、重组、应用增量和发送输入；主线程
构造 surface、绘制及生成用户事件。每 surface CPU/页预算沿用实验 admitted
provider；进程接收缓冲及 App/共享字体/系统/GPU 尚未纳入全局准入。GUI SIGKILL
不走 allocator 清理回调，交由 OS 回收；正常退出则必须 tick 到 scope 平衡。

两类进程用例（`roundtrip`/`kill`）、十一类 codec 与十九组定向变异的命令是
`python3 scripts/probe-session-process.py --mutations`；它还回归二十六类已有
full-surface 基线。生产 daemon、发现/列表/认证/token、真实应用接回入口与包内
helper 均尚未实施，独立受控 PTY 的 GUI 崩溃恢复证据不替代这些交付，P0 仍未通过。


### GUI 进程内多 surface 与接收缓冲的统一准入

P0 新增 `terminal/session-state/gui-budget`，由 GUI 进程持有一个共享 ledger，
取代每 surface 单独建池。每个 lease 在 admission 锁下成组预留 general/pages/wire，
失败完整回滚，既不释放其他 surface 的承诺，也不遗留失败 scope 或占据数量槽位。
所有增长余量先计入 committed；wire 的 canonical/resources 接收及最终对拍缓冲
不再走普通 malloc。wire 故障纳入同一个 surface 的 sticky health，拒绝后不能继续
发布 applied sequence 或输入；其他 lease 继续可用。故障 lease 等 renderer/IO
停用、tick 完延迟释放和接收缓冲归还后可释放并重试，不因故障永久锁住准入。

跨进程 GUI 用例同时创建两份真实 Metal surface，第三份因共享字节额度失败；
释放一份后再次成功。辅助 surface 的 wire 耗尽使它停止发布状态，主 surface
仍能恢复、逐字节对拍、发送实际输入并画出最终蓝色像素。八类预算基线另覆盖
两线程争抢一个额度、数量上限、部分失败回滚、存活分配阻止释放与策略溢出。
十四组预算变异加三组原生接线变异均完成目标红/同库对照绿和 SHA256 回滚。

这关闭的是 GUI 进程内多 surface 的 CPU/页/wire 超卖缺口。App/共享字体/系统/GPU、
截图、固定帧缓冲与线程栈尚未纳入总量，多个 GUI 进程也尚无共同协调者；不能据此
宣布完整资源上限或 P0 通过。生产 manager、listener、认证与正式协议仍未交付。


### 快照导入前核对发送端摘要

跨进程实验原先在 GUI 端为收到的缓冲计算引擎 checkpoint digest，发送端没有
提供内容摘要，因而不能在导入前发现传输损坏。现在 SNAPSHOT/EXPECT 的 104 字节
元数据携带 canonical/resources 各自的 SHA-256；GUI 收齐有界缓冲后先核对，再
构造 surface 或作最终状态对拍。它与引擎 checkpoint 的 g/S 绑定是两层检查。
两个独立篡改变异使摘要不一致，目标在导入前拒绝、同库 legacy 对照通过并回滚。
此机制不是认证或 MAC；恶意对端可同时替换内容和摘要。正式协议/构建标识和
生产身份/token 尚未交付，P0 仍未通过。


### 跨进程 READY 等待不停止读取增量

`crossing` 在 READY(S) 固定 L 后让 owner 继续 resize，事件序号超过 L。
服务器先排空到 L，发送 LIVE_BEGIN(L)，在等待 READY(L) 时继续发送越过 L 的
增量。客户端已经确认 L，但仍消费这些增量；LIVE_OK 的确认边界保持 L，之后
用户输入携新的实际 applied sequence，权威按该序号验证。之前进程探针恰好
没有 L 后事件，客户端只读取一条 LIVE_OK 的实现无法覆盖这个顺序。

两组定向变异（漏送越界事件、客户端等确认时停止应用增量）均目标失败、普通
恢复对照通过。最终 canonical/resources 和实际绘制仍验证。这里只验证受控的
一条 resize 越界，不声称高吞吐双向背压或完整 production writer 已完成。


### 有界 blob 的时间上限

P0 跨进程 codec 现将一个 blob 的单调 deadline 传递到所有帧，不再在每块
重新获得 5 秒。单帧本身的 header/body 也共享同一 deadline。发送或接收
部分内容后超时必须失败并终止流，不能把部分前缀冒充完整对象或继续解析。
慢分块测试以每次 30 ms 的 WAIT 驱动两个各延迟 2.7 秒的 body，逐帧可完成
但合计必须拒绝，发送/接收各一组变异有独立对照。此改动只限制单 blob，
不是整个 attachment 恢复的总 deadline，生产断连清理仍待正式 writer 接线。


### 跨进程快照严格绑定本次引擎产物

内容摘要只能证明缓冲没有变化，不能证明它属于客户端的解码器构建。进程
探针现在为本机 VT/full 实际链接产物、pin、目标、实验补丁链与快照契约生成
SHA-256 指纹；名称/值有长度前缀、顺序固定，避免串接歧义。发送端与客户端
各编入本次组合的同一指纹，快照元数据携带指纹，客户端在分配接收内容前
核对。不同指纹明确拒绝，不尝试旧格式读取或重启权威会话。

发送指纹翻位的定向变异在构建匹配检查处失败，legacy 对照通过。实际 archive
内容可能因重建细节变化，严格拒绝是本实验允许的保守行为；没有声称不同机器
重建必然得到相同值。正式生产 handshake、构建发布标识、身份/token 仍欠，
这里的快照拒绝不替代连接版本管理或产品升级流程，P0 仍未通过。


### 捕获、编码与跨进程恢复的测量口径

P0 进程探针增加阶段时间和预算观测：owner 不可变捕获、异步编码、本地发送
接受与 GUI 报告绘制完成分开计时。发送接受不等于应用；绘制完成包含主动
等帧和辅助 surface 检查，不能拿它作输入延迟。capture/encoder scope 的峰值
与权威 ledger live/committed 峰值单列，macOS authority 峰值 RSS、累计 CPU
另由 getrusage 记录。尚不包括 GUI RSS/空闲 CPU 或完整系统/GPU 内存。

这些是观测输出，不是新增性能闸门，没有用测出来的小负载数字自行定义通过
阈值。大 scrollback/资源用例、同环境旧路径对比、输入延迟分位数与单会话
开销仍待补齐；在这些证据缺失前不宣称 P0 的性能预算已验收。


### 从小屏幕扩大到真实 PTY 的千行历史

新增 `history` 进程用例生成 1000 行文本，再清可见屏进入原交互流程；捕获前
读权威引擎实际 scrollback 行数（至少 900），不把输出的行数直接当成保留行数。
同一不可变快照经异步编码和多帧 socket 传输，导入真实 Metal 副本并进行完整
canonical/resources 比较，随后实际输入仍必须被原 shell 逐字接受。辅助 surface
导入/预算故障隔离也在该历史负载上执行，不只在微型快照上成立。

删除负载必须使目标红且普通恢复对照绿。性能仍仅是确定性千行文本的一档样本，
不是完整旧路径对比或最坏资源情形；P0 预算/性能验收保持未完成。


### 编码失败回收撤销对应的在位 attachment

发现私有 attachment 的失败缺口：编码任务进入 FAILED 后，reap 只删除任务，
仍保留 active，于是客户端既拿不到快照，又无法重新 attach（BUSY）。现在
reap 成功回收 FAILED 任务时，若该任务 generation 仍在位，就撤销授权并清空
恢复流；错误仍通过 retired.error 原样返回。编码失败不停止 session/PTY。
若失败属于已 detach 的旧代，则回收仅处理旧任务，不撤销后来一代，也不清掉
其 current_task。owner 必须定期 reap，任务线程不会自行调用连接 owner。

`attachment_failure` 证明失败/释放后能 attach 新代、提交并消费完整快照；
`attachment_failure-stale` 在旧失败回收前先让新代提交任务，再验证新代照常完成。
两组定向变异分别漏撤销和无条件撤销新代，目标红、独立对照绿，SHA256 回滚
复绿。修复的是 core 私有生命周期，不宣称已完成生产 daemon 错误呈现与重试 UI。


### 恢复元数据不只比网格

跨进程 PTY 用例现在显式生成初始与 detached 更新两版 OSC 2 标题、OSC 7 cwd。
owner 在每次捕获和最后释放 terminal 前通过引擎 TITLE/PWD API 逐字核对预期，
避免只从“源和副本相等”推出实际上从未设置的字段已恢复。首次 GUI 导入初始
元数据，新代接回导入更新值，完整 canonical 状态比较继续验证副本一致。
四组删除变异覆盖两个字段、两个阶段，均有同库独立对照。

cwd 在引擎里保留原 URI，尚无路径解析/安全策略或产品 UI 接线；会话 runtime
自己的 OSC 133/7501 摘要仍需独立验收，不能拿标题/cwd 对拍替代摘要迁移。


### daemon 摘要的私有 C 记录表基础

OSC 7501 的产品记录当前由 Apple `ProgramStatusStore` 持有，不能把 GUI 进程
生存期当作 daemon 会话摘要寿命。新增 `core/src/session/program-status` 提供
无引擎/UI 依赖的 owner 表：只接受已解码的报告，不重写 VT 规则或引擎 app 继承。
每条记录复制 id/app/title/message 及 state/kind/progress，按首次插入顺序更新，
clear 按 slash 边界删除子树（空 id 则全部删除）。非法/超额/分配失败不改旧表。
快照为独立深拷贝，可在显式发布后移交编码线程，源表仍由原 owner 独占。

表上限显式配置：1..1024 条、持有字符串总计最多 1 MiB，各字段最多 4096 字节。
字符串计费包含 NUL，不包含固定 entry 数组、对象头与 allocator 开销；更新期间
还会临时持有一份新记录，capture 时另有整表副本，不能把持有字符串上限当作
整个恢复的内存上限。后续 manager 必须把这些开销和并发快照纳入准入。

六类单测、十二组变异及 ASan/UBSan 验证子树边界、未知进度、复制/别名寿命和
限额回滚。当前只交付私有存储基础，尚未接 VT 回调、worker 捕获或 IPC/S/L；
Apple 模型尚未迁移，OSC 133 命令摘要/文本显示清洗也未交付，P0 保持未通过。


### headless 权威收到的 OSC 7501 不再只存在 GUI

进程恢复探针已将 headless VT PROGRAM_STATUS 回调接到私有 C 表。字段来自
引擎解析/规范化，不另写 VT 规则；callback 临时 span 经有界转换后复制，字段
过长/NUL 或存储拒绝形成 owner sticky 失败。真实 PTY 发 working、detached
blocked、恢复输入后的 done，逐字核对 id/app/message 与状态/未知进度。

同一 owner freeze 回调复制程序表和 VT 状态，摘要随捕获对象在同一 S 发布，
编码线程只读其独立副本。源更新为 done 后，仍借出的副本必须保留 blocked，
直到 executor 释放结果。编码失败或取消而无结果时，input cleanup 也释放摘要。
三组变异保护回调接入、detached 状态来源与捕获遗漏。

仍无摘要 wire 编码/客户端导入、OSC 133 命令生命周期和产品 UI 迁移；摘要的
有界 malloc 尚未纳入 allocator 准入。此处只补权威与不可变捕获接线，不能把
引擎 canonical 一致宣称为整个会话摘要已恢复，P0 未通过。


### 客户端在 READY(S) 前接收程序摘要

实验快照现在包含独立、定长上限的程序记录表编码及长度/内容 SHA-256。
摘要与 VT/资源使用同一快照 generation 和 S，客户端在 READY(S) 前验证并
事务性解码，表不保留 wire 指针。格式不传 C struct，显式大端长度、拒绝
重复 id/clear/未知值/NUL/越界与尾随字节；损坏或导入遗漏不能进入恢复握手。
客户端直接核对初始 working 与接回 blocked 状态，程序报告不会因恢复历史
回调而被重新执行。完整状态还通过原 VT/资源快照逐字节比较。

最终 EXPECT 的 done 是测试 oracle，并非实时摘要事件。因此这只完成摘要的
捕获/初次恢复传输，LIVE 期间的摘要更新、OSC 133、产品 UI 与全局 allocator
预算仍欠。接收 wire 已计费，解码表与服务器序列化栈空间仍须纳入准入。


### 实时摘要与触发它的输出共享序号

P0 权威现在把发生摘要变更的输出和当时的记录表编码成一个 bounded restore
队列项，使用相同 S+k，避免 owner/发送线程竞态读取“最新表”。低频摘要不是
UI 字节流队列：实际输出仍经 transport/I/O 线程直达 surface。实验 writer
把配对项拆成标记输出、同 g/sequence 的摘要 metadata 与 blob；客户端在一个
I/O 消费步骤内完成摘要校验/替换后再推进流 cursor。未变更摘要不额外编码。

客户端在最终诊断摘要导入前检查 LIVE 表为 done，真实 PTY 输出和最终网格/像素
证据仍独立成立。三组变异覆盖遗漏配对、错误 generation、摘要损坏。
引擎 applied sequence 与客户端摘要不是通用原子 UI 发布机制；实时 UI、OSC 133、
大流量 writer 和完整 allocator ledger 仍待实现。这里的有界配对项还需生产侧
显式处理超队列额度，不能把当前测试的单记录负载当作最大摘要规模的验收。


### 摘要配对项的最大容量与累计溢出

实验队列单条容量改为 82576 字节，使合法最大摘要与 16 KiB 输出不因旧
64 KiB arena 被拒绝。32 条记录用满 65536 持有字符串字节，去掉 wire 不带
的 128 个 NUL 并加字段/头后为 66056 字节，配对项为 82448 字节；真实 C
attachment 测试确认它可完整入队。再入一条必须 LIMIT、清空并撤销该代，
下一代仍可复用该槽位。owner event 允许 LIMIT，不将该返回直接断言为进程
失败；生产 writer 的显式错误交付/断连回收仍需接线。

三组变异分别还原旧容量、遗漏撤权、遗漏清空，均目标红、独立对照绿，回滚
后两类基线复绿；整个相关 C 路径有 ASan/UBSan 证据。这是核心容量/背压证明，
不是完整 GUI 最大摘要压力或 production 自动重连证明；全局预算仍欠。


### OSC 133 命令摘要的纯 C owner 基础

新增 `core/src/session/command-status`，接收引擎已解码的 semantic prompt
事件，不解析 VT。命令/错误文本分别最多 4096 字节，状态自身持有固定数组，
无回调指针；owner 串行应用后可发布独立值副本，不直接将结构布局用作协议。
input/prompt/future kind 不清上一条命令，output_start 重置上一条结果并标记
running，command_end 结束运行并保留可选退出码（包括负值）与错误文本。

running 不依赖退出码或时钟是否存在；显式时间有效位允许 0 纳秒为真实起点。
只有有运行起点、两端时间有效且没有倒退时才计算 duration；只收到结束、缺
时钟、倒退时不编造耗时，重复结束也不会沿用过时的 duration。非法文本先拒绝，
保留整个旧值。原始文本仍须在展示层清洗，不能把“有界”当作“可信”。

六类基线、十三组变异、ASan/UBSan 已验证三态与复制寿命。当前是 daemon 共享
模型基础，未接 VT/worker/摘要 wire、未迁移 Apple 的 LastCommand，未实现
prompt_start 对 working/blocked 程序记录的清理联动，P0 保持未通过。


### 命令生命周期接入真实权威引擎

headless VT 的 SEMANTIC_PROMPT 回调已接 C 命令模型，复制解码 command/error，
保留 has_exit_code 与单调时间有效位。测试 PTY 在 detach 前 start、detach 后
end/7、恢复输入后下一条 start/end 无退出码，捕获时直接核对三态。
同一 S 的 freeze 回调按值保存命令状态；后台编码读取副本，owner 后来执行
另一条命令不会改写已捕获的命令/退出码/耗时。

三组变异覆盖未安装回调、未发送退出码、遗漏值捕获。该值目前未 wire 编码，
客户端尚无命令摘要导入；OSC 133 提示符与 OSC 7501 的清理联动、原始错误文本
的真实 PTY 覆盖及 UI 迁移未完成。因此它是服务端所有权证据，不是完整命令
恢复交付，P0 与生产 daemon 保持未完成。


### 命令状态跨进程快照传输

命令快照新增独立 GCS1 bounded codec，不复制 C owner 结构。显式传 running、
exit/duration 有效位、退出码、耗时与文本，排除 daemon 的单调起点；客户端
不得把外部起点用于自己的计时。codec 用独立固定字节 oracle 验证大端、负值
和存在位，0 与未知不混淆；拒绝语义矛盾、未知字段、越界、NUL 和尾随字节。
快照元数据带长度/SHA-256，命令与 VT/程序表使用同一 generation/S。

客户端在 READY(S) 前导入并核对运行中/已结束退出码 7 状态，原有 VT/图像/
像素及真实输入验证保持独立。最终 EXPECT 的未知退出码用于诊断，不是实时
命令推送；实时事件、OSC 133 prompt_start 的程序表联动与产品 UI 仍欠。
命令 wire 缓冲已计费，其他固定栈/状态对象尚未完成总预算，P0 未通过。


### 命令 start/end 的实时摘要交付

命令回调单独维护 dirty，和程序表 dirty 共同决定是否把输出与两份摘要作为
一个同序号队列项冻结；writer 不借用 owner 当前命令。paired metadata 分别
校验 GPS1/GCS1 长度与 SHA-256，客户端两份均成功后推进协议 cursor。引擎
输出应用及未来 UI 发布仍不因此变成一般多对象原子事务。

真实 PTY 的 done/THIRD 与后续仅 OSC 133/FOURTH 之间由测试门控分开，确认
没有程序表变化时命令仍能更新。客户端在最终 EXPECT 前核对命令已停止、退出码
未知、耗时有效；command dirty 遗漏与命令 digest 损坏必须目标失败、对照通过。
最大 envelope 包含程序表满额及命令/错误满额，固定 arena 增为 90804 字节，
累计溢出仍撤销该代。固定栈/对象的总 ledger、prompt_start 联动与产品 UI 尚欠，
P0 和生产 daemon 未完成。


2026-10-20 的 prompt_start 联动：私有 C 程序表只按记录状态删除 working/
blocked，保留 done/error/idle 子项并释放字符串预算；捕获副本独立，input_start
不清理。真实 PTY 的五条记录经 OSC 133 B 仍五条、A 后三条，由权威回调核对；
这不是客户端中间显示证据。清理段和后续仅命令更新用 PROMPT 门控隔开，继续
要求 command dirty 遗漏使旧断言变红。程序存储七基线/十六变异及 ASan/UBSan、
process 全套 84 变异/26 surface 基线与 hash 回滚、核心 73 测试/57 ABI 通过。
仍是 P0 探针，真实错误报告、统一预算及生产 daemon/产品 UI 未完成。


2026-10-20 的 shell error 验证：真实 PTY 用 OSC 133 D 报 detached failure/
退出码 7，重连快照同时保留；后续仅命令 LIVE 段报 unknown exit failure，
客户端在最终 EXPECT 前收到文本且退出码仍未知。旧冻结值独立，不以 error
推断非零退出码。新增 callback 丢弃 error、shell 漏报 LIVE error 两变异，
且旧漏报码变异保留 error 参数以免混淆。完整 86 变异/26 surface 基线与
SHA256 回滚、核心 73 测试/57 ABI 通过，日志见
`tmp/process-command-error-{verification,gate}.log`。仅覆盖不可信 shell 字段，
未完成低层 PTY/引擎/资源故障报告、全局预算或生产 daemon/产品 UI；P0 未通过。


2026-10-20 的摘要分配域：私有 C 程序表结构/条目数组/字符串及副本统一经
可选 allocator 分配，按精确原尺寸释放；默认 libc 入口不变。allocator context
须活到所有副本释放后，调用方串行访问。替换先新后旧，瞬时预算不足不改旧值。
GUI checkpoint/LIVE 的 GPS1 解码使用已准入 wire scope，旧表、新表、接收缓冲
同时计费，成功解码核对 live 增长，表关闭后方可释放 lease。
八类存储基线、21 变异/回滚、逐步创建复制失败与替换额度拒绝及 ASan/UBSan
通过；完整 process 88 变异/26 surface 基线和核心 74 测试/57 ABI 通过。
日志见 `tmp/program-allocator-{mutations,gate}.log` 与
`tmp/process-program-allocator-verification.log`。解析栈/命令 POD、owner 默认
分配及其他对象仍待统一 ledger；低层故障报告、生产 daemon/产品 UI 未完成。


2026-10-20 的 daemon 摘要预算：权威表占 source scope，冻结副本经私有
copy-with-allocator 入口显式占 encoder scope；capture 中间态释放不结束副本
预算。引擎释放后 source 仍保留原表占用，最终原表关闭后才关闭 source；
encoder 结果释放同时回收副本和快照。默认 copy 继承源域，显式 NULL 目标域
使用 libc；目标失败不改源值或源用量，部分目标全部回收，源关闭后副本独立。
九类存储用例/23 变异/回滚、ASan/UBSan 及完整 process 91 变异/26 surface
基线、核心 75 测试/57 ABI 通过。新增绕过 source、误用 source 保存副本、
漏释放原表三变异均目标红/独立对照绿。证据日志
`tmp/program-domains-{mutations,gate}.log`、`tmp/process-program-domains-verification.log`。
固定栈与队列/任务/worker 等对象还需统一预算，低层故障报告及生产 daemon/
产品 UI 未完成；P0 未通过。


2026-10-20 的恢复队列预算：私有 allocator 类型抽到 base 头，程序表保留原别名。
attachment 控制器、字节环、事件数组的预算构造覆盖三块分配；默认 libc 行为不变。
失败清理全部部分分配，BUSY close 保留，detach/溢出/新 generation 复用不追加
分配或退还已保留数组额度，真正关闭按原尺寸归还。daemon 探针使用 source
scope 创建队列，程序表用相对用量检查避免队列额度掩盖摘要漏计费。
restore 九基线/19 变异、attachment 十二基线/18 变异、程序表九基线/23 变异
与回滚、新用例 ASan/UBSan、完整 process 92 变异/26 surface 基线及核心
77 测试/57 ABI 通过。日志见 `tmp/queue-budget-*.log` 和
`tmp/process-queue-budget-verification.log`。固定栈、任务池/worker/peer 等尚未
纳入总 ledger；低层故障报告、生产 daemon 与产品 UI 未完成，P0 未通过。


2026-10-20 的运行时堆预算：任务池（固定槽位）、worker（请求队列/64 KiB
回复缓冲）及复制 input/resize payload 经私有 allocator 进入 source scope。
默认 libc 构造不变。context 活到 close/join，worker 回调需跨线程安全；池
BUSY close 不归还额度，实际 join 后精确释放。输入额度拒绝返回 NO_MEMORY、
无入队/回调，后续仍可接受满额请求并取消回收。线程栈和 factory 内对象另计。
worker 十基线/17 变异、task 九基线/14 变异与 SHA256 回滚、新分配用例的
ASan/UBSan、完整 process 94 变异/26 surface 基线、核心 79 测试/57 ABI 通过。
独立 worker sanitizer 编译补齐 Darwin 宏和 core 辅助链接后通过，辅助 core
对象未全量插桩。日志见 `tmp/runtime-heap-*.log` 与
`tmp/process-runtime-heap-verification.log`。OS 线程栈、peer/backend/宿主对象
仍需总 ledger；低层故障报告和生产 daemon/产品 UI 未完成，P0 未通过。


2026-10-20 的 IPC peer 预算：私有 adopt allocator 将对象计入 source，默认
libc 入口不变。回调无效/分配失败均不转移 fd、不改标志或 generation；HUP
关闭 fd/撤销旧代后保留对象额度到显式 close，旧对象关闭不撤销新代。I/O owner
adapter 单独统计 peer 请求尺寸并转发 source allocator（实际预算按映射页），
避免 worker 载荷并发回收干扰入账检查；最终两类用量均须清零。
十四 peer 基线/17 变异与回滚、新用例 peer/attachment/task/restore 的
ASan/UBSan、完整 process 95 变异/26 surface 基线及核心 80 测试/57 ABI 通过。
日志见 `tmp/peer-budget-{mutations,gate}.log` 与
`tmp/process-peer-budget-verification.log`。OS 栈/socket buffer、backend 与宿主
对象尚需总 ledger；低层故障报告、生产 daemon/产品 UI 未完成，P0 未通过。


2026-10-20 的线程容量：私有 POSIX options 明确设置 stack/guard，不合法或
属性失败显式拒绝，不回退默认。任务池/worker 保留原构造入口；宿主先预留容量，
在 join 后归还。P0 每条线程承诺 1 MiB+两页，source 相应调减，两条线程与
原 scope 总预留仍不超过 256 MiB。线程 scope 只计 committed，live 为零；
实际栈内核对局部地址、Darwin stack size、Mach guard 映射和预留上界。
本机 stack 请求/报告/guard/预留分别 1048576/1060864/16384/1081344 bytes；
额外 12288 仅当前宿主观测，舍入超额须拒绝证据，TLS/内部对象等另算。
独立本机 probe 观察 join 后原栈映射不再覆盖原地址，不外推其他 pthread cache。
三类线程基线/六变异与回滚、ASan/UBSan（关闭 fake stack 以观测真实局部地址）、
原 worker/task 变异、完整 process 99 变异/26 surface 基线及核心 83 测试/
57 ABI 通过。Darwin 才执行原生映射观测，其他 POSIX 不冒称已验 guard 映射。
日志 `tmp/thread-budget-*.log`、`tmp/process-thread-budget-verification.log`。
backend/GUI 线程、TLS/pthread 内部对象及其他宿主对象仍需总 ledger；低层故障
报告、生产 daemon/产品 UI 未完成，P0 未通过。


2026-10-20 的 PTY 堆预算：句柄、启动路径/登录 argv[0]/argv/环境指针数组/
TERM 和输入尾队列走 source 分配域，最大对齐分配头计原尺寸与头部，父进程
fork 后回收启动数据，子进程不调用分配器。默认 libc 保留 realloc；预算扩容
同时计旧/新两份，失败保留旧队列。4 MiB 尾上限用减法防溢出；登录参数 OOM
明确失败。write 报错前可能已发送部分前缀，不承诺请求原子回滚或可盲重试。
四类预算基线/八变异与 SHA256 回滚、真实 2 MiB PTY/cat 顺序对拍及 ASan/
UBSan、原登录 11/transport 12 检查与 inheritance 两变异、完整 process
100 变异/26 surface 基线、核心 87 测试/57 ABI 通过。macOS 登录 OOM 的
变异仅 Darwin 执行，其他平台不外推。日志 `tmp/pty-budget-*.log` 与
`tmp/process-pty-budget-verification.log`。waiter OS 栈、libc passwd/TLS/事件
内部对象及其他宿主资源未全量计费；低层故障报告、生产 daemon/产品 UI 仍欠，
P0 未通过。


2026-10-20 的 waiter 栈：PTY 新私有 options 构造在分配/fork 前完成 pthread
属性预校验，default/allocator 入口不变。私有原生 C 查询借用 waiter ID，
非 PTY 显式 UNSUPPORTED，不转移 join 所有权。P0 查询真实 waiter 的栈/guard
映射、独立线程与容量，Darwin 实测请求/报告/guard/预留分别
1048576/1060864/16384/1081344 bytes。三条线程均已预留，source 调减至
60.90625 MiB，总承诺仍 256 MiB；backend close/join 后、worker join 后才
释放 waiter scope。线程预算计 committed 容量，不伪造 live 分配量。
五类 PTY 基线/12 变异与回滚、相应用例 ASan/UBSan、inheritance 两变异、
完整 process 102 变异/26 surface 基线及核心 88 测试/57 ABI 通过。native
映射证据及忽略属性的 PTY 定向变异仅 Darwin，不冒充 Linux 映射验证。
日志 `tmp/waiter-stack-*.log`、`tmp/process-waiter-stack-verification.log`。
TLS/libc 内部对象及其他宿主账目、低层故障报告、生产 daemon/产品 UI 未完成，
P0 未通过。


2026-10-20 的 worker 终态：私有固定 POD 在资源/请求清理后、finished 前发布，
到 join 前可并发查询并拷值。PENDING/INVALID 不冒充终态，初始化前未知序号与
初始化后的合法 0 分开；READY 保存结束路径、原 GT 错误、失败阶段和最后成功
权威 apply 序号，不代表 GUI ACK 或 apply 失败后状态可恢复。finished 可能早于
start 返回，回调内查询需要先安全发布句柄。
显式 observe_exit 区分正常 CODE、SIGNAL、NOT_FOUND 与 UNSUPPORTED；非法观测
报 PROTOCOL。PTY 保存真实 waitpid 原因，新报告不从 legacy exited() 的 0 或
128+signal 猜测。公共 SSH legacy 占位 0 未修改；其新观测缺失明示 UNSUPPORTED，
没有冒称 SSH 退出迁移已完成。backend factory 指针失败仍是旧的 IO，void close
亦尚无独立错误路径。合法 capture 失败局部处理，空 frozen 违约则致命。
17 类真实 PTY/故障适配器基线、18 变异与回滚、新用例 ASan/UBSan、原 worker
17 变异、完整 process 103 变异/26 surface 基线及核心 105 测试/57 ABI 通过。
适配器注入故障不冒充内核实际失效。host 在 join 前取结果、join 后保留值，
尚无 IPC/产品 UI 终态接线。日志 `tmp/worker-result-*.log` 和
`tmp/process-worker-result-verification.log`。完整故障通知、资源总账和生产
runtime/daemon 尚欠，P0 未通过。


2026-10-20 的终态 IPC：固定 GWR1/PW_RESULT 在最后增量之后发送，客户端
对齐 generation/消息序号/权威序号/cursor，拒绝重复终态和其后的数据，收到后
关闭本地输入门禁。EXPECT 仍是诊断，不能重新授予 LIVE。窗口 title 属性在主
线程更新；这不是文字像素证据。实际 surface text 的 LATE 被门禁拒绝，变异
会先清旧输入缓冲，避免旧 metadata 检查造成假红。
真实 PTY 在 FOURTH 后经适配器触发 read error，worker 报 READ/IO，新的 Metal
GUI 在 EXPECT 前收到 ERROR 并保留最后网格。此是注入故障，不冒充内核错误。
十一 codec 基线/十一变异/回滚及 ASan/UBSan，五类 process/121 变异/26 surface
基线与核心 105 测试/57 ABI 通过。实验 build contract 已换为
process-terminal-result-1。日志 `tmp/result-wire-mutations.log`、
`tmp/result-ipc-gate.log` 与 `tmp/process-result-ipc-verification.log`。
只验证 LIVE 后 stop/read-error，启动/恢复中故障、apply 失败后诊断及生产 UI
仍欠；controller ENDED 相位和在途输入的显式拒绝回复尚未补齐，P0 未通过。

2026-10-20 的结束输入封锁（基于 `e9aac97`）：私有 controller 新增会话级
只读锁存及 ENDED 相位。已知最终序号的 end 立即撤销输入许可，保持当前连接
资格与已接受尾部；恢复尚未完成或最终事件尚未入队时返回 WAIT，输入仍封锁。
最终事件齐全且恢复至 LIVE 后再进入 ENDED，继续排空。重复相同终态幂等，
旧代、不同终态、越过最终序号的快照/事件及迟到握手均不得改写终态或丢尾部。
锁存跨 detach/reattach 保留；保存状态的只读重接由 C fixture 验证，尚非完整
已结束 GUI 的重连证据。此入口只接受已知最终序号，未知不能伪填零。

P0 在 worker join 后、发送末尾数据及结果前封锁 controller；探针专用
PW_PRE_END_INPUT 同步让尚未知结束的 GUI 用真实引擎编码 INFLIGHT，再以合法
代号及已应用序号发送。当前 peer 仍有效，输入被明确回复 PW_INPUT_REJECTED
（16 字节：generation 8、GT_ERR_CANCELLED 4、PW_INPUT_ENDED 4），不提交给
已结束 worker。随后旧尾部及 GWR1 仍正常到达；收到结果后的本地 LATE 门禁
继续验证。同步帧只是实验时序控制，不是生产协议设计。

Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0 下，核心 gate 108/108、
57 公共符号通过；restore-stream 10 基线/25 变异、attachment 14 基线/25 变异
均目标红、独立对照绿、SHA256 回滚复绿。三项新增结束用例经 ASan/UBSan
通过。日志 `tmp/ended-gate.log`、`tmp/ended-restore-mutations.log`、
`tmp/ended-attachment-mutations.log`。恢复途中 end 的延迟完成有核心证据，
原生五类场景及 68 进程/11 帧/14 GUI 预算/8 程序表/3 规模/8 命令/11 终态
编码变异（共 123）、26 类旧 surface 基线、SHA256 回滚全部通过，日志
`tmp/process-ended-verification.log`。原生 GUI 场景仍仅验证 LIVE 后终态；
恢复期故障、启动前未知序号失败、生产
listener/manager/auth/writer、产品 UI 与完整资源账本仍欠，P0 未完成。

2026-10-20 的恢复期终态原生证据（基于 `adb4f60`）：新增 `recovery-error`。
快照 S 已冻结并发出、controller 尚在 SNAPSHOT 时，探针向真实 PTY 注入
待处理命令输入使其推进至 FOURTH，再用既有 read 故障适配器结束 worker。
在处理 READY(S) 前查询 READY 结果并调用 end，必须返回 WAIT、锁存最终序号
与只读状态，保留原快照和增量。不是操作系统自行产生 read 错误的证据。

真实 Metal GUI 导入旧快照后继续 READY(S)/S..L/READY(L)，最终序号已入队，
所以 L 就等于终态序号；不再要求 LIVE 后额外产生数据，仍要求 cursor 精确等于
权威最终序号。握手完成不能重开输入；end 随后完成为 ENDED，相同代号/有效
应用序号的真实引擎编码 INFLIGHT 被明确拒绝，再发送 GWR1。GUI 在 EXPECT
之前核对程序/命令摘要、错误结果和最后网格，最终蓝色像素及本地 LATE 门禁
仍通过。控制器在恢复中结束、界面完成恢复后收到终态；尚非失败时中止恢复、
初始化前未知序号错误或已损坏 apply 状态的诊断方案。

`python3 scripts/probe-session-process.py --case recovery-error` 通过，
S=4、L=8、final=8，日志 `tmp/process-recovery-first.log`。同一 Darwin 25.6.0
arm64 / Xcode 27.1 / Apple clang 21.0.0 环境下，完整六类原生场景、70 进程/
11 帧/14 GUI 预算/8 程序表/3 规模/8 命令/11 终态编码变异（共 125）、
26 类旧 surface 基线及 SHA256 回滚通过；新增省略锁存、篡改最终序号两项
均 recovery-error 目标红、roundtrip 对照绿。核心 gate 108/108、57 公共符号
对拍通过。日志 `tmp/process-recovery-verification.log`、`tmp/recovery-end-gate.log`。
生产 listener/manager/auth/writer、产品 UI 与完整资源账本仍欠，P0 未完成。

2026-10-20 的 apply 失效清理（基于 `4f19fb6`）：worker 的私有配置新增可选
`state_invalidated(state, error, last_sequence)`。仅 sticky owner failure 触发，
backend 关闭后、close_state 之前同线程调用；传精确 owner 错误与最后成功序号，
通知时结果仍 PENDING。普通 backend/read/input 错误和可恢复的 capture OOM
不误标状态损坏，close_state 仍执行一次。回调必须有界、不 join、不导出状态；
最后成功序号不能证明当前引擎尚有效。公开 ABI 与 GWR1 字节格式未改。

P0 适配器接收通知后禁止最后快照导出/元数据读取。新增 `apply-error`：在
FOURTH 已成功发布后，通过故障 read 适配器喂入清屏及 POISON 字节；apply
在实际引擎 feed 成功后故意返回 IO，不发布事件、不推进成功序号。为构造独立
oracle，探针在破坏前保留规范/资源快照；失效后不从引擎重导出，销毁并报告
ERROR/APPLY/IO。GUI 仅排空有效事件，对齐最后成功序号并保留蓝色网格，在
EXPECT 之前得到结果与摘要，LATE 输入仍拒绝。该 oracle 是试验夹具，不是
生产自动回滚或通用 apply 恢复方案，也不证明上游错误必定在同一点发生。

独立 `--case apply-error` 通过；worker-result 17 基线/23 变异、SHA256 回滚
通过，新增遗漏通知、丢失错误、错误推进序号、误报健康状态及销毁后通知五项
均目标红/独立对照绿；apply/capture-null/capture/read 四例 ASan/UBSan 通过。
日志 `tmp/process-invalid-first.log`、`tmp/state-invalidated-mutations.log`。
Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，核心 gate
108/108、57 公共符号对拍通过；完整七类原生场景、73 进程/11 帧/14 GUI
预算/8 程序表/3 规模/8 命令/11 终态编码变异（共 128）、26 类旧 surface
基线及 SHA256 回滚通过。漏接通知、损坏状态仍导出、部分失败谎报成功三项
均 apply-error 目标红、read-error 对照绿。日志 `tmp/state-invalidated-gate.log`、
`tmp/process-invalid-verification.log`。
启动前未知序号、reply flush 失败时权威序号与已发布序号的差异、生产故障诊断/
listener/manager/auth/writer、产品 UI 与完整资源账本仍待完成，P0 未通过。

2026-10-20 的 reply 失败发布修复（基于 `9772e27`）：成功 apply 已推进
权威序号，reply flush 失败不能再提前返回并吞掉该事件。worker 保留真实
REPLY 错误，在 flush 尝试之后仍发布成功事件和 output，然后结束；apply
本身失败时不发布，已发送 reply 不重复重试。输出回调以成功序号是否推进
为准，不能以最终 rc==OK 为准。通知结果中的序号现在仍能对应已发布流，
不把 reply I/O 错误当成引擎失效。没有改变公开 ABI 或 GWR1 格式。

新增真实 Metal GUI `reply-error`：FOURTH 后故障适配器提供 DSR 状态查询，
权威引擎实际产生 CSI 0 n，backend 的这次 reply write 明确返回 IO；原来
成功发送的查询响应不受影响。成功解析的查询仍作为最后事件发布，S=4、
L=5、final=9，controller 可完成 ENDED、排空到最后序号再发 REPLY/IO。
GUI 应用查询时不产生第二份应答，保留最终网格；本地晚输入与在途输入均
继续拒绝。该故障由适配器注入，不声称复现真实内核写失败或已投递前缀。

独立 `--case reply-error` 通过，日志 `tmp/process-reply-first.log`。
worker-result 17 基线/26 变异及 SHA256 回滚通过，新增事件丢失、字节丢失、
重复发布三项均 reply 目标红、独立对照绿；既有 session-worker 10 类基线与
全部定向变异回滚复绿。reply/apply/read/capture-null 四例 ASan/UBSan 通过。
日志 `tmp/reply-publication-mutations.log`、`tmp/reply-worker-mutations.log`。
Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，核心 gate
108/108、57 公共符号对拍通过；session-worker 共 17 项变异回滚复绿。
完整八类原生场景、75 进程/11 帧/14 GUI 预算/8 程序表/3 规模/8 命令/
11 终态编码变异（共 130）、26 类旧 surface 基线与 SHA256 回滚通过；
遗漏最后查询、丢弃失败 reply 对应事件两项均 reply-error 目标红、read-error
对照绿。日志 `tmp/reply-publication-gate.log`、`tmp/process-reply-verification.log`。
启动前未知序号错误、生产诊断/监听器/manager/auth/writer、产品 UI 及完整
资源账本仍待完成，P0 未通过。

2026-10-20 的启动失败未知序号控制器契约（基于 `172f237`）：新增私有
`gt__attachment_end_unknown`，调用方仅能在 worker 结果 `has_sequence=false`
时使用，不能从数值零推断未知。只允许 capture/submission 之前，锁存会话
只读与 `has_terminal_sequence=false`，保留当前 peer 资格用于错误投递。
没有虚构数据流或 S/L：内部 phase 保持 CAPTURING，拒绝 begin/push 和后续
已知 end（包括零）；未知锁存跨 detach/reattach 保留，重复调用幂等。已有
已知零不能降为未知，已开始快照也不能改走该入口。已知结束接口现在明确
置 `has_terminal_sequence=true`，查询 POD 同时返回该判别位。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，核心 gate
109/109、57 公共符号对拍通过；attachment 15 类基线/33 项变异、SHA256
回滚复绿，其中八项新增变异均 unknown-end 目标红、nojob 对照绿。
unknown-end/ended/end-wait 经 ASan/UBSan 通过，原生 recovery-error 重新编译
运行通过（S=4、L=8、final=8）。日志 `tmp/unknown-end-gate.log`、
`tmp/unknown-end-mutations.log`、`tmp/unknown-end-native.log`。
新增 unknown-end 核心用例验证上述边界、旧代拒绝和重接；这只是控制器契约，
尚未实现无 surface 的启动错误 GUI/IPC 分支，也没有从 CAPTURING 假报 LIVE。
启动错误详情仍由 worker result 保存，上层须显式发送错误并结束恢复等待。
生产监听器/manager/auth/writer、产品 UI 与完整资源账本仍欠，P0 未通过。

2026-10-20 的无快照启动失败原生分支（基于 `29d7c40`）：新增
`start-error`，真实 worker 的 open_state 返回 IO，未建立 state/backend，
结果为 ERROR/STATE_OPEN/IO、has_sequence=false。join 后调用未知结束入口，
保持 peer 资格，在继承 socketpair 上直接发送 GWR1/PW_RESULT；帧序号零只是
无数据流的占位值，payload 判别位仍未知，未发送 SNAPSHOT/READY/LIVE。

GUI 的 I/O 线程在首帧分支验证终态后确认，不等待快照或构造 terminal surface。
主线程显示启动失败窗口及未知最终序号文本，断言窗口属性、surface/budget
为空且终端预算池无 reservation，随后退出并让 peer 回收本代。窗口属性是
UI 接线证据，不是像素验收。此场景的 bootstrap worker/controller 使用默认
分配器，尚未纳入完整资源账本；没有生产 listener、发现、身份认证或产品 UI。
独立 `--case start-error` 原生编译运行通过，日志 `tmp/process-start-first.log`。

脚本新增 `--startup-mutations` 定向运行模式，以 start-error 为目标、普通
roundtrip 为对照；也将启动场景与变异纳入完整 --mutations 的后续运行范围。
Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`python3 scripts/probe-session-process.py --startup-mutations` 的三项变异均
start-error 目标红、roundtrip 对照绿，SHA256 回滚及两类原生基线复绿；
日志 `tmp/process-start-mutations.log`。核心 gate 109/109、57 公共符号对拍
通过，日志 `tmp/start-error-gate.log`。现有 75 个进程变异锚点逐项唯一；
本切片未重跑完整进程变异集，不将脚本新增的完整运行范围算作通过证据。
完整 daemon 与 P0 仍未完成。

2026-10-20 的启动失败资源预算（基于 `b7811eb`）：start-error 的 executor、
controller/restore arena、worker 和 peer 改用同一 source 分配域；各对象创建后
必须观察到对应 live charge。总准入 4 MiB，其中 source 1 MiB，worker 与
executor 各预留 1 MiB 加两页容量（guard 及对齐余量）。实际失败 worker 的栈参数在
open_state 线程内验证；executor 在本场景没有任务，保留配置额度至 pool join，
不将此表述为本场景实测了其线程栈区域。

worker join 后 source 必须回到 controller/executor 基线，再释放其栈额度；
peer 关闭后回到同一基线，controller/pool 释放后 source 与栈 scope 全部关闭，
最终 live/committed 归零且映射回收对称，输出 bootstrap peak 指标。无需
backend、waiter 或 VT 额度。本次仅覆盖该失败场景中显式纳入的对象与线程容量，
不包含主线程栈、pthread/libc 内部开销、GUI/AppKit/renderer 或进程 RSS 总量；
生产全局准入、完整资源账本与 daemon 仍未完成。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，定向
`--startup-mutations` 十项均 start-error 目标红、roundtrip 对照绿，SHA256
回滚与两类原生基线复绿；七项新变异覆盖四类分配域绕过、worker 栈未预留、
peer 不释放及栈额度泄漏。实测 bootstrap peak=147456、committed peak=3211264
字节，最终 live=0、committed=0。核心 gate 109/109、57 公共符号对拍通过。
日志 `tmp/process-start-budget.log`、`tmp/start-budget-gate.log`。现有 75 个
进程变异锚点保持唯一，本次未重跑完整进程变异集。

2026-10-20 的 GUI I/O 线程容量（基于 `00b72a3`）：共享 GUI pool 增加独立
opaque runtime reservation，与 surface 的 general/pages/wire 额度竞争同一
总预算。reserve 拒绝时清空输出并关闭失败 scope，unreserve 归还容量；租约
存在时 pool close 返回 BUSY。计数随 pool 快照返回，API 使用方必须先停止并
join 借用线程再释放；租约不是 OS 分配器，不自动探测线程是否还活着。

独立 runtime 用例在 12 页总量下先预留 7 页，证明 6 页 surface 拒绝；归还后
surface 成功，再证明反向 runtime 准入拒绝；失败不影响原租约，pool 保持可
回收。P0 GUI 客户端为 process_consume 预留 1 MiB 加两页容量，线程明确设置
1 MiB stack/一页 guard，并在线程内核验实际栈尺寸及租约。总策略变为原有
320 MiB surface 容量加该 I/O 额度，两 surface 的准入观测包含此额度。普通
恢复与无快照启动失败都经 join 后归还；SIGKILL 仍由操作系统回收进程资源，
本切片不声称走过正常 unreserve。

新增 `--runtime-mutations`：五项 C 预算变异以 runtime 为目标、valid 为对照；
默认线程栈回退以 start-error 为目标、旧 surface 原型为对照。Darwin 25.6.0
arm64 / Xcode 27.1 / Apple clang 21.0.0 下，六项均目标红、独立对照绿，
SHA256 回滚、九类 GUI 预算基线及 start-error/roundtrip 原生复验通过，日志
`tmp/gui-runtime-verification.log`。runtime/valid/rollback/race 四例 ASan/UBSan
通过；核心 gate 109/109、57 公共符号对拍通过，日志 `tmp/gui-runtime-gate.log`。
现有 75 个进程变异锚点保持唯一，本次未重跑完整进程变异集。

pool/租约 descriptor 的 libc 元数据、主线程、AppKit/renderer 和其他线程/系统
内部开销仍不在此容量额度内；尚非完整资源账本或生产 GUI/daemon 接线，P0
未通过。

2026-10-20 的整合复验（基于 `155653c`）：此前启动失败、预算和 GUI runtime
切片的定向证据现已在同一版本合跑。`python3 scripts/probe-session-process.py
--mutations` 完成九类原生场景（roundtrip/kill/crossing/history/read-error/
recovery-error/apply-error/reply-error/start-error）、86 进程/11 帧/19 GUI 预算/
8 程序表/3 规模/8 命令/11 终态编码变异，共 146 项；所有目标红、独立对照绿，
SHA256 回滚、九类 GUI 预算基线和 26 类旧 surface 基线复绿。进程退出码为 0。
日志 `tmp/process-integrated-verification.log`。`bash scripts/gate.sh` 109/109、
57 公共符号对拍及 GATE PASS，日志 `tmp/process-integrated-gate.log`。

实际环境复核为 Darwin 25.6.0 arm64，macOS 26.6.2（25G83），Xcode 27.1
（27A9275）、Apple clang 21.0.0；脚本显式选择
`/Applications/Xcode.app/Contents/Developer`，不依赖全局 xcode-select 的 CLT
选择。上游 pin 仍为 `81598385d608dda4946093d6d6a234d204a2f225`。
本轮无实现改动，未新增平台支持或性能承诺；生产 listener/manager/auth/writer、
产品 UI、完整资源账本和 P0 逐项验收仍未完成。

2026-10-20 的跨进程中文组合/提交（基于 `df7819c`）：新增 `ime` 场景，
独立 Metal GUI 在恢复到 LIVE 后调用 preedit 与 composing key，观测引擎
ime_point 宽度为正且 input buffer 仍空；清除后宽度归零，再以非 composing
key 提交“中文”。此时输入精确为六字节原始 UTF-8，没有 bracketed-paste
包裹。随后同一 surface 的 USER 粘贴带原有包裹，合计 22 字节经带 generation/
applied-sequence 的 INPUT 帧送到真实 worker/PTY。shell 用独立的六字节读取及
字节相等检查确认中文，再验证后续粘贴，程序继续输出并完成最终网格对拍、
终态与晚输入拒绝。普通 roundtrip 不走中文分支，继续验证既有输入路径。

这是引擎 IME API 接到跨进程数据面的证据，不是操作系统候选窗、键盘布局或
NSTextInputClient 的端到端验证；没有以一次 surface_text 伪造 IME 提交。
系统输入法完整路径、其他交互逐项证据与 P0 资源/生产缺口仍待收口。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`python3 scripts/probe-session-process.py --ime-mutations` 四项均 ime 目标红、
roundtrip 对照绿：遗漏 preedit、组合提前提交、遗漏清除、提交误走粘贴。
SHA256 回滚及两类原生基线复绿，本轮 ime 的 S=4、L=5、final=9；
日志 `tmp/process-ime-verification.log`。核心 gate 109/109、57 公共符号对拍
通过，日志 `tmp/process-ime-gate.log`。现有 75 个进程变异锚点保持唯一；
本次未重跑加入 IME 后的完整进程变异集。

2026-10-20 的 GUI 元数据分配域（基于 `9136041`）：pool、surface 租约和
runtime 租约 descriptor 统一经可选 `p0_gui_metadata_allocator` 分配/按精确原始
尺寸释放，构造时清零；NULL 域明确选用原有 libc 行为。分配失败清空输出、
保留既有对象，容量准入失败也归还已分配 descriptor。新 metadata 用例逐个
拒绝 pool/runtime/surface 分配，验证 NO_MEMORY、BUSY、完整释放以及禁用
容量下的失败清理；域上下文必须活到 pool close 后，共享时回调须支持并发。

原生客户端接入独立 mmap 元数据域：包含对齐头及页舍入，每次映射前检查
4 MiB 上限，释放核对原始长度；pool close 后要求映射归零并销毁锁。没有把
VT 聚合头引入 full libghostty GUI，保持元数据接口为纯 C POD。这个 4 MiB
是独立于 surface/runtime 预算的额外上限，并非生产全局统一 ledger；主线程
中的分配域控制字段、其他框架/系统开销及其余资源仍待收口。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--metadata-mutations` 五项均 metadata 目标红、valid 对照绿，覆盖 pool/租约
绕过分配域、错误释放尺寸、失败准入泄漏与错误 pool 释放尺寸；SHA256 回滚、
十类 GUI 预算基线及 start-error/roundtrip 原生复验通过。实测两条路径元数据
映射峰值分别 32768/81920 字节，最终 live=0。日志 `tmp/gui-metadata-verification.log`。
metadata/runtime/rollback/race 四例 ASan/UBSan 通过。核心 gate 109/109、57
公共符号对拍通过，日志 `tmp/gui-metadata-gate.log`。现有 75 个进程、14 个旧
GUI 及六个 runtime 变异锚点逐项唯一；本次未重跑完整进程变异集。
P0 与生产 daemon 未完成。

2026-10-20 的 GUI 元数据统一容量准入（基于 `8db4849`）：私有 limits 新增
metadata。非零时要求显式提供能约束实际开销的元数据域，并在 pool 构造阶段
从同一 total 预留全部 metadata 上限；否则拒绝，不能悄悄回落到无界 libc。
域本身继续核对映射实际字节，pool 则按完整额度保守承诺，不重复把这些实际
字节再加到 committed。metadata=0 明确不包含元数据，保留旧探针对照语义。
构造额度不足返回 LIMIT、清空输出并释放 descriptor；pool 关闭时归还元数据
承诺再销毁预算，最后宿主释放 pool 的映射。

combined 用例以 12 页总量预留 7 页 metadata，验证 6 页 surface 和 6 页 runtime
都因剩余 5 页而拒绝；runtime 5 页可成功并与 metadata 合计 12 页，归还后
回到 7 页。metadata 大于 total 的构造失败可完整清理。原生策略 total 明确为
320 MiB surface + I/O 栈容量 + 4 MiB metadata；启动失败和正常恢复在 pool
关闭前保留 4 MiB promise，关闭后实际 metadata 映射归零。这是同一 GUI pool
的容量准入，尚非跨进程/生产 manager 全局账本；live 的引擎统计与元数据实际
映射统计仍分项记录。其他框架、线程/系统开销和 P0 余项仍未完成。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--combined-mutations` 四项均 combined 目标红、valid 对照绿，覆盖遗漏 metadata
预留、误接受无界 libc、额度不释放及额度拒绝谎报成功；SHA256 回滚、十一类
GUI 预算基线与 start-error/roundtrip 原生复验通过，日志
`tmp/gui-combined-verification.log`。combined/metadata/runtime/rollback 四例
ASan/UBSan 通过；核心 gate 109/109、57 公共符号对拍通过，日志
`tmp/gui-combined-gate.log`。现有 75 个进程、14 个旧 GUI、六个 runtime、
五个 metadata 变异锚点保持唯一。本次未重跑完整进程变异集。

2026-10-20 的混合并发准入（基于 `5b1b118`）：新增 combined-race，在同一
12 页 pool 中先承诺 6 页 metadata；两个真实 pthread 同时请求 6 页 surface
和 6 页 runtime。32 轮交替创建顺序，每轮必须恰有一个成功、一个 LIMIT，
输出句柄与结果一致，committed=12 页、对象计数匹配，pool 仍 BUSY；释放
赢家后精确回到 metadata 的 6 页及一个 pool descriptor。无数据分配的容量
竞争不能谎报两份成功，也不能因为部分申请/回滚造成两方都失败。

每轮随后强制 surface 占满再拒绝 runtime，反向再做一次，检查失败输出清空、
已有容量保持、失败 descriptor 回收及最终 pool 释放。不依赖随机竞争赢家来
覆盖两条拒绝清理路径。元数据 fixture 的调用在该单 pool 的 admission 锁内
串行化，主线程只在 pthread join 后观测计数；这不证明共享域跨 pool 的并发
回调安全，也不替代生产全局 manager 的并发准入。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--combined-mutations` 九项均目标红、独立对照绿；五项新增变异覆盖遗漏
metadata 承诺、runtime/surface 不预留以及两条拒绝路径泄漏 descriptor。
surface 不预留使用 invalid 对照，其余使用 valid 对照。SHA256 回滚、十二类
GUI 预算基线及 start-error/roundtrip 原生复验通过；日志
`tmp/gui-combined-race.log`。combined-race/combined/race 三例 ASan/UBSan 通过，
核心 gate 109/109、57 公共符号对拍通过，日志 `tmp/gui-combined-race-gate.log`。
本次未重跑完整进程变异集。P0 与生产 daemon 仍未完成。

2026-10-20 的真实 VT 启动配额失败（基于 `0f18605`）：新增 state-oom。
open_state 在程序表建立后即把部分状态交给 worker 清理；VT 创建/初始 resize
失败返回 IO，不能再 CHECK 退出整个权威进程。尚未完成配置的状态由 close_state
只销毁 terminal/程序表，不读取最终元数据或导出快照。只有配置完成后才置
state_ready，普通 LIVE 终态仍保留原有最终快照与摘要路径。

场景在控制器、executor 和 worker 已分配后收紧 source 至当前 live+64 KiB，
实际调用 VT 构造，必须观察到 engine_attempted、一次 partial_cleanup、
SCOPE_LIMIT/denied、无残留 terminal/程序表，以及 join 后回到原控制器基线。
不是用工厂直接返回 IO 代替真实引擎分配拒绝。worker 结果仍为未知最终序号
ERROR/STATE_OPEN/IO，不伪造 S=0 或进入恢复握手。

source 故障是 sticky，不能从该域再分配错误投递 peer。场景在失败之前从
4 MiB 总量中额外预留 64 KiB 健康诊断域，仅该 peer 使用；释放走同一域。
GUI 收到通用 STATE_OPEN/IO 并确认，没有创建 terminal surface；此处没有新增
wire 配额原因字段或把 IO 当成专用 NO_MEMORY。诊断域和 source/线程额度在
关闭后全部归零。普通 start-error 和正常 roundtrip 保留原分配路径。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--state-oom-mutations` 五项均 state-oom 目标红、普通 start-error 对照绿，
覆盖错误释放域、部分程序表泄漏、从故障 source 分配诊断 peer、丢弃部分清理
上下文及用直接 IO 冒充真实引擎失败。SHA256 回滚和 state-oom/start-error/
roundtrip 原生复验通过，日志 `tmp/process-state-oom-verification.log`。
本轮实际 peak=212992、committed peak=3276800 字节，最终 live/committed=0。
核心 gate 109/109、57 公共符号对拍通过，日志 `tmp/state-oom-gate.log`。
现有 75 个进程变异锚点保持唯一；本次未重跑完整进程变异集。仍未覆盖所有
setter/backend 启动失败原因或生产低内存诊断，P0 与生产 daemon 未完成。

2026-10-20 的第二轮整合复验（基于 `188d859`）：
`python3 scripts/probe-session-process.py --mutations` 在同一版本完成十一类
原生场景（原九类加 ime/state-oom），95 进程/11 帧/33 GUI 预算/8 程序表/
3 规模/8 命令/11 终态编码变异，共 169 项；所有目标红、独立对照绿，
SHA256 回滚、十二类 GUI 预算基线和 26 类旧 surface 基线复绿，退出码 0。
包括真实 VT 配额拒绝后的部分状态清理与健康诊断 peer、跨进程中文提交、
元数据/surface/runtime 统一容量及混合并发拒绝清理，均已从单独定向证据
进入本轮完整回归。日志 `tmp/process-latest-integrated.log`。
`bash scripts/gate.sh` 109/109、57 公共符号对拍及 GATE PASS，日志
`tmp/latest-integrated-gate.log`。

环境仍为 Darwin 25.6.0 arm64 / Xcode 27.1（27A9275）/ Apple clang 21.0.0，
上游 pin `81598385d608dda4946093d6d6a234d204a2f225`；脚本显式使用 Xcode
Developer 目录。本轮未修改实现，未新增平台或性能声明。系统输入法完整路径、
其他交互逐项证据、所有启动失败原因、完整资源账本及生产 listener/manager/
auth/writer/产品 UI 仍未完成，不能由完整脚本绿色推导 P0 已通过。

2026-10-20 的 backend 启动配额失败与已知零（基于 `9d61bca`）：新增
backend-oom。启动 state 先完成真实 VT 初始化，随后在 backend factory 内把
source 上限收紧到当前 live，实际调用 PTY 构造；首个 backend 分配被拒绝，
必须有 denied、无 backend 对象/字节。按当前 PTY 实现，拒绝发生在 pipe/fork
之前，不需要 waiter 栈。worker 结果为 ERROR/BACKEND_OPEN/IO，
has_sequence=true、sequence=0；与 state-oom 的未知序号明确区分。

清理按 state_ready/backend_started 的生命周期判别：backend 从未成功启动
时，只销毁已建立的 VT/程序表，不要求程序已经输出最终摘要，不导出不适用
的最终快照。源额度故障后仍由独立健康诊断域建立 peer。控制器 end(0) 返回
WAIT 并保留 known=true/terminal_sequence=0、只读和连接资格；尚无快照或
READY/LIVE，不能为了发送错误伪造恢复完成。GWR1 的已知位随终态发送。

GUI 在无 surface 的启动失败分支按 BACKEND_OPEN 检查已知零，并显示该阶段
与“known: 0”；STATE_OPEN 保持 unknown。两条路径都不创建 terminal surface，
IO 线程 join 和 pool/元数据释放仍按既有顺序完成。backend 场景 bootstrap
总额度 8 MiB，source 4 MiB、两线程栈和 64 KiB 诊断承诺均显式纳入；不是
增加生产全局准入或完整进程 RSS 保证。生产 daemon 和 P0 余项仍未完成。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--backend-oom-mutations` 四项均 backend-oom 目标红、state-oom 对照绿，
覆盖已知零误走未知入口、遗漏已初始化状态清理、PTY 拒绝被工厂直接 NULL
冒充和 wire 已知位丢失。SHA256 回滚及 backend-oom/state-oom/roundtrip 原生
复验通过，日志 `tmp/process-backend-oom-verification.log`。本轮实际 peak=
1966080、committed peak=6422528 字节，最终 live/committed=0。核心 gate
109/109、57 公共符号对拍通过，日志 `tmp/backend-oom-gate.log`。
现有 75 个进程与五个 state-oom 变异锚点保持唯一；本次未重跑完整进程变异集。
仍不覆盖所有配置 setter、PTY 创建失败点或生产启动错误 UI，P0 未通过。

2026-10-20 的跨进程鼠标/焦点报告（基于 `13461d8`）：新增 pointer。
真实 PTY 在首个快照前开启 DEC 1000/1006/1004，GUI 仅从恢复状态取得这些
模式；LIVE 后调用鼠标/焦点引擎 API，逐步验证 SGR 左键按下/释放各 9 字节、
CSI O/I 各 3 字节，重复设置 false 不新增输入，空移鼠标也不产生报告。
24 字节报告再接原有 16 字节 USER 粘贴，整体携带同代/同一已应用序号，经
INPUT 到 worker 和真实 PTY。shell 独立读取并逐字节比较报告，再验证粘贴，
继续产生输出；最终网格、摘要、单一查询应答及结束后输入拒绝仍通过。

这是恢复模式与跨进程编码数据面的证据；API 事件在主线程产生，字节在 I/O
线程和 worker 之间传送，不进入 UI 事件队列。尚非真实 AppKit 点击/窗口焦点
切换的端到端宿主路由验证；不以此宣布原生事件入口或 P0 完成。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--pointer-mutations` 四项均 pointer 目标红、roundtrip 对照绿：遗漏按下、
释放误作按下、遗漏聚焦和重复失焦误作状态翻转。SHA256 回滚及两类原生
基线复绿；本轮回滚 pointer 观测 S=5、L=6、final=10（依实际 PTY 分块推进，
不是固定序号期望）。日志 `tmp/process-pointer-verification.log`。
核心 gate 109/109、57 公共符号对拍通过，日志 `tmp/process-pointer-gate.log`。
现有 75 个进程变异锚点保持唯一；本次未重跑完整进程变异集。

2026-10-20 的有序配色命令基础（基于 `50a663f`）：私有 worker 新增
COLOR_SCHEME 命令、dark=0/1 的固定 POD 和 GT_STATE_COLOR_SCHEME 事件（kind=4，
不与实验输出摘要 bundle 的 kind=3 混用）。支持能力由 config 显式声明，默认
false；未声明时 admission 返回 GT_WORKER_UNSUPPORTED，不分配、不排队、
不修改状态；这是私有队列结果域，未来公开入口仍须映射为 GT_ERR_UNSUPPORTED。
支持时复制 POD，经原有有界 FIFO 在 state owner 上 apply，成功后发布同一
权威序号，后排 capture 必须看到该配置；不写为用户输入、不触发 backend resize。
适配器 apply 失败保留原错误、停止 worker 并触发状态失效通知，不伪报成功。

四类核心用例使用真实 PTY 和配置状态模型，验证 FIFO 中“配置→捕获”、调用方
复用 POD 后值不变、unsupported/invalid 不产生副作用、失败结果保留。模型不是
VT/GUI 配色接线；现有适配器仍默认未支持。实际引擎 scheme callback、快照中
配置元数据、全 surface 的有序配置 barrier、查询应答与渲染观察仍须完成，
不能用这些模型测试声称配色同步或 P0 已通过。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，核心 gate
113/113、57 公共符号对拍通过；四基线/八项配置变异均目标红、独立对照绿、
SHA256 回滚复绿，两套旧 worker 变异（17 与 26 项）也全部回滚复绿。
flow/unsupported/failure/invalid 经 ASan/UBSan 通过；现有原生 roundtrip 重新
编译运行通过。日志 `tmp/worker-scheme-gate.log`、`tmp/worker-scheme-mutations.log`、
`tmp/worker-scheme-old-worker.log`、`tmp/worker-scheme-old-result.log`、
`tmp/worker-scheme-native.log`。公开 ABI 和 GWR1 未改，未重跑完整进程变异集。
P0 与生产 daemon 仍未完成。

2026-10-20 的真实 headless VT 配色适配（基于 `8ceecba`）：新增独立
`daemon/probes/session-scheme-owner.c`，只链接 VT/core，不链接 AppKit/Metal。
`otool -L` 实测该二进制及 VT dylib 的依赖仅 VT/libSystem，没有窗口/渲染框架。
在该适配器声明配色能力，state owner 应用配置后，VT 的 COLOR_SCHEME 查询
回调读取同一 owner 的配置。真实 PTY 依次发出三次 CSI ?996n，并独立比对
light/dark/light 的 CSI ?997;2n / ;1n / ;2n；每次检查多余字节为空，查询和
应答计数均为三。主线程只提交配置/capture/input；回调和 apply 在 worker 上，
有序配置后的 capture 记录外部配置值与同一序号。阶段使用唯一序数 marker，
不能拿首次 light 的输出冒充第三次应答。

capture 此处仅是独立的外部配色 POD，不是完整 VT 规范快照或 GUI 恢复载荷。
当前主进程原型的 VT/GUI 适配器仍未启用配色能力；需把配置元数据并入一致
checkpoint，接通 full surface 有序配置 barrier，再验证并发查询/渲染。此
独立适配器的成功不授权 GUI 绕过 worker，也不表示配色跨进程恢复已完成。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--scheme-owner-mutations` 四项均目标明确退出 1、roundtrip 对照退出 0，覆盖
查询忽略新配置、未声明能力、未接真实回调、捕获遗漏外部配置；SHA256 回滚、
真实 VT 基线和原生 roundtrip 复绿，日志 `tmp/scheme-owner-verification.log`。
C 适配器/worker/PTY 的 ASan/UBSan 链接探针通过，VT dylib 保持原构建。对象与
VT/PTY 通过 source 4 MiB 域，worker/waiter 栈按显式 options 预留，总额度
8 MiB；实际 peak=1933312 字节，最终 live/committed=0。本轮仅实际核验 worker
栈尺寸，不把 waiter options 等同于新做了线程区域测量。
核心 gate 113/113、57 公共符号对拍通过，日志 `tmp/scheme-owner-gate.log`。
未重跑完整进程变异集；完整资源账本、生产 daemon 和 P0 余项仍待完成。

2026-10-20 的完整进程集成复验（基于 `ea21f82`）：在 Darwin 25.6.0 arm64、
Xcode 27.1 / Apple clang 21.0.0、Ghostty pin
`81598385d608dda4946093d6d6a234d204a2f225` 下，实际运行
`python3 scripts/probe-session-process.py --mutations`，退出 0。
181 项变异为 103 process + 11 wire + 33 GUI-budget + 8 summary-wire +
3 summary-queue + 8 command-wire + 11 result-wire + 4 scheme-owner；目标红、
对照绿及 SHA256 回滚通过。回滚后 roundtrip、kill、crossing、history、
read-error、recovery-error、apply-error、reply-error、start-error、ime、
state-oom、backend-oom、pointer 十三类原生场景与 26 类旧 surface 基线通过。
完整日志 `tmp/process-integrated-181.log`；临时日志不入库。

同轮 `bash scripts/gate.sh` 退出 0，113/113 测试与 57 个公共符号对拍通过，
日志 `tmp/process-integrated-181-gate.log`。未新增断言、未改公开 ABI。
本轮整合了最近定向验证的切片，不扩大其证据范围：独立 headless VT 配色
适配器尚未接入主进程快照/增量或 GUI；GUI 实际应用配置后的序号 barrier、
系统输入法与原生鼠标/焦点宿主路由、完整资源账本及生产 daemon 仍待完成。
P0 保持未通过。

2026-10-20 的 full surface 配置应用屏障（基于 `05d11bb`）：实验补丁
0030 将配色事件分为 begin（仅准入）和主线程 apply（仅提交带序号配置消息）。
待应用期间保持旧 applied sequence，后续有序 output/resize 拒绝且不消费序号，
新编码输入通过 stamp.blocked 丢弃、不排队重放；焦点 I/O 编码路径及宿主动作
也遵守阻塞。已在此前编码的输入保留原 stamp。Termio 在持锁校验精确配置消息、
主题和预算后，实际更新终端默认颜色等配置，最后才发布该事件序号。

绑定副本的普通无序配置重载会使流失效，不能在事件流外修改权威状态；错误
标签或派生失败同样不允许伪造成功。没有取消待应用事件的接口，调用方超时须
作废连接。上游在配置没有主题条件时会跳过 conditional state 更新，实验标签
消息因此显式携带权威主题。未修改 vendor 或公开 core ABI，补丁及理由随代码
存于 `patches/libghostty/8159838/experimental/`。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，实际运行
`python3 scripts/probe-session-worker-surface.py --scheme-barrier`，退出 0。
真实 full Metal surface 验证 dark→light→dark→dark（同主题也推进事件）、I/O
配置主题、终端实际背景色 0x123456、待应用期间的序号/输入/输出/resize 隔离、
错误代次/序号拒绝、配置后查询仍不从副本出站，以及无序重载使流失效。
七项隔离变异覆盖提前发布、遗漏发布、后续事件越过屏障、输入 stamp 丢标记、
配置消息丢序号、无序重载被接受及实际背景色未更新；均明确目标退出 1、普通
continuation 对照退出 0，SHA256 回滚与最终两条原生基线复绿。
日志 `tmp/scheme-barrier-verification.log`。

同一最终引擎构建经进程探针的 copy_sources/build/probe/legacy 入口，重跑
十三类原生进程场景与 26 类旧 surface 基线通过，仓库源文件哈希未变；日志
`tmp/scheme-barrier-compat.log`，临时驱动 `tmp/verify-scheme-barrier-compat.py`。
核心 gate 113/113、57 个公共符号对拍通过，日志 `tmp/scheme-barrier-final-gate.log`。
本轮未重跑 181 项完整进程变异集，不能把独立七项变异视为其新增已集成覆盖。

确认只覆盖 I/O 配置及终端状态实际应用，不承诺 renderer 已完成绘制；该探针
也不证明系统事件入口、任意字体/配置的权威事件模型或完整资源账本。主进程
IPC 配色能力仍禁用：一致快照中的外部配置、增量载荷与 GUI 主线程调度尚待
接线。生产 daemon 与 P0 其余要求仍未完成，P0 保持未通过。

2026-10-20 的跨进程配色快照与增量接线（基于 `541c53b`）：新增 scheme
原生场景。私有 worker 适配器仅在该场景声明 COLOR_SCHEME 能力，owner 应用
主题与固定实验配色档的默认颜色；实际 VT COLOR_SCHEME 回调仍是唯一查询应答
owner。capture 在同一 worker 边界复制 dark 值并冻结完整 VT，编码与传输使用
捕获值，不读取后来变化的 live 配置。实验 snapshot/EXPECT 元数据在该档扩为
224 字节，新增 PW_SCHEME 为 8 字节 0/1 的有序增量；旧档仍严格接受 216 字节。
这不是生产 IPC v1 或能力协商，双方仍通过构建指纹核验当前实验契约。

0031 实验补丁在 bound checkpoint 构造参数中携带可选主题，并把域标记及主题
值加入 generation/S/资源/规范快照的同一摘要。只翻转主题而沿用摘要必须拒绝；
未绑定元数据和缺少显式设置标记的 dark 值也拒绝。surface 从该主题初始化条件
状态与 I/O 配置，不能继承新 GUI 进程的全局外观。显式主题的条件派生失败不走
上游日志后回落。未修改 vendor、公开 core ABI 或生产平台绑定。

GUI 的 I/O 线程直接接收并应用输出；配色增量先通过 0030 begin 保留旧序号，再
用固定大小的低频请求通知主线程派生配置。I/O 等待真正的 applied cursor 后才
消费下一增量，并检查实际 I/O 主题与终端默认背景；终端字节不进入 UI 队列。
实验配色档为 dark 背景 0x123456 / light 背景 0xabcdef、白色前景、非闪烁光标
默认值与 pin 的默认 palette。光标策略显式与源 VT 一致；首次整表对拍发现普通
配置重载会带入 GUI 的不同光标默认值，未用掩码或放宽比较规避差异。

真实 PTY 先验证 light 查询；捕获 dark 快照后，light 增量进入 S→L，READY(S)
冻结边界后再发 dark 增量，LIVE 后再发 light。GUI 构造前强制全局 light，验证
恢复的 dark 不受它污染；三条增量按 light/dark/light 次序实际应用。PTY 在用户
输入后再次逐字核验 dark 查询，并确认没有多余应答。结束时原捕获值保持 dark、
owner 已为 light；GUI 的完整 canonical/resources 与最终权威状态逐字一致。
保留原有终态、输入拒绝、程序摘要、命令状态与像素断言。应用确认仍不是绘制
完成确认，也未新增默认背景色的像素验收。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`python3 scripts/probe-session-process.py --scheme-mutations` 退出 0：十一项
C/宿主/codec 及两项隔离引擎变异均 scheme 目标明确退出 1、roundtrip 对照退出
0；覆盖捕获缺失、误传 live 值、构造标记、非法元数据/增量、错误 kind、查询
忽略配置、权威颜色漏应用、GUI 颜色错误、主线程不接请求、codec 拒绝新 kind、
构造 I/O 主题落回全局和跳过主题摘要校验。SHA256 回滚与两类原生基线复绿，
日志 `tmp/process-scheme-verification.log`。

同一最终构建另跑十四类原生进程场景、26 类旧 surface 基线通过；旧的“权威
重复光标应答”变异限定到原光标分支后，roundtrip 红、legacy continuation 绿，
SHA256 回滚与复验通过。75 个旧进程变异锚点逐项唯一，仓库源文件哈希未变。
日志 `tmp/process-scheme-compat.log`，临时驱动
`tmp/verify-process-scheme-compat.py`。核心 gate 113/113、57 公共符号通过，
日志 `tmp/process-scheme-gate.log`。完整进程脚本已纳入新场景和十三项变异，配置
总数变为 194；本轮未重跑完整集合，不把声明的计数当作已完成的集成证据。

生产设置 UI→daemon 命令、任意主题/字体/配置的权威模型、原生事件入口、绘制
完成与性能验收、完整资源账本及 listener/manager/auth/writer/打包仍待完成。
当前命令由原型驱动提交；固定实验配色档的成功不代表产品配色接线或生产
session runtime 已交付，P0 保持未通过。

2026-10-20 的跨进程配色背景像素验收（基于 `a39db2a`）：在 scheme
原生场景增加六次真实 Metal 视图采集，分别为初始 dark 快照、S→L 的 light、
L 后的 dark、可输入 LIVE 的 dark、LIVE 后的 light 以及 light 终态。主线程
在相应配置实际应用后观察帧，后续配置仍须由同一主线程接收；像素观察没有
改变协议 I/O applied 的含义，也没有把它变成绘制完成确认。

从自己的 NSView 采集并转为 RGBA，不依赖屏幕录制权限或既有截图文件。每帧
要求期望背景 RGB（dark=0x123456、light=0xabcdef，各通道容差 5）覆盖超过
半幅画面，另一种背景颜色少于 64 像素；后者单独拦截局部残留，不能只凭主体
颜色正确放行。六次采集计数与当前阶段绑定，最终必须实际执行六次。
本轮 1600×1200 帧中，前四帧匹配背景均为 1908988 像素，后两帧为
1899513，另一背景颜色均为零。临时 PNG 位于
`tmp/worker-surface/process-scheme-{0..5}-*.png`，不入库。

`python3 scripts/probe-session-process.py --scheme-pixel-mutations` 在
Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下退出 0。四项隔离
变异均 scheme 明确退出 1、roundtrip 对照退出 0：采集丢失内容、复用初始
bitmap、读取缓冲混入 256 个旧色像素，以及只让实际 renderer 的 RenderState
在 light 时仍使用 dark 背景。最后一项不改权威或 GUI I/O 状态，证明仅对拍
canonical 和配置值看不见的错误也会红。混色项仍满足主体新色比例，但旧色
计数 256，单独触发残留断言。SHA256 回滚与 scheme/roundtrip 原生复验通过，
日志 `tmp/process-scheme-pixel-verification.log`。

同一最终构建的十四类原生进程场景与 26 类旧 surface 基线通过，75 个旧进程
变异锚点逐项唯一、仓库源文件哈希未变；日志
`tmp/process-scheme-pixel-compat.log`，临时驱动
`tmp/verify-scheme-pixel-compat.py`。核心 gate 113/113、57 个公共符号对拍
通过，日志 `tmp/process-scheme-pixel-gate.log`。完整进程变异脚本已纳入四项
像素变异，配置总数为 198，本轮未重跑完整集合。

证据只覆盖固定实验配色档的默认背景及已有前景/SGR 图像；不覆盖任意主题、
字形/抗锯齿或通用 renderer 绘制完成屏障。截图诊断的 bitmap 分配不是生产
资源 ledger 的证据。生产设置 UI→daemon、原生事件入口、性能验收、完整资源
账本与 listener/manager/auth/writer/打包仍待完成；P0 保持未通过。

2026-10-20 的完整配色集成复验（基于 `dab0da0`）：实际运行
`python3 scripts/probe-session-process.py --mutations`，退出 0。环境为
Darwin 25.6.0 arm64、Xcode 27.1 (27A9275)、Apple clang 21.0.0，Ghostty pin
`81598385d608dda4946093d6d6a234d204a2f225`。198 项变异为 103 process +
11 wire + 33 GUI-budget + 8 summary-wire + 3 summary-queue + 8 command-wire +
11 result-wire + 4 scheme-owner + 13 process-scheme + 4 scheme-pixel；目标红、
对照绿及 SHA256 回滚通过。最后重新构建，roundtrip、kill、crossing、history、
read-error、recovery-error、apply-error、reply-error、start-error、ime、
state-oom、backend-oom、pointer、scheme 十四类原生场景与 26 类旧 surface
基线通过。完整日志 `tmp/process-integrated-198.log`，临时日志不入库。

同轮 `bash scripts/gate.sh` 退出 0，113/113 测试与 57 个公共符号对拍通过，
日志 `tmp/process-integrated-198-gate.log`。仓库工作树在验证后保持干净，运行中
未改被监测源码。本轮没有新增断言或改公开 ABI，整合验证最近配色与渲染切片。

P0 仍未通过。当前真实 TUI 原生场景只有 vim/less，不能推导 tmux 已验；本机
已只读确认 `/opt/homebrew/bin/tmux` 为 3.6a，但尚无 tmux 恢复场景证据。
系统事件入口、任意主题/配置、通用绘制完成与性能验收、完整资源账本，以及
生产 listener/manager/auth/writer/设置入口/打包仍待完成。完整脚本绿色不改变
这些交付边界。

2026-10-20 的真实 tmux 跨进程恢复（基于 `50ebb63`）：新增 process/tmux
场景，使用 PATH 中的 tmux（本轮 3.6a）。Python 测试控制器在工程 tmp 下建立
独立前台 server、显式配置及专属 socket；PTY 中运行真实 tmux client，pane
运行实际交互脚本。客户端用 -N 禁止偷偷另起 server，缺少 tmux 明确失败，不
跳过或回落到裸 shell。该 server 是测试应用负载，不是 Ghosterm 生产 daemon。

第一代 GUI 导入备用屏幕的 FIRST 画面并完成 READY 后，被真实 SIGKILL；连接
死亡释放旧代，权威 worker、同一 PTY 与 tmux 继续运行。detached 时 resize 和
输入推动 pane 产生 SECOND/VERIFIED；第二代 GUI 经原有快照、增量和 S/L 屏障
恢复，并把实际编码的 bracketed USER 输入送回 pane。pane 逐字核验 7 字节
DSR 与 16 字节粘贴，且在两次交互分别实测 stty 尺寸 31×91、32×92（有界等待
异步 SIGWINCH）。原有红/绿/白及最终蓝色像素断言仍通过。

capture/close 要求权威处于真实 alternate screen、标题为 Ghosterm tmux；两代
GUI 的 canonical/resources 均完整逐字对拍。真实应用没有发送程序摘要或命令
报告，故 PSW 保持空、命令保持 unknown/empty，未伪造已运行/已完成/退出码 0。
旧的有报告场景继续原断言；只扩展测试预期，不分叉恢复算法或渲染接缝。
外层 VT 查询沿用同一 worker reply sink，GUI legacy 写及宿主动作计数为零；
本轮观察到 3 次回调、34 字节外层应答，pane 的 DSR 由 tmux 自身处理。

测试只清理自己的 server：明确 kill-server 并 wait/有界 terminate/kill，随后
核验遗留 socket 的设备/inode、类型、uid 及不可连接状态再 unlink；不会直接
删除替换节点或活端点。配置和环境变量恢复，socket/config 最终归零。四个清理
基线覆盖本代失效 socket、普通文件、替换 inode 和活端点；不把此测试 fixture
当作生产 endpoint 身份/启动锁方案。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`python3 scripts/probe-session-process.py --tmux-mutations` 退出 0。三项清理
变异均目标红、本代正常清理对照绿；六项 C 变异均 tmux 明确退出 1、roundtrip
退出 0，覆盖 pane 查询/粘贴预期错误、backend resize 假成功、未知命令伪造成功
零、权威备用屏幕丢失和 detached 输入遗漏。编译失败不计作目标红。SHA256
回滚、四条清理基线及两类原生场景复绿，日志 `tmp/process-tmux-verification.log`。

同一最终构建的十五类原生进程场景、26 类旧 surface 基线通过，75 个旧进程
变异锚点逐项唯一、仓库源文件哈希未变。otool -L 验权威和 VT dylib 均只依赖
VT/libSystem，无 UI 框架。日志 `tmp/process-tmux-compat.log`，临时驱动
`tmp/verify-tmux-compat.py`。核心 gate 113/113、57 公共符号通过，日志
`tmp/process-tmux-gate.log`。新场景与九项变异已纳入完整脚本，配置总数 207；
本轮未重跑完整集合，最近一次完整 198 项证据仍基于 `dab0da0`。

P0 仍未通过。tmux 应用进程的资源不属于已测 runtime 配额；完整资源账本、
系统事件入口、任意主题/配置、性能与各平台原生验收，以及生产 listener/
manager/auth/writer/设置入口/打包仍待完成。没有改公开 ABI、vendor 或生产绑定。

2026-10-20 的真实 vim/less 跨进程恢复（基于 `860ba1b`）：新增 process/less
与 process/vim，运行系统 `/usr/bin/less`、`/usr/bin/vim`，不再只依赖同进程
worker-surface 的 TUI 证据。本轮为 less 668、Vim 9.1 patches 1–1752，Darwin
25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0；引擎 pin 未变。

两场景均在工程 tmp 建立 200 行受控文件；less 禁用用户 LESS 过滤/历史设置，
vim 用 -u NONE/-i NONE/-n 并禁标题修改，测试文件独占创建、finally 删除，环境
变量恢复。真实应用先显示首页，第一代 Metal GUI 导入备用屏幕后被 SIGKILL。
同一 PTY/worker 在 detached 期间 resize，less 收到 G 后显示末页，vim 收到
iHELLO 后保持插入状态；随后在新 generation 上捕获并恢复到第二代 GUI。

LIVE 后通过真实 surface key API 编码事件：less 收到 g 返回首页，vim 接受
USER 文本并产生新的输出。按协商模式传输有界编码字节（1–128），没有把所有
TUI 强行当作固定 bracketed paste 载荷；旧场景继续原字节断言。成功必须等待
提交输入之后的新程序输出，不能拿第一代 GUI 之前的首页标记冒充后续响应。
故意确认成功却不转发输入，会被新的 since-offset 观测拦住。

两代 GUI 都按原快照/有序增量/S-L 协议恢复，初始及最终完整 canonical/resources
逐字对拍；权威要求 alternate screen，未观察到的标题/cwd、程序摘要与命令保持
empty/unknown，不伪造退出码 0 或时长。设备属性由权威回调应答，GUI 的 legacy
写和宿主动作仍为零。三次 TUI 图像采集要求真实白色内容非空，未声称字形逐像素
一致；两程序的终态、迟到输入拒绝和分配域释放沿用既有验收。

`python3 scripts/probe-session-process.py --tui-mutations` 退出 0。八项变异
均目标明确退出 1、对应 vim/less/tmux 对照退出 0：detached 导航/插入丢失、
恢复后的错误导航/文本、空白帧、权威备用屏幕丢失、输入 ACK 后实际未转发，
以及将未观察命令伪造为成功零。SHA256 回滚及 less/vim/roundtrip 原生基线
复绿，日志 `tmp/process-real-tui-verification.log`。

同一最终构建的十七类原生进程场景与 26 类旧 surface 基线通过，75 个旧进程
变异锚点逐项唯一、源文件哈希未变；权威与 VT dylib 的 otool -L 仍仅 VT/
libSystem。日志 `tmp/process-real-tui-compat.log`，临时驱动
`tmp/verify-real-tui-compat.py`。核心 gate 113/113、57 公共符号通过，日志
`tmp/process-real-tui-gate.log`。完整脚本已纳入两场景与八项变异，配置总数 215；
本轮未重跑完整集合，最近完整 198 项证据仍基于 `dab0da0`。

这些是 macOS arm64 的键盘 API/结构化状态与真实程序交互证据，不替代系统
NSTextInputClient/原生鼠标焦点事件入口、任意主题/字体、通用绘制完成或性能
验收。截图/应用内存不是完整 runtime ledger 的证明。生产 listener/manager/
auth/writer/设置入口/打包与各端收口仍待完成，P0 保持未通过。公开 core ABI、
vendor 和生产平台绑定未改。

2026-10-20 的 AppKit 鼠标跨进程恢复切片（基于 `f8523d7`）：新增
process/native-pointer 与实验 `session-appkit-host.inc`。合成 NSEvent 经
`NSApp sendEvent:`、真实 NSWindow/NSView 分发，宿主进行 view 坐标转换及 y
翻转，并在按下后检查实际 firstResponder。恢复前禁输入，LIVE 后原始按下/
释放报告连同已有焦点 API 的 O/I 报告经 stamped INPUT、IPC、同一权威 worker
进入原 PTY，shell 逐字检查 24 字节报告，再接收 16 字节 bracketed paste。
终端输出仍由 I/O 线程直接 feed surface，不进入宿主事件队列。

五项定向变异覆盖恢复期放行、y 翻转丢失、释放变成按下、按下丢失，以及点击
后 firstResponder 错交给窗口；每项要求 native-pointer 明确退出 1、旧 pointer
对照退出 0，最终 SHA256 回滚及两个原生基线复绿。NSWindow 已会在鼠标分发中
设置 firstResponder，故没有把删除宿主重复设置当成有效故障。

本轮实际通过定向函数 `native_pointer_mutations`（临时驱动调用脚本同名函数，
安装同一 INT/TERM handler），日志 `tmp/process-native-pointer-targeted.log`。
完整入口 `--native-pointer-mutations` 已纳入五项变异，完整集合配置数 220；
本轮未重跑完整集合，最近完整 198 项证据仍基于 `dab0da0`。
十八类原生、26 类旧 surface、75 个旧变异锚点唯一性、源哈希与无 UI 权威
依赖对拍见 `tmp/process-native-pointer-compat.log`，驱动
`tmp/verify-native-pointer-compat.py`。核心 gate 113/113、57 公共符号通过，
日志 `tmp/process-native-pointer-gate.log`。公开 ABI、vendor 与生产端绑定未改。

范围仅为 macOS arm64 实验宿主的合成鼠标事件。探针尝试 AppKit 激活后仍未取得
系统 key window，已删除激活逻辑；没有用伪造 isKeyWindow/通知冒充 OS 焦点
证据。窗口 key/resign、系统物理输入、NSTextInputClient、生产 Swift 宿主、
任意主题、通用绘制完成与完整预算仍待核验。生产 daemon listener/manager/
auth/发现列表/恢复 UI/helper 打包未交付，P0 保持未通过。

2026-10-20 的原生文本输入恢复切片（基于 `d845f40`）：实验 AppKit 宿主实现
NSTextInputClient，并由 keyDown 调 interpretKeyEvents。新 process/native-ime
在 LIVE 前经 NSWindow 分发合成按键、模拟组字和提交，要求全部被宿主拒绝，
按键解释器未运行，既不缓存 marked text 也不产生终端输入。LIVE 后真实
firstResponder 接收合成 x 按键，必须经过解释器文本回调，而非只靠普通按键
fallback 碰巧编码出同一字节。

随后通过 NSTextInputClient 模拟中文组字，验证 marked/selected 的 UTF-16
range、引擎 preedit 宽度非零、且没有额外输入。取消组字必须清空标记与引擎
宽度；候选窗锚点按引擎坐标转换到 window/screen 并与独立期望比较。引擎 imePoint.y 是单元格底边，转换不再减行高；独立变异护此约定。再次用
NSAttributedString 组字及提交中文，要求清空组合态、按键入语义输出精确
`x中文` 七字节，无 bracketed paste 包裹。该载荷与后续 16 字节 USER 粘贴经
stamped INPUT/IPC/权威 worker 到达原 PTY，shell 独立逐字核验，沿用完整状态、
终态、迟到输入拒绝及分配域释放检查。高频输出仍直接走 I/O 线程到 surface。

`python3 scripts/probe-session-process.py --native-ime-mutations` 通过十一项
定向变异：解释器绕过、预编辑遗漏、组字泄漏输入、取消未清标记、提交误走
粘贴、提交未结束组合、候选窗 y 翻转丢失、多减一行高度，以及 keyDown/setMarkedText/
insertText 三个恢复期保护分别失效。每项目标退出 1、原 ime API 对照退出 0，
SHA256 回滚后两场景复绿，日志 `tmp/process-native-ime-verification.log`。
之前五项原生鼠标变异也复验通过；只读鼠标锚点改为精确匹配 mouseDown，避免
新增文本入口后误匹配其他保护，日志 `tmp/process-native-ime-targeted.log`。

最终十九类原生与 26 类旧 surface 基线通过，75 个旧进程变异锚点仍逐项唯一，
源哈希未变；权威与 VT dylib 无 AppKit/Foundation/Metal 依赖。日志
`tmp/process-native-ime-compat.log`、驱动 `tmp/verify-native-ime-compat.py`。
核心 gate 113/113、57 公共符号通过，日志 `tmp/process-native-ime-gate.log`。
完整脚本已纳入新场景与十一项变异，配置总数 231；本轮没有重跑完整集合，最近
完整 198 项记录仍基于 `dab0da0`。公开 core ABI、vendor 与生产端代码未改。

这是 macOS arm64 合成普通按键与模拟 NSTextInputClient 回调的实验宿主证据，
不是物理键盘、系统中文输入法选字、系统 key-window、生产 Swift 宿主或设置
入口的恢复验收。NSString/AppKit/候选窗等内存不构成完整 runtime ledger
证明；任意主题、通用绘制完成、跨进程总预算仍待核验。生产 daemon listener/
manager/auth/发现列表/恢复 UI/helper 打包未交付，P0 保持未通过。

2026-10-20 的完整进程原型集成复验（基于 `aa60729`）：
`python3 scripts/probe-session-process.py --mutations` 退出 0，十九类原生场景、
231 项变异和 26 类旧 surface 最终基线通过。不是将不同构建的定向结果相加：
同一命令按序运行全部组，完成 SHA256 回滚后重新构建，并再次运行十九类原生
与所有旧 surface；所监控源文件及补丁的起跑哈希在 finally 中逐项保持一致。
日志 `tmp/process-integrated-231.log` 中实际有 231 条 target red/control green
记录，与分项计数一致。此前基于 `dab0da0` 的完整 198 项记录保留为历史证据。

本次覆盖 roundtrip/kill/crossing/history、read/recovery/apply/reply/start
错误、state/backend OOM、IME API、鼠标/焦点 API、固定配色档、tmux、less、
vim、AppKit 合成鼠标和原生文本输入。最终真实 PTY/Metal GUI 的结构化状态、
有序输入、查询 owner、终态与资源释放检查继续通过；配色六个时点的背景像素、
TUI 非空画面及旧 surface 的资源/continuation 等基线同一运行通过。

计数口径：103 process = 原有 75 + startup 10 + runtime 6 + IME API 4 +
backend OOM 4 + pointer API 4；33 budget = GUI-budget 14 + state OOM 5 +
combined 9 + metadata 5。其余为 wire 11、summary-wire 8、summary-queue 3、
command-wire 8、result-wire 11、scheme-owner 4、process-scheme 13、
scheme-pixel 4、tmux 9、real-TUI 8、native-pointer 5、native-IME 11。
预算的 19 项没有同时算进 103 process，避免重复或漏计。

同一源树的 `bash scripts/gate.sh` 退出 0，113/113、57 公共符号与错误入口
闸门通过，日志 `tmp/process-integrated-231-gate.log`。最终 `otool -L` 另验
process-server 与 VT dylib 仅依赖 VT/libSystem，没有 AppKit/Foundation/Metal，
日志 `tmp/process-integrated-231-linkage.log`。本轮没有改实现或新增闸门。

环境 macOS 26.6.2 (25G83)、Darwin 25.6.0 arm64、Xcode 27.1 (27A9275)、
Apple clang 21.0.0 (clang-2100.3.34.2)；Ghostty pin 仍为
`81598385d608dda4946093d6d6a234d204a2f225`。实际链接产物和补丁共同生成的
engine build 身份为 `680e3ca7eac6efb3f4cbb209f8eef22736b3cb789634468014211fe13aa0113a`。
源/补丁/日志 SHA256 与环境记录在 `tmp/process-integrated-231-manifest.json`，
清单 SHA256 为 `9568ab969e66e0f0387741af59325bf88bf9f562ecb9050aa2601da1ad8209c7`；
可按上述基准提交、钉版本和命令重新运行完整验证，临时日志不入库。

完整集合通过仍不等于 P0 通过或生产支持：连接仍是继承的私有 socketpair，
AppKit 证据仍是合成事件和模拟文本回调，系统输入法选字/系统窗口焦点及生产
Swift 接线没有新增证据；任意主题、通用绘制完成、框架/GPU/全局资源预算和
最终性能验收未完成。listener/manager/auth/writer、发现/恢复 UI、helper
打包以及后续各平台/SSH 迁移仍待交付，能力矩阵继续标 ❌。

2026-10-20 的主线程容量准入切片（基于 `6b4e3e7`）：审计发现进程原型已预留
worker/encoder/waiter/GUI I/O 栈，但权威主线程与 GUI 主线程仍在账本外。
新增私有 Darwin `session-main-stack.h`，仅在实际主线程查询 pthread 栈容量，
检查当前局部变量位于该栈，并经 mach_vm_region 核验其下方完整连续、不可访问
的保护区；查询失败、边界不一致、溢出或合计超过 128 MiB 明确拒绝，不回落到
猜测值。Mach object port 查询后释放。此接口不进入 portable core 或公开 ABI。

本机实际栈为 8,372,224 字节，连续 VM guard 为 58,736,640 字节，合计
67,108,864 字节（64 MiB）。整份虚拟容量计入承诺，不把 guard 当成新增 RSS，
也不只计算一页保护区。OS 已创建主栈；原型在启动 worker/I/O 和接纳 surface
之前预留既有容量，不修改栈大小或保护区映射。server/process_client 等已有
主栈局部对象被这份容量覆盖，不再另算一份对象字节。

权威 normal 总限额从 256 MiB 的原覆盖范围加上实测 main capacity；startup/
state/backend 失败场景同样加入独立 main scope，保持原 source/线程限额不变。
GUI 总量加同一实测容量，通过既有 pool reserve 与 surface/runtime 共享准入，
在进程 pool 中只预留一次，不随 surface 重复计费；
I/O join 只归还 I/O 份额，主线程份额保留至 bootstrap/pool 最终关闭。资源
账本最终回到零不表示 OS 主栈已消失，只表示已无活动会话/恢复任务、准入域
正在销毁，随后进程退出；没有把常驻栈从 RSS 中假删。

独立 measurement/admission/thread 三类基线验证实际 RLIMIT_STACK、完整 guard
计费、容量与承诺一致、错误承诺拒绝、非主线程拒绝且输出清零。
`python3 scripts/probe-session-process.py --main-stack-mutations` 退出 0，十一项
变异目标红/对照绿、SHA256 回滚及 roundtrip/start-error/state-oom/backend-oom
复绿。覆盖 owner/GUI 未预留、漏释放、漏 guard、全局容量漏项，以及测量漏
保护区、准入忽略 committed、工作线程误判为主线程。正常路径要求明确退出 1，
独立 helper 检查要求非零；日志 `tmp/process-main-stack-verification.log`。
owner 未预留变异用“只初始化 scope、不承诺容量”，避免未初始化对象崩溃冒充
有效的准入断言。SIGINT/TERM 沿用既有 handler/finally 恢复和起跑 SHA 校验。

最终十九类原生、26 类旧 surface、75 个旧进程变异锚点唯一性、源哈希及无 UI
权威/VT 链接复验通过，日志 `tmp/process-main-stack-compat.log`，驱动
`tmp/verify-main-stack-compat.py`。核心 gate 113/113、57 公共符号通过，日志
`tmp/process-main-stack-gate.log`。完整集合已纳入十一项，配置总数 242；本轮
未重跑完整 242，最近完整 231 项仍基于 `aa60729`。引擎 pin、上游补丁、公开
core ABI 与生产端绑定未改。

这是当前 macOS arm64 进程原型的 bootstrap 虚拟容量准入证据。Framework/GPU/
TLS/其他系统线程、全局资源上限、任意主题、通用绘制完成、系统输入法与窗口
焦点及生产 Swift 接线仍有缺口；不能据此声称完整 runtime ledger 或 P0 通过。
生产 listener/manager/auth/writer、发现/恢复 UI 与打包仍待交付。

2026-10-20 的 full surface 线程容量切片（基于 `bc0785c`）：审计发现 renderer
和 I/O 使用 Zig 0.16.0 的 std.Thread.spawn 默认栈（每个 16 MiB），原
surface CPU/page allocator 不覆盖这两份 pthread 栈。

新增实验补丁 `0032-surface-thread-observation.patch` 与 WHY：提供工具链默认
栈大小、Darwin 活 surface 的 renderer/I/O 线程 handle 只读查询。handle 是
当前进程内借用值，只在 surface_free 前使用，不序列化，不参与输入授权，
非 Darwin 查询分支返回 false，其他端尚无编译/运行证据；线程创建及 join
实现没有改变。正式脚本从干净 pin 依次应用全部补丁后重建通过。

GUI pool 的每 surface lease 新增可选 threads 容量域，在构造引擎前与三个
数据域一起准入；线程容量拒绝时回滚三个数据域、失败 scope 和 descriptor，
不占 surface 槽。threads=0 显式排除线程预算，不创建空域。非零容量在
surface_free 已 join renderer/I/O、且延期分配释放完成后才随 lease 归还；
主线程及宿主 I/O 的进程级 reservation 仍独立保留，不随 surface 释放。

宿主按查询值为两个线程各预留 stack + 2 OS pages（Darwin 栈报告额外空间与
完整 VM guard），当前合计 33,619,968 字节/Surface。初始及两次 sibling
构造均通过真实 pthread_get_stacksize_np/stackaddr 和 mach_vm_region 检查，
每线程报告栈 16,789,504、guard 16,384、承诺 16,809,984 字节；两角色必须
是不同非主线程。空对象、无效角色及空输出参数明确拒绝。两个实际 surface
的完整状态、第三个拒绝、隔离故障、释放与重用继续通过；全局额度只增加
新承诺，不扩大原 general/pages/wire 的单 surface 限额。

新增 threads 预算基线覆盖先预留 8 页、第二份数据域已满 14 页时在线程域
拒绝、重复拒绝不泄漏、释放及整数溢出。共享预算基线从 12 增至 13，日志
按实际用例数输出。`python3 scripts/probe-session-process.py --engine-thread-mutations`
退出 0：十一组目标红/对照绿、源码 SHA256 回滚及最终基线复绿，日志
`tmp/process-engine-threads-verification.log`。其中两组真的修改临时引擎并重建：
让 I/O 查询返回 renderer（句柄重复），以及将实际 renderer 栈翻倍至 32 MiB
（真实 pthread 栈超过承诺）；对应旧 surface continuation 对照仍绿。其他组
护未预留、拒绝回滚、释放、溢出、少算线程/guard、池漏项及观测漏报。

新增拒绝路径使两个旧替换锚点出现歧义，已收窄到原三个数据域的回滚路径；
14 GUI + 9 combined 旧变异重新通过，源哈希未变，日志
`tmp/process-engine-threads-budget-compat.log`。最终十九类原生、26 类旧 surface、
75 个旧进程锚点唯一性与无 UI 权威/VT 链接复验通过，日志
`tmp/process-engine-threads-compat.log`、驱动 `tmp/verify-engine-threads-compat.py`。
核心 gate 113/113、57 公共符号通过，日志 `tmp/process-engine-threads-gate.log`。
完整集合配置数为 253，本轮未重跑完整集合，最近完整 231 仍基于 `aa60729`。
Ghostty pin、生产平台绑定和公开 core ABI 未改，新增补丁仅在实验路径应用。

以上是当前 macOS arm64 原型的线程虚拟容量证据，不是 RSS 或完整 runtime
ledger。std.Thread 的入口 Args 使用 libc allocator 且持有至线程结束，TLS、
搜索/其他框架线程、App/共享字体/GPU 与跨进程总预算仍未覆盖；系统输入法/
窗口焦点、任意主题、通用绘制完成和生产 daemon 链路仍待交付，P0 未通过。

2026-10-20 的线程入口帧预算与初始化失败清理切片（基于 `6a2bfc4`）：新增实验
补丁 `0033-surface-thread-args.patch`。Zig 0.16.0 POSIX spawn 忽略配置中的
allocator，旧 Args 走 libc 且保留至线程退出。预算型 Darwin surface 现在用
专用 pthread 启动器，入口 Frame 由可选 p0_thread_allocator 分配；省略时用
已有宿主 general allocator，分配拒绝直接失败，不回落 libc。Thread allocator
单独出现、缺少完整预算及 host/checkpoint 约束时拒绝创建。

Thread 包装保存标准 std.Thread 或预算 pthread handle，不构造 std.Thread 的
私有 Impl。未启用预算的路径继续用 std.Thread；预算路径保持原默认栈/guard、
maybeAttachSignalStack、命名与 join。renderer/I/O 的 stack 容量政策未降低，
非 Darwin 预算线程路径明确报 UnsupportedThreadAllocator，其他平台仍无原生
编译/运行证据。新的只读 allocator descriptor 查询仅供当前进程核验实际选择，
不序列化指针；所有回调和 allocator descriptor 要活到 surface_free。

完成 IO 对象初始化后，失败清理责任从临时局部副本转交已发布的 Surface；
初始化后续失败时先 stop/join 已启动线程，再按 renderer、两个线程对象、
terminal 的顺序销毁，避免运行线程继续访问已释放状态、notifier 或 display
link。第一帧拒绝时没有线程启动；第二帧拒绝时已启动 renderer 被停止/join，
其入口帧归还。健康路径各持有两个入口帧，surface_free 后实际 free 两次。

原生探针专用 frame provider 将请求转交同一 general 预算，并独立限制存活
入口帧数为 0/1/2；记录真实 alloc/free/denial。每个正常恢复均验证首个/第二个
请求拒绝时构造失败、所有帧及临时 lease 回收、原 surface 仍健康，随后两个
正常 sibling 的构造/拒绝/释放重用继续通过。省略可选 provider 时另创建真实
surface，逐线程核验 allocator descriptor 确实为其 general 域。

独立 Zig 四例覆盖成功线程、选定 allocator、分配拒绝不运行入口、无效栈参数
引起启动失败后的回收，以及内核 sigaltstack 保留已配置的备用信号栈。
`python3 scripts/probe-session-process.py --thread-args-mutations` 退出 0：
八项目标红/对照绿、SHA256 回滚及最终单元/原生基线复绿，日志
`tmp/process-thread-args-verification.log`。三组单元变异护正常 free、启动失败
errdefer、信号栈初始化；五组临时引擎重建变异护 libc 绕过、指定域忽略、默认
域选错、缺预算接受 thread-only allocator，以及分配失败悄悄回落 libc。
单元目标明确退出 1/对照 0，原生目标退出 1/旧 continuation 对照 0；编译失败
不算目标红。另跑 `--engine-thread-mutations`，原十一项栈准入变异复绿，包括
新预算 pthread 路径下真实 renderer 栈翻倍，日志
`tmp/process-thread-args-stack-compat.log`；旧物理栈变异锚点同步到预算启动器。

最终十九类原生、26 类旧 surface、75 个旧进程锚点唯一性、源哈希及无 UI
权威/VT 链接检查通过，日志 `tmp/process-thread-args-compat.log`，驱动
`tmp/verify-thread-args-compat.py`。核心 gate 113/113、57 公共符号通过，日志
`tmp/process-thread-args-gate.log`。完整集合配置数 261，本轮未重跑完整集合，
最近完整 231 仍基于 `aa60729`。Ghostty pin、公开 core ABI 与生产绑定未改。

这里只关闭了预算型 renderer/I/O 的入口帧分配缺口。TLS/备用信号栈、libpthread
内部、搜索/其他框架线程、App/共享字体/GPU 和跨进程总预算仍未完成；系统
输入法/窗口焦点、任意主题、通用绘制完成及生产 listener/manager/auth/writer、
发现/恢复 UI/helper 打包仍待交付，P0 保持未通过。

2026-10-20 的宿主拥有备用信号栈切片（基于 `70c5f3b`）：实验补丁
`0034-owned-signal-stack.patch` 把预算型 renderer/I/O 实际注册的缓冲区放进
入口 Frame，大小仍取 std.options.signal_stack_size。保留正常 pthread 栈和
VM guard，不关闭、缩小或取消已配置的备用信号栈；配置显式为 null 则不注册。
每线程注册前保留旧状态，工作返回后先恢复旧注册，再归还 Frame。

Darwin 真实 C 探针发现：新 pthread 查询返回 SS_DISABLE、size=0，但把这份
状态直接传回 sigaltstack 会报 ENOMEM；即使 SS_DISABLE，仍需满足
MINSIGSTKSZ（本机 32768）。恢复禁用状态时只补足最小 size，保留禁用语义；
已有活动栈则保留其原地址和长度。检查 free 时是否仍注册还必须看 DISABLE
标志，不能只看非零 size。临时 C 探针在工程 tmp 中执行，没有改 OS 配置或
用户应用的信号状态。

五类独立 Zig 基线验证分配/启动/释放、内核登记地址位于实际分配帧内、free
之前已恢复注册，以及嵌套活动栈恢复。期望大小直接取 std 配置，不复用待测
helper 派生常量，避免两边一起缩小而假绿。Frame 的信号区按 16 字节对齐；
错误路径沿用入口帧 allocator 和先 join 后回收的初始化事务。

原生 roundtrip 记录配置为 262144 字节/线程，两帧实际逻辑请求峰值 524352
字节，进入既有 general 域；这是请求量，不是 RSS，也不等于 provider 的
页舍入映射量。正常 Root/sibling 在真实构造后分别核对两帧至少覆盖配置字节，
首/次请求拒绝与最终 free 计数继续通过。新增私有只读大小接口不传输地址，
公开 core ABI 与生产绑定未改。

`python3 scripts/probe-session-process.py --signal-stack-mutations` 退出 0，
六组目标红/对照绿，源码 SHA256 回滚及最终基线复绿，日志
`tmp/process-signal-stack-verification.log`。五组单元变异覆盖派生大小减半、
注册 TLS 替代堆内存、释放前漏恢复、错误禁用活动旧栈、Darwin 禁用大小未
归一化；最后一组必须确实报告 SizeTooSmall，其他异常不能冒充有效失败。
原生变异真实重建半尺寸缓冲区，Frame 请求量检查变红，旧 continuation 仍绿。
编译失败不计入目标红。

`--thread-args-mutations` 的原八组复绿，信号设置变异锚点同步到新注册路径，
日志 `tmp/process-signal-stack-args-compat.log`。最终十九类原生、26 类旧 surface、
75 旧进程锚点唯一性、源哈希及无 UI 权威/VT 链接复验通过，日志
`tmp/process-signal-stack-compat.log`，驱动 `tmp/verify-signal-stack-compat.py`。
核心 gate 113/113、57 公共符号通过，日志 `tmp/process-signal-stack-gate.log`。
完整集合配置数 267，本轮未重跑完整集合，最近完整 231 仍基于 `aa60729`。
Ghostty pin 未变，补丁只在实验路径应用，其他平台没有新增运行证据。

这只证明当前注册信号缓冲区的宿主所有权、计费与恢复；不证明 TLS 分配消失
或 RSS 下降。标准库链接的未使用 TLS 数组仍可能随映像 TLS 分配，其他 TLS、
libpthread、搜索/框架线程、App/字体/GPU、跨进程总预算仍未完成。系统输入法/
窗口焦点、任意主题、通用绘制完成及生产 listener/manager/auth/writer、会话
发现/恢复 UI/helper 打包仍待交付，P0 保持未通过。

2026-10-20 的搜索线程预算与失败回滚切片（基于 `1553f54`）：实验补丁
`0035-budgeted-search-thread.patch` 将预算型 surface 的惰性 search 线程接入
既有宿主线程 allocator；未预算路径继续使用标准线程。每个 surface 在构造
前预留 renderer/I/O/search 三份栈与 guard，当前共 50,429,952 字节；搜索
停止后入口帧与状态归还，容量保留到 surface lease 释放，供下一次搜索使用。
这份容量与实际存活线程数分别核验，空闲时不虚报已启动第三个线程。

另修复搜索启动失败只销毁 state 却保留 Some 的旧路径：错误退出同时撤销槽位，
避免之后查询、重试或销毁触碰未初始化的 thread。只读 presence 查询先验空槽，
不会为检测陈旧槽而读取无效句柄。角色 3 的句柄只在当前搜索存活时可借用，
不能跨 end_search 保存或序列化，所有查询由 surface main owner 发起。

新增原生 search 场景，先拒绝第三个入口帧分配并验空槽，再允许重试两轮；
真实 search_total 低频回调必须报告 FIRST 的匹配，校验实际 pthread、独立
句柄、宿主 allocator、栈/guard。第一轮停止须归还第三个帧，第二轮保持搜索
活跃后 SIGKILL GUI；同一 PTY 与新代 GUI 恢复后再次搜索，最终完整状态与
资源对拍。正常销毁必须回收活跃搜索；最终入口帧 attempts=5、freed=4、
denied=1、final_live=0，峰值请求 786,528 字节。终端输出仍由 I/O 直送 surface。

`python3 scripts/probe-session-process.py --search-mutations` 七项定向变异覆盖
少预留第三份容量、搜索绕过 allocator、失败残留槽、失败泄漏状态、忽略搜索
文本、停止遗弃线程、搜索角色冒用 I/O 句柄；每项目标红且独立对照绿，源码
SHA256 回滚后基线复绿。编译失败不计目标红，日志
`tmp/process-search-verification.log`。原十一项 engine-thread 变异复验通过，
日志 `tmp/process-search-engine-compat.log`。最终二十类原生、26 类旧 surface、
五类入口帧单元、75 旧进程变异锚点唯一性及源哈希/无 UI 链接复验通过，驱动
`tmp/verify-search-compat.py`，日志 `tmp/process-search-compat.log`。
核心 gate 113/113、57 公共符号通过，日志 `tmp/process-search-gate.log`。
完整集合配置数 274，本轮未重跑完整集合，最近完整 231 仍基于 `aa60729`。

Ghostty pin 未变；补丁只在实验路径应用，未改公开 core ABI 或生产绑定。
这只补齐当前搜索线程的预算与清理证据；其他 TLS、libpthread/框架线程、
App/字体/GPU 与跨进程总账本仍未完成。系统输入法、通用绘制屏障和生产
listener/manager/auth/writer、会话发现/恢复 UI/helper 打包仍待交付，P0 未通过。

2026-10-20 的 App 共享分配域切片（基于 `5850f27`）：实验补丁
`0036-app-allocator.patch` 为 runtime config 增加可选宿主 allocator，覆盖
CoreApp、embedded App、克隆配置与经 App allocator 分配的共享字体容器。
创建、错误回滚与 app_free 使用同一域；未提供时仍用默认分配器。只读
allocator 查询返回进程内描述符借用，不跨 IPC 或序列化。

GUI 进程在同一预算池中另预留 64 MiB App 域，先完成主线程与 App 准入再
创建引擎 App，surface 不再先于这份准入持有一个隐形全局 App。runtime
reservation 可选择使用其有界 allocator；活跃分配阻止 unreserve，分配超额
是局部粘性故障，旁边的 surface 域保持健康。App 销毁后才能撤销容量及关闭
总池。原始 config 与全局初始化目前仍在该域之外，不能宣称全部引擎内存
已经计费。两个 detached 预热线程、CoreText/Metal 内部、其余 TLS/框架/
GPU 资源与跨进程总账本仍须收口。

正常创建前分别拒绝前四次分配，要求创建明确失败、已分配对象归还、空域可
重用；成功后核验 runtime App 对象确实来自第二次宿主分配，CoreApp 的真实
allocator 与宿主描述符相同，配置副本有实际计费。正常回收完整 App 后宿主
分配归零；启动失败也必须销毁 App 后才能关闭池。私有 runtime-allocator
基线另验页取整、容量计费、活跃对象 BUSY、超额故障隔离和回收后关闭。

`python3 scripts/probe-session-process.py --app-budget-mutations` 十项目标红/
对照绿、SHA256 回滚和原生基线复绿，日志 `tmp/process-app-verification.log`。
变异分别破坏 runtime allocator、活跃内存检测、故障可见性、BUSY 口径、
scope 关闭、总池 App 容量、CoreApp/embedded App/克隆配置的分配域与 App
对象释放；引擎变异必须编译成功并由原生检查明确失败，不能用崩溃冒充。
旧 GUI 14、combined 9、metadata 5、main-stack 11 项变异另复绿；最终二十类
原生、26 类旧 surface、14 类 GUI 预算与五类入口帧单元、75 旧进程锚点唯一性、
源哈希/无 UI 链接复验通过，驱动 `tmp/verify-app-compat.py`，日志
`tmp/process-app-compat.log`。核心 gate 113/113 与 57 公共符号通过，日志
`tmp/process-app-gate.log`。完整变异配置数 284，本轮未运行完整集合；最近完整
231 仍基于 `aa60729`。Ghostty pin、公开 core ABI 和生产绑定未变，其他平台
没有新增运行证据。P0 仍未通过，生产 daemon 与 P1–P8 仍未交付。

2026-10-20 的 App 预热线程所有权切片（基于 `f91ab06`）：实验补丁
`0037-owned-app-warmups.patch` 将预算型 App 的字体与 Metal 预热改为两个
受拥有的 joinable 线程，实际预热工作继续执行。完成 App 配置克隆后才启动，
使用已有预算线程入口与宿主信号缓冲；单独提供 thread allocator，省略时
继承 App allocator。App 域缺失却设置线程 allocator/observer 会明确失败，
不静默回落。未预算路径保持上游标准预热线程行为。

第二次启动失败必须先 join 第一次已启动工作，然后销毁配置/runtime/core；
正常 App.deinit 在释放共享状态前 join。观察分三个阶段：worker 开始、
worker 工作完成、owner join 完成，避免将完成工作冒充释放线程生命周期。
GUI 在同一池中另预留两份栈/guard，当前共 33,619,968 字节，持有至 App
销毁与 join 完成。实际线程内测栈、VM guard 和 sigaltstack，校验两个 role
都开始并完成且由 main owner join，两个入口帧最终均归还。帧/信号缓冲使用
App 的有界数据域，栈容量单独计费，不重复宣称为实际 RSS。

正常 App 创建前分别拒绝第一次和第二次线程入口分配，核验明确失败、已
启动线程归还、App 对象与配置全部回收，预算域随后可重试。保留前四次 App
分配拒绝测试。独立 warmup 测试验证两项实际工作、第二次失败后的 joined
回滚、活跃工作阻止 join 返回；连同导入的五项线程入口测试共八项。

`python3 scripts/probe-session-process.py --warmup-mutations` 十项目标红/
对照绿、SHA256 回滚与正常恢复/启动失败/搜索基线复绿，日志
`tmp/process-warmup-verification.log`。三项单元变异破坏实际工作、失败回滚
和 join 等待；七项原生变异改回标准线程、忽略提供的 allocator、真实栈翻倍、
移除 App 域前置检查、漏工作完成报告、App 销毁漏 join 与容量减半。引擎
变异必须编译成功并由检查明确失败，编译错误/崩溃不能充数。

旧 App 预算十项、main-stack 十一项与 engine-thread 十一项变异复验通过，
最终二十类原生、26 类旧 surface、14 类 GUI 预算、八项 warmup/入口单元、
75 旧进程锚点唯一性、源哈希及权威/VT 无 UI 链接复验通过，驱动
`tmp/verify-warmup-compat.py`，日志 `tmp/process-warmup-compat.log`。
核心 gate 113/113 与 57 公共符号通过，日志 `tmp/process-warmup-gate.log`。
完整变异配置数 294，本轮未运行完整集合，最近完整 231 仍基于 `aa60729`。
Ghostty pin、公开 core ABI、生产平台绑定未变，其他平台无新增运行证据。

原始配置、全局初始化、CoreText/Metal 框架内部、libpthread/TLS 与其他线程/
GPU/跨进程总账本仍须收口；尚无生产 listener/manager/auth/writer、公开
session API、会话发现/恢复 UI/helper 打包。P0 保持未通过，P1–P8 未交付。

2026-10-20 的预热变异隔离修正（基于 `401d6f0`）：完整整合复验期间回查
验证器，发现 warmup 组最后一个 C-only 容量变异跳过 full engine build；
前一个“App 销毁漏 join”变异虽然恢复了源码，静态库仍可能保留该变异。
原容量检查确实在目标容量断言处变红，但不能把这当作只有一个故障的隔离
验证。现在每个原生变异前都从当前源码重建引擎，含 C-only 变异，确保链接
的是回滚后的正常库。此修正不改引擎、协议或预算策略，也不增加断言。

`python3 scripts/probe-session-process.py --warmup-mutations` 修正后退出 0，
十项目标红/独立对照绿、SHA256 回滚及 roundtrip/start-error/search 基线
复绿，日志 `tmp/process-warmup-isolation.log`。八项 warmup/线程入口单元
继续通过。核心 gate 113/113 与 57 公共符号通过，日志
`tmp/process-warmup-isolation-gate.log`。

修正前完整整合进程已停止，日志保留在
`tmp/process-integrated-294-pre-isolation.log`，不能记作完整 294 项通过。
配置仍为 294；最近已经完成的完整集成仍是基于 `aa60729` 的 231 项。
修正后的完整集合需重新运行，不能拼接不同轮次的定向结果代替同次通过。
P0 保持未通过，生产 daemon 与后续交付范围不变。

2026-10-20 的资源与线程完整集成复验（基于 `1cde0fa`）：
`python3 scripts/probe-session-process.py --mutations` 退出 0，同次通过二十类
原生场景、294 项变异及 26 类旧 surface 最终基线。每组源码 SHA256 回滚后
重新构建，最终再次运行全部原生和旧 surface；62 个源码/补丁哈希与起跑
记录一致。日志 `tmp/process-integrated-294.log` 实际含 294 条 target red/
control green，不以不同轮次的定向结果拼接。此前完整 231 项保留为历史记录。

新增整合部分为主线程容量 11、surface 线程容量 11、入口帧 8、备用信号栈 6、
搜索线程 7、App 共享预算 10、App 预热所有权 10，合计 63 项；与此前 231 项
合为 294。其余分项口径沿用本日完整 231 记录，其中 process 103、GUI-budget
33 不重复计入彼此。原生场景新增 search，最终 GUI 预算基线为 14 类。

本轮包含 `1cde0fa` 的验证器隔离修正：C-only 预热容量变异前同样重建引擎，
不能链接上一项已回滚源码但尚未重建的故障静态库。修正前中断的整合日志
不作为完整通过证据。真实 PTY、Metal GUI、两代恢复、查询 owner、TUI、
合成 AppKit 输入、固定配色像素、错误终态与资源回收均在当前补丁链下复验。

同一代码树 `bash scripts/gate.sh` 退出 0，113/113、57 公共符号及错误入口
检查通过，日志 `tmp/process-integrated-294-gate.log`。`otool -L` 另验权威
进程与 VT dylib 不链接 AppKit/Foundation/Metal 等 UI 框架，日志
`tmp/process-integrated-294-linkage.log`。Ghostty pin 为
`81598385d608dda4946093d6d6a234d204a2f225`，实验补丁至 0037；本轮未改实现或增加断言。
环境 macOS 26.6.2 (25G83)、Darwin 25.6 arm64、Xcode 27.1 (27A9275)、
Apple clang 21.0.0；实际 engine build 身份
`79f2a5b785d109a3e4959f25552beade47a6665e4ce309e2574410a4e8642037`。
源码/补丁/日志 SHA256 与环境见 `tmp/process-integrated-294-manifest.json`，
清单 SHA256 为 `69af4e58fccf1f6fc0e545649a73d2258e0060a0e5a391180b86847d2ee9fdef`。

完整通过仍不代表 P0 或生产 daemon 完成。历史双限额与截断摘要尚未在真实
权威/副本链路交付；原始配置/全局初始化、框架/GPU/其余系统资源总账本、
系统输入法/窗口焦点、任意主题、通用绘制完成与最终性能验收仍未收口。
连接仍是私有继承 socketpair；生产 listener/manager/auth/writer、公开
session API、发现/恢复 UI/helper 打包及 P1–P8 仍待交付，能力矩阵保持 ❌。

2026-10-20 的宿主隔离场景完成标记修正（基于 `24cffb8`）：兼容复验抓到
isolation 偶发少一份配色应答。旧等待标记 SECOND 会先匹配 OSC 通知正文
SECONDNOTE，于是主线程可能在第二次配色查询完成前就停止 worker。现在
使用独立 ISOLATION_DONE，只有子进程读完并验证应答后才输出；在第二条
通知与查询之间刻意分离 0.2 秒，验证不依赖 PTY 恰好合并两段输出。

`tmp/check-isolation-phase.py` 的真实 full surface 场景通过；改回旧标记后，
应答次数/字节断言明确变红，query-delta 独立对照仍绿，SHA256 回滚后两场景
复绿，日志 `tmp/isolation-phase-verification.log`。核心 gate 113/113 与
57 公共符号通过，日志 `tmp/isolation-phase-gate.log`。本切片只修验证器同步，
不增加生产能力，也不将历史完整复验当作“从未存在竞态”的证明。P0 未通过。

2026-10-20 的限额历史淘汰通知接缝（基于 `fa44fb0`）：实验补丁
`0038-history-pruned-notify.patch` 在 PageList 字节回收确认删除历史页后、
Limits.enforce 确认删除历史页后，通知注册的源 owner。C 入口
`ghostty_terminal_p0_set_history_observer` 可注册/注销借用上下文与回调；
无效 terminal 明确返回错误。回调只能记标记，不分配、不重入、不派发 UI。
没有提供精确逻辑行丢弃数量，也不把页级近似限额宣传为精确行上限。

primary reset 保留通知；clone/import 的独立 PageList 默认无通知，函数与
上下文指针不入快照。九类 C 基线分别覆盖独立字节限额、独立行数限额、两者
同时配置、不淘汰对照、清屏/reset 不误报、同块淘汰后 reset、调低策略立即
淘汰、导入后继续输出以及注销。检查实际保留文本：最旧内容已消失、最新内容
仍在，状态无处理错误。导入后未来输出的完整 canonical 仍与源相同。
三个新 PageList 用例具名执行并通过，所选 VT 模块共 36/36 测试。

`python3 scripts/probe-session-history.py --mutations` 退出 0，八项目标红/
独立对照绿，编译与执行分开，编译失败和信号崩溃不能冒充断言变红。变异
覆盖缺少字节/行通知、忽略注册、reset 误报/丢注册、clone 继承源通知、忽略
注销、恢复页缺增长余量；最后一项专门验证既有 0028 对未来淘汰一致性的作用。
SHA256 回滚后全部基线复绿，日志 `tmp/history-pruned-verification.log`。
验证器使用自己的隔离引擎/缓存，注入仅编译测试产物的命名构建步骤，避免
编译失败混入单元测试红；不修改 vendor 或产品构建。标记预检退出 2，活动
变异中的 SIGINT/SIGTERM 均退出并恢复三份起跑 SHA256，记录
`tmp/history-harness-check.log` 与 `tmp/history-harness-check.json`。

`python3 scripts/probe-session-process.py --case history` 原生构建/场景通过，
日志 `tmp/history-pruned-native-first.log`。随后二十类原生、26 类旧 surface、
入口帧/预热单元、75 个旧进程变异锚点唯一性及源码哈希/无 UI 链接复验通过，
驱动 `tmp/verify-history-pruned-compat.py`，日志 `tmp/history-pruned-compat.log`。
期间修复的 isolation 完成标记竞态见前一切片，不用一次重试掩盖回归。
核心 gate 113/113、57 公共符号通过，日志 `tmp/history-pruned-gate.log`。
完整进程变异集合已串行调用本验证器，配置数为 302，本轮未跑完整集合；最近
完整 294 仍基于 `1cde0fa`。Ghostty pin、公开 core ABI 与生产平台绑定未改。

这里只交付真实淘汰的引擎诊断接缝及验证；权威会话尚未注册它、显式双限额
策略/冻结 S 上的截断摘要/有序 LIVE 更新和产品 UI 尚未接线。零历史策略和
其他平台无新增运行证据。系统输入法/焦点、任意主题、通用绘制完成、其余
资源总账本与生产 listener/manager/auth/writer、发现/恢复 UI/helper 打包
仍未完成。P0 保持未通过，不能将可选回调默认空解释成“历史从未截断”。

2026-10-20 的限额历史会话接线（基于 `c5aafcc`，实验 P0）：新增
`history-limit` 场景。权威 VT 在 PTY 启动前显式设置 1 MiB 字节/100 物理行
双限额，读取引擎 getter 核对两项实际配置后才开始处理输出。上限沿用上游
页级近似语义，不声称精确保留 100 行；独立字节/行淘汰证据仍见 0038 验证器。
通知只将会话原子标记置为已截断，真实终端字节仍由 worker 直接送 surface。

私有 32 字节 HST/v1 控制记录区分未知、未截断、已截断；未知不猜限额，已知
策略要求两项正值。编码为固定大端、有保留字节校验，非法输入不修改输出。
不是公开协议或 `ghosterm.h` ABI。协议构建身份升级为
`process-history-restore-1`，不声称兼容旧实验 GUI。

冻结 S 同时捕获策略与截断状态。每条恢复事件在入队时复制同序号的控制状态，
出队后以相同 generation/sequence 发送历史帧；不从最新全局状态拼造旧事件。
客户端拒绝错代、错序、非法策略及状态倒退。真实场景在 S 后触发 1200 行
输出，验证旧快照仍为未截断、LIVE 已截断；已绘制 GUI 被 SIGKILL 后，同一
PTY 与新代 GUI 保留截断状态，并继续验证真实输入、权威回复、程序/命令摘要
和最终 canonical/resources 相等。

P0 AppKit 窗口通过原子控制状态在主线程更新 subtitle，分别显示未知、空提示
和已截断；第一代验证三次状态变化，第二代验证未知到已截断。这是 NSWindow
属性断言，尚无标题栏文字像素或可访问性证据，也不是生产 Swift UI。

`history-state` 的 18 项定向变异全部目标红/独立对照绿，回滚后协议基线及
原生 history-limit/roundtrip 复绿；覆盖协议 header/长度/窄化/策略/字节序/
事务性、遗漏通知、错误的两项实际限额、实时状态替换冻结状态、旧状态污染
事件、错代/错序、未知折叠、隐藏截断和不发布 LIVE UI。编译失败不能算红。
驱动 `tmp/run-history-state.py`，日志 `tmp/history-state-verification.log`；正式
入口为 `python3 scripts/probe-session-process.py --history-state-mutations`。
变异标记预检退出 2，活动变异中的 SIGINT/SIGTERM 均恢复三份起跑 SHA256，
日志 `tmp/history-state-harness.log`。变异过程暴露并补齐“两项限额互相掩盖”
的检查缺口：仅观察发生截断不能证明行数策略正确，必须读取实际配置核对。


本切片另通过 21 类原生场景与 26 类旧 surface、75 个旧进程变异锚点唯一性、
实际入口帧/预热用例及源码哈希；权威/VT 的 `otool -L` 不含 UI 框架，日志
`tmp/history-state-compat.log`。配色组十三项变异全绿；另验证两处重定向的旧
进程变异（初始标题与恢复队列预算）及十一项 wire 变异，目标红/独立对照绿，
回滚后协议基线复绿，日志 `tmp/history-state-retargets.log`。合计本轮复验十八项
新变异和二十六项相关旧变异，不是整套 320 项。核心 gate 113/113、57 个
公共符号通过，日志 `tmp/history-state-gate.log`。环境、源码/补丁和日志 SHA256
汇总见 `tmp/history-state-manifest.json`。

完整进程集合已加入本组，配置数为 320；本轮未跑完整集合。最近完整 294
仍基于 `1cde0fa`，不能以分项验证替代新版本整套集成。零历史/无限历史策略、
产品配置/恢复 UI、系统 IME/焦点、任意主题、通用绘制屏障、其余资源总账本
及生产 listener/manager/auth/writer、发现与 helper 打包仍未交付。P0 未通过。

2026-10-20 的完整进程集成复验（基于 `9331e42`）：
`python3 scripts/probe-session-process.py --mutations` 退出 0，同次通过 21 类
原生场景、320 项变异及 26 类旧 surface 最终基线。日志逐项包含 320 条唯一
目标红/独立对照绿记录，各组回滚后基线复绿。新增八项历史淘汰和十八项会话
历史状态验证已与原有恢复、输入、资源、线程和配色检查串行执行，不能仅把
此前的分项结果相加当作本次证据。67 份起跑源码/补丁 SHA256 与终态完全相同。

日志 `tmp/process-integrated-320.log`，核心 gate 113/113、57 个公共符号通过，
日志 `tmp/process-integrated-320-gate.log`。终态权威进程/VT 的 `otool -L`
无 AppKit/Foundation/Metal 等 UI 框架依赖，记录
`tmp/process-integrated-320-linkage.log`。期间在无 Zig 编译器活动的纯 C 变异
阶段轮换并删除整份闲置构建缓存，保留安装的引擎库及源码；之后的引擎变异
从新缓存重建，记录 `tmp/process-integrated-320-cache-rotation.json`。

Ghostty pin `81598385d608dda4946093d6d6a234d204a2f225`；终态实际引擎构建身份
`50dc6faa586df08aed77a7a953f222d6be7cd87dd41cf351e9ba83ed0a1e647d`。环境、分项通过记录与源码/日志 SHA256 见
`tmp/process-integrated-320-manifest.json`，清单 SHA256 为
`7a802985d51ceffc8bb02c739d2803171d1cfb49f273c2cb47c03b6e4b73c966`。本次替代旧 `1cde0fa`/294 作为最新完整集成记录；旧记录保留其历史范围。

同轮只读配置审计复现：正常文件加载背景 `abcdef`，超长行和目录路径却都
留下默认 `282c34` 且诊断计数为零，日志 `tmp/config-load-audit/results.log`。
因此不能仅凭诊断为空判断配置加载成功。原始 Config 对象/默认 arena/加载
暂存仍在全局 allocator，尚未进入 GUI 额度。另以真实额度耗尽验证独立配置
预算域变为故障时，App/surface 预算域仍健康且可继续分配、最终全部回收，
日志 `tmp/config-domain-audit.log`；这是预算域证据，不代表已绘制 surface 的
配置失败交互已经验证。配置构造/克隆/有界加载候选仍只在临时目录，未交付。

本轮不增加生产 daemon 能力。P0 仍未通过；原始配置、全局初始化、框架/GPU/
其余 TLS 与总预算边界、真实系统 IME/焦点、任意主题与通用绘制屏障仍需收口。
生产 listener/manager/auth/writer、公开生命周期、发现/恢复 UI、helper 打包
及 P1–P8 的其余交付仍未完成；其他平台没有新增原生运行证据。

2026-10-20 的原始配置独立预算切片（基于 `4adb76c`）：实验补丁
`0039-config-budget.patch` 为 C Config 提供宿主构造器和独立目标域克隆。
对象、默认 arena、配置克隆与初始文件暂存使用对应 allocator；释放先保存
arena 的 owner，再 deinit 和归还对象。普通 C 创建仍默认使用全局域。
借用描述符须存活到对应全部配置/克隆销毁；P0 显式克隆可选择另一域，并已验
源配置及其域释放后仍能读取克隆的动态标题字符串。

P0 严格读取初始配置至多 1 MiB，额外一字节用于区分恰好达标与越界。
当前 Zig Reader 达到 limit 即返回 StreamTooLong，首轮正向边界测试暴露了
这一包含边界问题；现已验证恰好 1 MiB 成功、超出失败。物理行至多 4094
字节，首行 BOM 单独处理；空文件、BOM 空文件及末行无换行均有正向用例。
目录/读取失败/长度越界/解析诊断有显式 false；P0 finalize 同样显式报告结果。
这只覆盖初始文件及这些检查，环境映射/路径展开的上游回退仍需后续处理。

GUI 池为根配置预留独立 8 MiB，配色更新另借同池临时 8 MiB 事务；失败自动
销毁部分配置并归还额度。根配置在 App 销毁后、池关闭前释放，不再先于池
准入创建全局占位配置。已有 Metal surface 绘制后，真实一页配置额度耗尽和
超长配置读取失败均发生并回收；根配置/App/surface 域仍健康，随后真实输入、
权威回复与最终 canonical/resources 对拍通过。配色更新继续走原有有序事件
和实际应用屏障，六个时点的背景像素检查保持通过。GUI 元数据峰值本轮观察
为 147456 字节，仍在原有 4 MiB 准入内；额度预留不是 RSS 声明。

19 组配置 API 基线、28 项新变异全部通过；每项目标红/独立对照绿，单元负例
必须以自身 FAIL 断言和退出 1 拒绝，编译失败/信号崩溃不算红。覆盖选域、
克隆、部分构造失败、回收、文件/行/BOM 包含边界、显式错误、空参数拒绝和
GUI 事务/实际故障演练接线。正式入口为
`python3 scripts/probe-session-process.py --config-mutations`；本轮使用相同
函数的缓存驱动 `tmp/config-budget-candidate/run-mutations.py`，日志
`tmp/config-budget-mutations-28.log`。变异开始前检查标记，逐项 SHA256 回滚；
每项均重新确认引擎产物，确保纯 C 变异不会复用上一项被还原的引擎。

活动冷缓存编译中的 SIGINT/SIGTERM 均验证后代终止和四份起跑源码 SHA256
恢复，标记预检退出 2，日志 `tmp/config-budget-harness.log`。这项检查先实际
发现了原构建函数直接调用 subprocess.run 绕过受控 runner 的残留进程；
`tmp/config-budget-harness-unmanaged-build.log` 保留失败，修复后将 runner
显式传入构建函数，编译与链接由独立进程组管理。

另复验 40 项旧变异（App 额度/预热各十、runtime 六、metadata 五、combined
九）、21 类原生和 26 类旧 surface，日志 `tmp/config-budget-compat.log`；
另单独复验引擎线程总额度变异，目标红/独立对照绿与回滚基线均通过，日志
`tmp/config-budget-thread-total.log`。合计复验 41 项相关旧变异。
249 处旧源码锚点按各自预期次数检查，随新配置额度更新了两处总量变异位置。
干净钉版本副本的完整补丁链可应用，配置 API/实现/头文件逐字节匹配已编译
基线，日志 `tmp/config-budget-patch-chain.log`。核心 gate 113/113、57 公共
符号通过，日志 `tmp/config-budget-gate.log`；权威/VT 无 UI 框架链接，记录
`tmp/config-budget-linkage.log`。源码、补丁和日志 SHA256 汇总见
`tmp/config-budget-manifest.json`。

完整集合已配置为 348，本轮未跑全集；最近完整 320 仍基于 `9331e42`。
这里没有生产配置入口或 daemon 能力增量。全局初始化、环境映射/路径展开、
框架/GPU/其余 TLS 和总预算边界，真实系统 IME/焦点、任意主题与通用绘制
屏障，以及生产 listener/manager/auth/writer、公开生命周期、发现/恢复 UI
和 helper 打包仍未完成。P0 保持未通过，其他平台没有新增运行证据。


### 2026-10-20：配置环境副本归属与路径展开错误（P0 未通过）

实验补丁 0040 将配置处理的环境映射副本移入调用方 allocator：包括工作目录、
浅/深色主题、普通配置路径、主题位置及默认 XDG 路径。工作目录和主题的
`~/` 展开失败向上传递，不再仅记日志后返回未展开路径；普通配置路径保留
诊断语义，P0 严格加载器拒绝诊断。全局环境初始化/驻留不在本切片范围。

`python3 scripts/probe-session-config-env.py --mutations` 从干净 pin 副本应用
完整补丁链，向隔离配置模块注入六条定向单元；所选依赖测试一共 83 项通过。
五项变异逐一证明指定 allocator、展开错误与工作目录/两档主题调用点有牙，
均为目标断言红、独立对照绿，SHA256 回滚后全绿。编译失败与信号不能计为
断言红。日志 `tmp/config-env-final-verifier.log`。标记预检返回 2，INT/TERM
实际中断已标记的编译进程组后，后代退出且源码 SHA256 复原；日志
`tmp/config-env-signals.log`。

另复验原生 `roundtrip`、`scheme`（含初始配置 API 的 19 组基线），日志
`tmp/config-env-native-{roundtrip,scheme}.log`；核心闸门 113 项/57 公共符号
通过，日志 `tmp/config-env-gate.log`。没有改原生宿主目录或公共 Ghosterm ABI。
本轮没有跑完整进程变异全集；最近完整 320 项证据仍基于 `9331e42`。
生产 daemon 仍未交付，P0 仍未通过；全局初始化、框架/GPU/其余 TLS、完整
总预算、真实系统 IME/焦点、任意主题/通用绘制屏障与生产生命周期仍待完成。


### 2026-10-20：全局引擎的宿主 allocator 与单次生命周期（P0 未通过）

实验补丁 0041 新增 `ghostty_p0_init` / `ghostty_p0_deinit`，全局初始化可
显式使用宿主 allocator，结束时释放由该 allocator 申请的全局/I/O 对象。
宿主 descriptor 必须活到全部 App/config/surface 销毁、线程 join 和全局
释放之后；释放不是仍有 borrower 时可调用的 BUSY 探针。初始化尝试失败或
成功释放后均不可重试；NULL 参数拒绝不消费尝试，legacy 与 owned 初始化
也不能相互覆盖。释放后不得再调用依赖全局状态的 API。非 C main/tool 路径
保留原 allocator 选择。

`python3 scripts/probe-session-global-owner.py --mutations` 从干净钉版本
应用完整链，通过六类基线：冷态释放拒绝、成功/配置 borrower/全局回收、
首/次分配拒绝、真实容量不足、legacy 隔离。九项定向变异均为目标断言红、
独立对照绿，SHA256 回滚后六类复绿；编译失败和信号不能计作断言红。日志
`tmp/global-owner-verifier.log`。该脚本与 process/worker-surface 共用构建
路径，必须串行运行。另实测已有变异标记时预检返回 2；SIGINT/TERM 中断
实际冷缓存编译器，后代清空且三份源码 SHA256 复原，日志
`tmp/global-owner-harness.log`。

另复验 21 类原生、26 类旧 surface 和 19 组配置 API，日志
`tmp/global-owner-compat.log`；权威/VT 无 UI 框架链接，日志
`tmp/global-owner-linkage.log`。核心 gate 113 项/57 公共符号通过，日志
`tmp/global-owner-gate.log`。没有改宿主目录或公共 Ghosterm ABI，也没有重跑
完整进程变异全集；最近完整 320 项仍基于 `9331e42`。

该入口尚未接入 P0 GUI 的统一 pool。全局 I/O 工作线程的原生栈/guard，
借用的 argv/environ、signal/locale 副作用，以及第三方库与系统框架内部
分配仍未纳入完整账本；不能把这个 allocator 入口当作总预算已完成。
产品 daemon、公开生命周期/发现/恢复 UI、全局准入及其余 P0 条件仍待交付，
P0 保持未通过。下一步需要补全实际 GUI 准入接线与全局 I/O 线程容量策略。


### 2026-10-20：全局引擎在 GUI 同池准入与有序释放（P0 未通过）

进程原型现在先建立统一 pool，再以独立 8 MiB 租约调用 owned 全局初始化；
初始配置和 App 也在该准入之后创建。已有 surface、App/配置及其线程清理后
才结束全局状态，最后释放该域。旧 surface 模式保留既有一次初始化路径。
实验补丁 0042 提供实际 allocator 身份观测，owned 期间核验与租约一致，
冷态/legacy/释放后为零；该值不跨 IPC，不延长寿命。

私有 global owner 管理初始化失败回收与 close 的 BUSY 保留。五类租约基线
覆盖无效参数、成功、池满拒绝后释放额度重试、真实分配失败，以及尚有 raw
allocator 借用块时保留域、归还后再次 close。测试不授权带着存活的 Ghostty
borrower 调用全局 deinit。实际容量契约另外同时准入两块 surface 和一份
临时配置，所有进程级域一直持有；多一页必须拒绝，释放后承诺恢复。该测试
不复制 total 算式作为期望值，能检出漏算全局 8 MiB。

`python3 scripts/probe-session-process.py --global-owner-mutations` 的十五项
新变异通过目标断言红/独立对照绿，SHA256 回滚后 roundtrip/scheme 复绿；
日志 `tmp/global-integration-mutations-final.log`。一次冷态 panic 曾被脚本
拒绝计作有效红，目标改为释放后的身份断言后整组通过。预检遗留标记返回 2，
SIGINT/TERM 实际终止冷缓存编译器及后代，四份私有源 SHA256 恢复；日志
`tmp/global-integration-harness.log`。

另复验旧配置 28 项、旧全局生命周期 9 项和两条保留全局额度的旧总量变异，
日志 `tmp/global-integration-old-config.log`、`tmp/global-integration-old-global.log`
及 `tmp/global-integration-totals.log`。261 处工作源文字锚点按预期次数检查，
日志 `tmp/global-integration-anchors-final.log`。最终 21 类原生、26 类旧 surface、
19 组配置 API 与五类租约通过，日志 `tmp/global-integration-compat.log`；
元数据峰值 180224 字节，仍在 4 MiB 预留内。权威/VT 无 UI 框架链接，记录
`tmp/global-integration-linkage.log`；核心 gate 113 项/57 公共符号通过，记录
`tmp/global-integration-gate.log`。

进程变异集合现配置为 363，本轮没有跑全集；最近完整 320 仍基于 `9331e42`。
全局 I/O 工作线程的原生栈/guard、std.Thread 的 C allocator 入口参数帧、
框架/GPU/其余 TLS 与总预算仍未收口；真实系统输入法/焦点、任意主题/通用
绘制屏障和生产 listener/manager/auth/writer、公开生命周期/恢复 UI 与打包
仍待完成。P0 未通过，不能把全局 Zig allocator 接线当作完整 GUI 内存限额。


### 2026-10-20：全局 I/O 双限额、原生容量与宿主入口帧（P0 未通过）

全局 owned I/O 现在固定最多两条工作线程、请求栈 16 MiB。每槽实际观测为
16789504 字节栈和 16384 字节连续不可访问 guard，分别由 16809984 字节承诺
覆盖；两槽合计 33619968 字节在 GUI 同一 pool 中于初始化前预留。入口参数
和 256 KiB 备用信号缓冲走既有全局 8 MiB allocator 域，退出前恢复信号注册，
不再落到 std.Thread 的 C allocator/TLS 分支。

实验 0043 与 `patches/zig/0.16.0/0001-owned-io-threads.patch` 配套：构建准备
先从所选 Zig 读取固定 SHA256 的 Threaded 源及空测试 stub，再只在 tmp/ 的
隔离副本应用补丁。不改安装的 Zig 或 vendor，保留原任务/普通 I/O 实现，
四处线程创建改经 owned registry；legacy 分支仍可使用标准线程。
`prepare-session-io.py` 拒绝版本/三份输入哈希漂移、修改过的输出和符号链接
逃逸，并在生成源旁保留 MIT 许可证。守卫破坏后拒绝写入、独立控制仍绿，
日志 `tmp/owned-io-generator-final.log`。

Threaded.wait_group 只覆盖 worker 函数，结束时外层帧还可能未释放；全局
清理先结束 Threaded，再 join 全部实际 pthread，最后释放 general/native
两个租约。关闭竞争用例刻意阻塞退出回调，去掉真实 join、只伪造计数仍被
断言抓到。future/group 的完成通知也早于 busy_count 递减，连续复用原型曾
触发瞬时 unavailable；验证预热加入有界空闲屏障，普通 I/O 饱和语义不变。

`python3 scripts/probe-session-process.py --global-io-mutations` 的 27 项
定向变异均为目标断言红/独立对照绿；四条 async/concurrent/group 创建路径、
取消、真实帧拒绝/重试、400 次复用、关闭竞争与空闲模式九类基线及回滚通过，
日志 `tmp/owned-io-mutations-final.log`。独立 C 观测还检查实际 pthread、
allocator 身份、信号缓冲位于所选分配范围，以及释放时已解除注册。
标记预检返回 2，SIGINT/TERM 中断实际冷缓存编译器后后代清空、七份私有源
SHA256 复原，日志 `tmp/owned-io-harness.log`。

另复验旧全局 9 项、旧 owner 15 项、旧配置 28 项及两条保留 native I/O
额度的旧总量变异，日志 `tmp/owned-io-old-global.log`、
`tmp/owned-io-regressions.log`、`tmp/owned-io-totals.log`。最终 21 类原生、
26 类旧 surface、19 组配置 API、五类租约和九类 I/O 基线通过，日志
`tmp/owned-io-compat.log`；元数据峰值 196608 字节，仍在 4 MiB 承诺内。
权威/VT 无 UI 框架链接，日志 `tmp/owned-io-linkage.log`；核心 gate
113 项/57 公共符号通过，日志 `tmp/owned-io-gate.log`。

完整进程变异现配置为 390，本轮未跑全集；最近完整 320 仍基于 `9331e42`。
框架/GPU/其余系统及 TLS 开销、完整资源账本与性能校准仍未完成；真实系统
输入法/焦点、任意主题/通用绘制屏障和生产 listener/manager/auth/writer、
公开会话生命周期、恢复 UI 与 helper 打包仍待交付。P0 仍未通过，不把本机
P0 I/O 的证据扩展到其他平台或完整 daemon 能力。


### 2026-10-20：真实 PTY → VT 的直接/worker 性能基线（P0 未通过）

命令 `python3 scripts/probe-session-performance.py --rounds 3 --mutations`。
每个模式使用全新进程、同一真实 raw PTY 子程序和未修改的钉版本 VT；三轮
交替先后顺序。每轮核验 4 MiB 输出和 128 次输入往返的每个字节，再与独立
分块生成的 VT 快照逐字节比较。输入延迟从发送/提交到输出实际进入 VT 并被
观测；每次发送前错开 1–7 ms，避免只测刚读完的有利窗口。分位数为 nearest
rank。空闲期间 direct 也以同样 10 ms 有界读等待，不以主线程纯 sleep 冒充基线。

本机 macOS arm64、Zig 0.16.0、Ghostty `81598385d608dda4946093d6d6a234d204a2f225`；
probe `-O2`、Core Debug、VT ReleaseFast。三轮观测区间：

| 路径 | MiB/s | 输入 p50 ms | 输入 p99 ms | 空闲 CPU % 单核 | 峰值 RSS 字节 |
|---|---:|---:|---:|---:|---:|
| direct | 90.019–118.966 | 0.038–0.041 | 0.056–0.066 | 0.158–0.171 | 4177920.000–4194304.000 |
| worker | 74.398–82.800 | 6.049–6.064 | 10.563–10.845 | 0.159–0.173 | 4308992.000–4308992.000 |

worker 输入 p50 约 6 ms、p99 约 11 ms，与其每轮 10 ms backend read 等待吻合；
这是待处理的实测开销，不设未经校准的产品性能闸门。峰值 RSS 包含最终 oracle/
快照；空闲 CPU 仅计测量进程，不含 PTY 子进程。报告附平台、构建档、编译器和
实际二进制/库/输入 SHA256，位于 `tmp/session-performance/results.json`；完整
日志 `tmp/session-performance-final.log`。输出损坏、跳过 VT 应用、输入回显错误
三项变异均命中普通断言，同变异的独立 oracle 对照仍绿，私有源码 SHA256 回滚。
预检遗留标记返回 2；实际测量进程的 SIGINT/TERM 清理与源码复原另验，日志
`tmp/session-performance-harness.log`。核心 113 项/57 符号闸门通过，日志
`tmp/session-performance-gate.log`。

范围仅为当前后台 PTY/worker/VT 链路：未测现有 GUI full surface、生产 IPC、
渲染/显示延迟、多会话公平性或整应用内存；不能代替计划要求的最终进程内 GUI
对比、P3/P8 校准或完整资源账本。完整进程变异仍配置 390，本次未重跑；此三项
为独立性能探针变异，不并入该计数。P0 与生产 daemon 仍未完成。


### 2026-10-20：worker 命令通知唤醒 PTY 读取（P0 未通过）

worker 提交命令/停止时在同一队列锁内写自有非阻塞管道，owner 在取命令前
排空通知并在读取前检查剩余队列；已有命令时 read timeout 为 0。私有 backend
另有可选 `read_with_wake`：PTY 将借用的通知 fd 加入 poll，不消费或关闭它，
不改变 EOF/PTY 生命周期；同时就绪时先读一块输出，下一轮继续处理命令，防止
连续命令把输出饿住。其他 backend 未提供该接口时保持原有 10 ms 有界 read，
不宣称有唤醒能力；PTY 也保留该上限，继续驱动积压输入的 flush。

新管道两端 CLOEXEC/NONBLOCK，直到 join 才关闭；创建/属性设置/线程创建失败
均清理。每 worker 多两枚 fd，worker 结构仍由原 allocator 域计费，管道的
内核资源等仍须在完整资源账本中收口。跨线程提交只通知 worker，不调用 backend。

`python3 scripts/mutation-check-worker-wake.py` 的十二项变异均为目标断言红、
同变异独立对照绿；六类 C 基线包含通知早于实际 poll、stop、已有两条命令不再
等待、无唤醒接口、真实 PTY 不消费通知/持续通知下输出前进，以及仅剩一个 fd
槽位时启动失败且不关闭调用方 fd。八次循环关闭后 fd 数回归，最终 SHA256
回滚；日志 `tmp/worker-wake-mutations-final.log`。遗留标记预检返回 2，
SIGINT/TERM 中断实际编译器后后代退出、三份私有源码复原，日志
`tmp/worker-wake-harness-final.log`。既有 worker 十七项变异复绿，日志
`tmp/worker-wake-old-mutations.log`。

相同命令 `python3 scripts/probe-session-performance.py --rounds 3 --mutations`
最终三轮观测如下（环境/口径与上一条后台基线相同）：

| 路径 | MiB/s | 输入 p50 ms | 输入 p99 ms | 空闲 CPU % 单核 |
|---|---:|---:|---:|---:|
| direct | 101.158–180.424 | 0.027–0.033 | 0.060–0.066 | 0.108–0.170 |
| worker | 110.354–154.494 | 0.027–0.043 | 0.085–0.106 | 0.066–0.132 |

此前 `40d3710` 中 worker p50 为 6.049–6.064 ms、p99 为 10.563–10.845 ms。
本次不据三轮吞吐波动设产品 SLA。原始结果/输入和实际库 SHA256 已保留在
`tmp/worker-wake-performance-results.json`，日志 `tmp/worker-wake-performance-final.log`。
输出损坏/跳过 VT 应用/输入回显错误三项性能探针变异另全部复绿。

核心 gate 119 项、57 公共符号通过（`tmp/worker-wake-gate-final.log`）；最终
21 类原生恢复、26 类旧 surface、19 组配置 API、五类全局租约与九类 I/O
基线通过（`tmp/worker-wake-native-final.log`）。公开 `ghosterm.h` 与平台绑定
未变。此切片未重跑完整 390 项进程变异，也不将独立十二项变异并入该计数。
完整 GUI/IPC/渲染性能、系统/GPU/TLS/全局资源账本及生产 P0–P8 仍未交付。


### 2026-10-20：私有多会话注册表与有序回收（P1-c 基础，P0 未通过）

`core/src/session/manager.{h,c}` 新增不依赖引擎/UI/协议的固定容量注册表，
最多 64 个槽；创建中、实际 join 中和保留的终态记录均占槽。成功创建才接管
调用方 context，worker join 后才调用 disposer；创建失败清零返回 ID、不接管
context，并归还槽位。ID 在单个 manager 生命周期内单调分配、不复用；它不是
生产协议的全局/认证 ID。worker 仍独占 backend/state，注册表不泄漏裸指针。

按 ID 提交、查询、全表快照、显式 terminate、reap、forget 已有实现。列表
容量不足整份不写；终态错误、退出信息和 sequence 的已知位原样保留，不把
尚未就绪折叠为成功。reap 先缓存实际结果，在锁外真实 join/销毁 context，
终态记录继续占容量直到 forget。创建/reap 在途时 close 返回 BUSY、保持可用；
正常 close 先请求所有 worker 停止，再逐一 join/销毁，最后释放注册表。
它不根据客户端数量隐式结束会话，也没有建立新的 attachment/恢复协议。

六类 C 基线验证真实 PTY 连续交互、自然 EOF、显式结束、保留记录/旧 ID 隔离、
两种异步工厂失败及未知退出信息、精确分配域/拒绝回滚、八线程争抢两槽、
阻塞最后回调时并发 reap/close，以及创建 allocator 屏障内的容量与 close。
`python3 scripts/mutation-check-session-manager.py` 十九项变异均达到目标断言
红、同变异对照绿、SHA256 回滚及最终六类基线复绿；日志
`tmp/session-manager-mutations.log`。遗留标记返回 2，SIGINT/TERM 中断真实
变异编译器后退出并复原三份私有源码，日志 `tmp/session-manager-harness.log`。

manager/worker/state-owner/PTY 和测试五份 C 源分别以 ASan+UBSan、TSan 插桩，
各六类用例退出 0；其余静态 core 依赖未插桩，不扩展为全 Core 覆盖。二进制、
源/库 SHA256 与退出码见 `tmp/session-manager-sanitizers/results.json`，逐例日志
位于同目录。完整核心闸门 125 项、57 公共符号通过，日志
`tmp/session-manager-gate.log`；公开 ghosterm.h/平台绑定未变。

这只是可复用的私有注册表基础：尚未组合进生产 daemon/runtime 或公开 API；
attachment/快照任务 owner、TTL/发现元数据、统一原生资源准入、listener/auth、
有序 writer、产品恢复与打包仍待接线。本轮未重跑完整 390 项进程变异，十九项
为独立 manager 变异，不并入该计数。P0 未通过，生产 daemon 能力仍标 ❌。
