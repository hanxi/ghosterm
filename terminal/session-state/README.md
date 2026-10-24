# 会话终端状态：P0 探针

此目录当前只有设计验证探针，生产适配器尚未实现。原生 surface 的引擎分档不变。

探针使用 pin `81598385d608dda4946093d6d6a234d204a2f225` 的原始 libghostty-vt C API，
不链接 AppKit、GTK、Metal 或核心库；`pty` 用例另创建真实 POSIX PTY。因此它验证的是无 UI 状态引擎的一部分能力，
不能证明 full surface 恢复或 daemon 生命周期已经工作。

## 运行

需要 C11 编译器、Python 3、Zig **0.16.0** 和已初始化且无源码修改的 `vendor/ghostty`。
第一次构建需要网络获取上游校验过哈希的依赖。脚本检查 pin、跟踪文件是否干净及 Zig 版本，
每次调用先运行引擎增量构建再编译探针，不直接复用未经核验的旧探针二进制。

```bash
git submodule update --init vendor/ghostty
ZIG=/absolute/path/to/zig python3 scripts/probe-session-state.py
ZIG=/absolute/path/to/zig python3 scripts/probe-session-state.py --case continuation
ZIG=/absolute/path/to/zig python3 scripts/probe-session-state.py --case effects
ZIG=/absolute/path/to/zig python3 scripts/probe-session-state.py --case corruption
ZIG=/absolute/path/to/zig python3 scripts/probe-session-state.py --case pty
ZIG=/absolute/path/to/zig python3 scripts/probe-session-state.py --case resources
ZIG=/absolute/path/to/zig python3 scripts/probe-session-state.py --mutations
```

`ZIG` 未指定时先查 PATH，再查本机已下载的 `tmp/zig/zig-aarch64-macos-0.16.0/zig`。
其他架构显式指定自己的 Zig。构建缓存、库、探针和变异副本全部放工程 `tmp/`，不修改上游源码。
当前运行证据仅来自 macOS arm64；其他平台需要各自原生验证。

| 用例 | 实际验证 | 当前退出码 |
|---|---|---|
| continuation | CSI/OSC/UTF-8/ESC/DCS/APC 六类未完成输入，恢复前后快照逐字节对比，补齐序列后的状态对比；含主/备用屏幕、历史、超链接、模式、宽/组合字符及 resize | 0 |
| effects | 权威 VT 实例响应一次 DSR，恢复后的 VT 副本不重复响应，二者状态一致 | 0 |
| corruption | 损坏快照 CRC 和截断 FINISH 均被 decoder 拒绝 | 0 |
| pty | 真实控制终端中的最小 TUI；半截 CSI 快照、离线期间的查询应答、实读增量重放、主备用屏幕切换及子进程观察到的 51×17 resize | 0 |
| resources | 原实例确实存有 inline RGBA 图像，恢复后相同 ID 消失 | 3（已确认限制） |

无参数运行或运行 resources **返回 3**，表示发现阻止准确恢复的缺口。
环境/构建失败或断言失败返回 1，变异标记残留/无效参数返回 2。
前四组用例的 0 只表示对应局部验证通过，不能用于宣传完整会话恢复。

`--mutations` 是测试有效性验证：分别制造补续字节丢失、重复应答、吞掉 decoder 错误、
图像源夹具缺失，以及 PTY 增量丢失、副本重复应答、resize 未传给子进程。
每次变异独立编译，目标用例失败，同一变异下另一组对照仍通过，
回滚后重新编译并恢复原结果。七组通过时脚本返回 0，仍明确报告 P0 未完成。
脚本起跑前扫描标记；INT/TERM 触发清理；只修改临时副本，复原后逐字节比对，
退出时核验仓库源文件与起跑前 SHA256 相同。

PTY 用例只验证精确握手边界处的 VT 状态恢复；输出以小块读取，副本停收期间仅权威
实例应答，再将实际读取的有界增量送给副本。子进程检查控制终端、前台进程组及真实
窗口尺寸，读取等待有超时，失败时回收子进程。它没有使用生产 session/attachment、
IPC 或 full surface，也没有运行 vim/tmux 等外部 TUI，更不能证明 GUI 崩溃重连。

## 尚需补齐

macOS arm64 的 full surface 隔离原型另运行：

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface.py --mutations
```

需要完整 Xcode 与 Metal toolchain。脚本从干净 pin archive 应用现有补丁与
`experimental/0004-replica-surface.patch`，独立构建 full Metal 库，不覆盖生产
xcframework。VT 夹具与 full surface 不在同一进程，不互传内部指针。
ground 和六类 continuation 验证构造时导入、规范状态、禁用查询/剪贴板/通知回调及
保留 bracketed-paste 输入。额外 `pty` 夹具来自真实 TUI 子进程：GUI 不存在期间由 VT
权威实例应答查询，实际读到的增量与快照进入 full surface，恢复状态与权威实例对拍。
另起系统 Vim（关闭用户配置、插件、viminfo 和 swap），在真实 PTY 中输入测试文本，
从 VT 网格确认编辑成功，再将快照和实读增量导入 full surface 对拍。
tmux 夹具使用独立 socket、空配置和真实 `/bin/cat` pane，检查输入已进网格，
捕获快照/增量供 full surface 对拍；客户端 detach 后关闭该临时 server，
通过 `has-session` 确认 server 已停止，而不依赖 socket 文件是否立即删除。
需要 PATH 中存在 tmux，或用 `GHOSTERM_P0_TMUX` 显式指定可执行文件。
完整八组变异覆盖续接、应答隔离、用户输入、PTY/Vim/tmux 增量、Vim 实际编辑及 tmux server 清理。
其中五组夹具变异可单独用 `--fixture-mutations` 运行，仍先构建实验库并跑完整基线。
默认完整运行仍返回 3；`--case state|effects|input|pty|vim|tmux` 与成功的变异验证返回 0，
只表示对应局部证据成立。构建日志在 `tmp/session-surface-build.log`。

此原型没有运行中 terminal 替换、生产 barrier、图像恢复或完整内存预算；
只读 effects 也不能单独证明图像文件装载等外部路径已经隔离。

分配与故障可观测性另由以下探针验证，同样使用精确 pin 的原始 VT 库：

```bash
python3 scripts/probe-session-allocation.py --mutations
```

该探针注入标题更新分配失败，检查 sticky `VT_PROCESSING_ERROR` 在 reset 后仍保留；
在正常解码及预算耗尽后检查 callback 分配回收、失败输出 handle 为 NULL。
四组变异验证错误隐藏、预算绕过、decoder 清理遗漏及 reset 丢失失败状态，均有独立对照。
预算只包含 C allocator 请求的字节，**不含原生直接 mmap/VirtualAlloc 的终端页面**，
不能用于宣称完整会话内存已受限。源代码与变异均通过 `-Wall -Wextra -Werror` 编译。

资源 I/O owner 另验证：

```bash
python3 scripts/probe-session-resource-io.py --mutations
```

权威实例执行 Kitty 临时文件装载会消费删除文件，同一原始命令不能由副本再次装载；
即便规范快照相同、语义失败标记为 false，图像也可能不同。原始重放用例退出 3。
inline 对照和受控的“从权威读取真实像素后送给副本”对照返回 0，独立检查像素和 placement；
后者不是通用流重写器。四组变异验证源夹具、真正重放、像素和位置，均有对照及回滚。
默认完整运行仍返回 3；只有指定成功的局部用例或成功的变异验证返回 0。

引擎侧资源 I/O hook 原型另运行：

```bash
python3 scripts/probe-session-resource-hooks.py --mutations
```

该脚本在原始 pin 的隔离副本中仅应用实验 0005，构建无 UI VT 库。
真实临时文件由权威实例读取并消费；副本通过 resolver 消费结果，不允许本地文件回退。
成功、源读取失败、observer 拒绝、路径不匹配、重复消费与普通文件六类用例通过；
四组 native 变异均目标红、同库 hook 对照绿、回滚复绿。
默认完整运行仍退出 3；`--case` 或成功的变异验证只代表局部通过。
该 hook 传递原始外部载荷/读取拒绝，尚未同步最终图像解码和配额结果；
还不是生产资源协议或 full surface 接线。详情见实验补丁的 WHY.md。

加 `--builtin-png` 叠加实验 0006，以 full 引擎的真实 Wuffs decoder 处理 PNG。
`--png-mutations` 验证真实解码、坏 PNG 的零输出与分配失败回收（三组变异）；
普通文件用例在权威读取后覆盖源内容，副本仍应使用已捕获的原像素。

完整边界与决策记录见 [会话运行时 ADR](../../docs/architecture/session-runtime.md)。
下一步必须解决图像/placement 恢复、full surface 导入、全量应答和宿主副作用隔离、
一致快照的捕获开销以及失败可观测性，再验证持续输出、其他外部 TUI 与 GUI 崩溃重连。

full Metal 的资源 resolver 原型（独立实验库）：

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-resources.py --mutations
```

生产补丁与实验 0004–0007 只应用到 `tmp/` archive，不改 vendor 或生产 xcframework。
无 UI 权威实例先消费临时文件；full surface 接收原始增量，在引擎装载点取已捕获载荷。
RGBA、真实 PNG 与源失败分别检查像素/位置、规范状态、输入和释放。
缺失/重复事件必须明确失效；三组变异验证载荷、真实位置和失效可观测性。
默认退出 3；成功变异仅代表局部证据，不包含 checkpoint 内既有图像或生产重连。

原始资源事件的最终配额分歧可单独复现：

```bash
python3 scripts/probe-session-resource-budget.py --mutations
```

权威额度 3 字节、副本额度 4 字节，读取同一份 4 字节 RGBA：读取事件成功、路径仅消费
一次、规范状态相同、两边语义错误标记均为 false，但权威拒绝图像而副本成功。
两边额度同为 3/4 的拒绝/成功对照均通过。三组变异护源拒绝、副本差异与真实像素，
默认和 `--case divergent` 返回 3，成功变异或一致额度的局部用例返回 0。
不能把成功传递原始载荷当成最终资源提交一致的证据。

显式导入源图像额度并验证 GUI 配置重载：

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-resources.py --quota-mutations
```

该分支加实验 0008：resolver 构造必须提供源额度，先于 screen 解码应用，重载后保留
额度及 resolver，并保持本地资源读取禁用。4 字节图像在源/副本 3 字节额度下均拒绝。
四组变异验证上述边界；其他配置字段的权威同步和最终提交一致性仍未解决。

checkpoint 内静态资源的无 UI 编解码实验：

```bash
python3 scripts/probe-session-resource-snapshot.py --mutations
```

实验 0009 叠加在 0005–0006 后，按有界、显式字段编码 RGB/RGBA、两张 screen、
pin/virtual、ID 计数器及淘汰顺序；CRC/preflight 与临时资源图防止错误输入部分发布。
loading、动画、pending 和 relative 明确未支持，不能悄悄丢弃。
默认退出 3；成功局部用例和变异只证明静态子集，非生产恢复事务。
`--case oom-sweep` 遍历四个实际分配失败位置，验证未发布、重试及最终回收；清理遗漏另有定向变异。

full Metal 在构造时导入 checkpoint 静态资源：

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-resources.py --snapshot-mutations
```

实验 0010 在启动线程前导入 0009 的资源图，先核对源额度；在处理增量前独立验证
已有 RGB/RGBA/PNG 及双屏 pin，随后处理真实资源增量并返回 alternate 对拍。
拒绝构造的路径也验证 App 能释放；四组变异覆盖导入、额度、缓冲区和字体引用回滚。
这不包含 loading/动画/relative 或生产重连，默认仍退出 3。

relative placement 的无 UI 资源编解码实验：

```bash
python3 scripts/probe-session-relative-resources.py --mutations
```

实验 0011 保存父 placement 的完整键与有符号偏移，支持多层链和父节点后置；
先验证缺失父节点、循环及深度，再分配/发布。夹具检查坐标和删除父节点后的清理，
内部 ID 不折叠为外部 ID；四组变异要求目标红、独立对照绿及回滚复绿。
默认退出 3；尚缺 full surface relative 恢复证据、loading/animation 和生产恢复事务。

full Metal 构造时 relative 资源图恢复实验：

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-relative.py --mutations
```

实验 0012 导出 surface 的实际资源图；五类夹具包括静态对照、相对父链、多层、
父节点后置和内部 ID。在增量前、绘制调用后、删父节点后分别比较资源图，同时比较
canonical 状态并检查副本没有应答或宿主副作用。四组变异护偏移、命名空间、GUI
提前 reflow 和删除。GUI resize 仅提出请求，逻辑网格保持权威尺寸；有序接受/回传
尚待实现。import-only 对照不运行 App tick，以隔离 resize 保护变异。
这不是 relative 的像素验收；也未证明一致 checkpoint、动画或生产重连，默认退出 3。

权威 output/resize 的有序同步应用实验：

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-events.py --mutations
```

实验 0013 在同一状态锁内应用连续序号的事件，再发布 applied sequence。GUI 仍
只请求 resize，显式权威 resize 才改变逻辑网格。六类用例验证输出、resize、交错
序列、错误顺序拒绝、原始输出绕过后的失效及初始对照；逐步对拍 canonical 与资源
图，并在模式事件应用后检查真实输入字节的 bracketed-paste 开/关。
七组变异覆盖实际应用、校验/发布序号、原始绕过、输入模式与非法尺寸假成功。
序号还没有 attachment generation 或 checkpoint 绑定，输入请求尚未携带已应用
序号，resize 失败的分配注入尚待补；这不是生产 barrier/恢复事务，默认仍退出 3。

checkpoint 成对绑定与 generation 隔离实验：

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-checkpoint.py --mutations
```

实验 0014 将 canonical、资源图、generation、起始序号和源额度纳入独立 Python
SHA256 描述，full surface 构造时验证；错误组合必须拒绝，合法蓝色资源重算摘要
后必须接受。恢复序号从指定的 40 继续，事件和查询必须携带匹配 generation；
旧实验入口不能绕过绑定实例，bound sequence=0 也禁止原始输出绕过。
六类用例与八组变异验证上述边界，均带独立对照和回滚。
该证据限于 surface 侧校验；没有真实 attachment 生命周期、身份认证或并发原子
捕获，输入尚未标记 generation/已应用序号。总内存、动画和生产恢复仍待完成，默认退出 3。

输入编码点元数据与异步队列实验：

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-input.py --mutations
```

实验 0015 让普通键、延迟按键序列、粘贴分片、鼠标和原始绑定保存编码时的
generation/sequence/linefeed。探针在编码后、排队前推进模式，核验旧编码字节仍标记
旧序号；Host 按保存的 linefeed 分片转换，每片保留元数据。缺失回调、未标记或
非法标记写入会使副本显式失效，不回落到无标记回调。
十五类原生用例、十五组 native 变异加两组上游单测变异验证上述边界、72 字节消息
预算和只读释放；每组带独立对照及回滚。测试 hook 与注入 API 均不属于生产接口。
本探针的完整引擎缓存位于本轮临时目录，结束后清理，避免变异静态库跨轮累积。
尚无 daemon worker/IPC 校验、原子捕获、全量宿主授权动作或真实 GUI 接线，
loading/动画和总内存仍待完成，默认退出 3，P0 保持未通过。

GUI reset 的权威动作实验：

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-reset.py --mutations
```

实验 0016 修复 bound GUI 的 reset 直接改 VT 模式却不推进序号的问题。GUI 只提交
generation/已应用序号/动作种类的低频请求；回调缺失或拒绝时保持原状态，收到
有序权威 reset 事件后才修改模式并发布新序号。回调在锁外，可同步查询/应用事件。
七类用例对拍无 UI reset 前后的 canonical/资源图、实际输入字节与序号，含半截 CSI
延续、旧 generation 和未绑定对照；七组变异逐项证明目标红、独立对照绿与回滚。
请求不得泄漏为终端输入字节。clear_screen、配置等其他动作、真实 worker/IPC、
原子捕获、loading/动画与总内存预算仍待完成，默认退出 3，P0 保持未通过。

## 权威清屏与页面删除修复（0017）

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-clear.py --mutations
```

GUI bound `clear_screen` 只请求 kind=2；权威有序事件携带 history/outcome，副本验证
实际结果后才推进序号。提示符的 FF 重绘只由无 UI authority 发送，副本不写。
夹具区分活动区图片删除与历史图片保留，比较完整 canonical/资源图；覆盖备用屏幕、
拒绝、缺失回调、旧 generation、结果分歧及旧未绑定行为。

该实验复现并修正 pin 中共享历史页的 `eraseActive` 断言失败。单页/跨多页的新回归
及 `PageList erase` 的 103 项既有回归、十类原生用例与十组变异均已通过最终回滚
复验。仍没有真实 worker/IPC、完整恢复事务，默认退出 3，P0 仍未完成。

## 图像分块传输中的快照（0018）

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-loading.py --mutations
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-loading.py --mutations
```

实验资源格式升级为 GTIR0003，将两个 screen 各自的 LoadingImage 与资源图一起
preflight、暂存、原子发布；保留分块数据、压缩、初始 display、quiet、应答和隐式 ID。
无 UI 基线覆盖原始/zlib/PNG、无 display、半截 APC、双屏、隐式 ID 后续分配、非法
枚举、线长上限和分配失败回收/重试，并明确拒绝动画帧 loading。十一类 full surface
基线已通过，含覆盖同 ID、其他图片并存、隐式 ID 续传及无 loading 对照；十一组
无 UI/四组原生变异及最终回滚复验均已通过。此证据仍局限于隔离引擎，P0 未完成。

旧资源实验按其各自补丁版本继续独立运行；0018 不解码 GTIR0002。尚无动画/pending
恢复、并发原子 checkpoint、会话总预算与生产 daemon/IPC，默认仍退出 3。

## 加载记录的严格 preflight（0019）

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-loading-validation.py --mutations
```

拒绝上游正常加载不可能产生的零图片 ID。十三类非法记录各自重算 CRC，在已有
完整图片/placement 后检查错误、零 allocator 调用及目标状态不变；含静态独立对照。
基线、补丁后的 full surface direct 控制及五组变异/最终回滚复验均通过。
日志为 `tmp/session-loading-validation-build.log`，默认仍返回 3，P0 未完成。

## 动画 checkpoint 与权威 tick（0020）

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-animation.py --mutations
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-animation.py --mutations
```

GTIR0004 保存动画帧、gap/状态/当前帧、循环计数/预算及最后权威 tick。无 UI 基线
已通过运行/暂停/加载态、有限循环、gapless、根帧/未放置、continuation、计数饱和、
畸形记录拒绝及八处分配失败后的回收/重试；十类原生用例、十一组无 UI/五组原生
变异及最终回滚复验全部通过。副本等候期间保持动画状态，只经有序 tick 推进，
倒退时间和旧 generation 均明确拒绝。P0 默认仍返回 3。

## 未完成动画帧（0021）

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-frame-loading.py --mutations
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-frame-loading.py --mutations
```

GTIR0005 保存 FrameContext 命令与捕获时资源引用是否仍有效，导入后只重绑有效引用。
支持按编号引用的原始零 ID，保留编辑/合成/背景/gap/quiet/应答与解析器 continuation。
二十八类无 UI 基线、九处分配失败回收/重试及十四组变异/最终回滚通过；十五类
原生帧续传用例及六组原生变异/最终回滚全部通过。默认退出 3，异步 pending、原子捕获、总预算与生产重连仍未完成。

## 异步 pending 资源（0022）

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-pending.py --mutations
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-pending.py --mutations
```

GTIR0006 区分 pending 的预留长度与 complete 像素，保存 authority producer token，
恢复后仍使用独立的本地 renderer generation。生产者完成、取消及晚到结果显式
比较 applied 结果；有序事件验证 attachment generation 和连续 sequence。
无 UI 用例覆盖 RGB、编号、资源替换/删除、重复完成、帧续传、错误长度/零 token、
额度及静态对照。异步用例在另一个 pthread 中执行真实 PNG 解码，捕获/导入或取消
后才释放条件变量，检查解码分配全部回收；线程只产出像素，Terminal 仍归 owner。
原生夹具刻意让权威 token 与 GUI 缓存 generation 不同，避免身份碰巧相同导致假绿。

这是引擎隔离实验，正常 stream 解码器尚未异步化；pending 事件当前作用于活动
screen，跨 screen 的生产者路由尚待明确实现。没有真实 worker/IPC、原子捕获、
完整总预算或 GUI 崩溃重连。默认仍返回 3，表示 P0 未通过。

本切片十六类无 UI、九类原生用例通过；五组无 UI 与四组原生变异、SHA256 回滚
检查及最终全量基线复验通过。pending 专项分配失败、畸形记录和跨 screen 路由仍待补齐。

## pending 生产者跨 screen 路由（0023）

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-pending-screen.py --mutations
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-pending-screen.py --mutations
```

完成/取消按 authority origin 查原 screen，再校验图片 ID，不切换当前 screen。
两屏可拥有相同图片 ID，但不允许复用存活的 producer token；重复 origin 的资源
记录以正确 CRC 提交，必须在零 allocator 调用下拒绝并保持目标状态不变。
用例覆盖主/备用屏切走后的完成、取消与晚到结果，以及双屏分别完成和原屏帧续传。

此切片补齐 0022 的活动 screen 限制；pending 专项 OOM/其余畸形记录、真实生产者
调度、原子捕获、总预算和生产恢复链路仍未交付，P0 仍未通过。

运行结果：九类无 UI、七类原生用例与 0022 十六类回归通过；三组无 UI、两组
原生变异均完成独立对照、SHA256 回滚及最终全量复验。默认仍返回 3。

## pending 分配失败与畸形记录

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-pending-failure.py --mutations
```

此探针使用 0023 的既有补丁链，不新增引擎接口。主屏 pending 带未完成动画帧，
备用屏另有同 ID pending。导入时逐一拒绝每个 allocator 分配，检查未发布半份资源、
可在同一目标重试、旧 producer token 仍能完成原屏图像/帧，并在销毁时回收全部分配。
像素提交单独注入 OOM，要求明确错误、applied=false、资源不变且可以重试。
重复完成、过期取消与有效取消均不得分配内存。

十二类正确 CRC 的 pending 畸形记录覆盖 kind、token、complete/token 冲突、短/超长
长度、宽度、ID、格式、flags、预留总量、限额及 animation 标记。目标双屏预先存有
图片，预检须以零 allocator 调用拒绝，并保持 canonical 与资源字节不变；这还区分
了错误记录和目标已有资源的 UNSUPPORTED 状态。

allocator 描述符及计数器活到 terminal 销毁；故障注入只覆盖目标操作，观测快照时
撤销注入，避免把探针自身分配失败误判为引擎错误。本证据不包含原生 surface 的
分配失败、OS page/mmap 预算、并发原子捕获或生产恢复事务，P0 仍未通过。

运行结果：十五类基线通过，导入覆盖八个失败分配点；四组变异均有目标红/独立
对照绿、SHA256 回滚验证与最终全量复验。核心 gate 9/9、57 个公开符号及快速
构建通过。默认返回 3，表示 P0 未完成。

## 原生页面供给与额度（0024）

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-page-provider.py --mutations
```

显式创建/解码选项将 OS 页面分配接到宿主 page allocator；其返回值必须为零填充、
页对齐且可独立 decommit/recommit 的原生映射，不能使用普通 malloc。allocator
描述符及 context 要活到派生终端与克隆全部销毁。此策略不写入快照。

C 探针用 mmap/munmap 计费完整映射页，含预热池容量；验证精确额度、少一字节失败、
逐页 OOM 回收、解码后选项冻结、默认策略、备用屏继承和恢复后的备用屏继承。
Zig 测试验证克隆共用页面供给器、拒绝不绕过预算、分配字节在销毁时全部回收。

这只验证原生页面接缝，尚非完整内存预算。full surface 的自定义预算接线、一般
分配/资源/快照副本的总额度、resize 失败事务、并发捕获和真实 worker/IPC 仍缺失。
P0 仍未通过；完整运行默认退出 3，单用例或变异验证通过可返回 0。

0024 运行证据：八类无 UI 基线、十六个页面 OOM 点与克隆定向测试通过；六组
变异均完成目标红/独立对照绿、SHA256 复原及最终全量回滚复验。默认 full surface
七类恢复回归通过，核心 gate 9/9、57 个公开符号和快速构建通过。

## 共享 POSIX 额度与故障隔离

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-shared-budget.py --mutations
```

`budget-posix.c/.h` 是私有 P0 适配器，依赖实验 0024 的 C 页面接口，不属于正式
`ghosterm.h`。一般分配、OS 页面与临时缓冲区可用不同 allocator 入口共享同一
全局账本，每个 owner scope 另有局部额度和不可清除的首个故障。预算拒绝影响
申请者，不污染其他会话；增加额度也不会复活已失效的 scope。

本实现对每次分配使用 mmap，计费 OS 页粒度及对齐所需的临时额外映射，包含
预热池和小分配的页内余量。resize/remap 优化不提供，调用者正常回落到新分配，
峰值因此包含新旧缓冲区同时存活。分配和回收计费受 mutex 保护；统计 live 包含
已预留、尚在映射的字节。该保守实现尚未作生产性能校准，计费数不是 RSS。

allocator 描述符和 context 地址固定并活到所有借用者销毁。close 只允许 live=0；
宿主还须先停止任务并等回调结束。释放在故障后仍允许。feed/resize 在操作前后
检查资源故障，故障后的输入不再进入引擎；其他引擎操作必须经过
`p0_budget_result` 检查，不能只看引擎自身的 success 返回。

十二类基线覆盖类别合计、全局/局部上限、故障保持、对齐与溢出、真实终端与
双屏图片恢复、像素/切屏/resize 失败、新会话与 scratch 拒绝不杀已有会话，
以及八线程争抢四份额度。十一组变异及最终全量回滚通过。

source 编码器当前仍使用终端的一般 allocator；本切片的 scratch 用例只验证临时
缓冲区的分配准入，未证明快照编码失败不污染权威状态。快照专用 allocator、
不可变/原子捕获、full surface 预算、真实 worker/IPC 与性能校准仍缺失，P0 未通过。

## 快照 scratch scope 与成对回滚（0025）

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-scratch-checkpoint.py --mutations
```

新 canonical/资源分配接口把 continuation、编码 scratch、压缩页临时副本和最终
wire 缓冲区都交给任务 allocator。私有 `checkpoint-budget` 在同一共享账本的独立
scratch scope 中构造两份输出；失败全部回滚，成功才交出完整 pair，并检查合计
wire 字节上限。描述符必须活到 pair release；调用者传入空的输出对象。

探针将 source scope 的额度冻结在当前 live 上，使任何意外源分配都立即可见。
覆盖半截 CSI/UTF-8、资源分块加载、压缩历史、两阶段失败和 wire 上限；成功数据
与旧编码器逐字节相同，失败后 source 的分配计数/表示不变且仍可输入。额度扫描
逐 OS 页增加任务限额，不等同于每个分配调用的独立 OOM 扫描。压缩基线要求平台
实际支持压缩，不将缺失能力当作通过。另有 Zig 单测强制压缩活动页，拒绝源分配
后仍须能编码/解码并保持源压缩状态。

这仍是同步捕获，调用者独占源直到两份编码结束。尚无 COW/原子切片、worker/IPC、
原生 GUI 预算或性能校准。0025 初版当时只有按需 scope，尚未保护活动会话的增长
余量；后续预留模式见下节。P0 未通过。

0025 运行证据：十四类无 UI 基线与压缩活动页定向测试通过；五组任务额度阶梯
扫描覆盖 canonical 和 resources 两阶段拒绝，未发布半份输出。十组变异均完成
目标红/独立对照绿、SHA256 复原及最终全量回滚复验。默认 full surface 七类恢复
回归通过；核心 gate 9/9、57 个公开符号及快速构建通过。

## 活动 owner 的峰值额度预留

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-reserved-budget.py --mutations
```

`p0_budget_scope_admit` 在执行任务前一次性承诺整个峰值额度；失败留下空的故障
scope，调用者关闭后丢弃。`committed` 是所有已接纳 scope 的 grant 加上按需 scope
的实际 live。任务准入和按需映射只用未承诺容量；已接纳 owner 在自己的 grant 内
分配不再重复争抢全局余量。释放映射不归还 grant，直到 scope 关闭或显式缩额。

扩额必须取得剩余承诺容量，失败保留旧额度且不污染健康 owner；缩额不得低于
实际 live。总上限也不能低于 committed。它保证预算不被其他 owner 抢占，不是
预分配或锁定物理 RAM；OS 分配失败仍显式报告。resize/对齐的瞬时双份映射同样
属于声明的峰值额度。checkpoint 只接受同一总账下两个已接纳的 source/task scope，
调用者必须将 terminal 的一般/页面 allocator 都绑定到该 source。

十类基线与十二组变异/最终回滚通过，覆盖并发准入、按需混用、grant 增减/归还、
角色校验，以及检查点缓冲区仍存活时真实终端 resize/继续输入，检查点字节不变。
原共享额度十一组变异、十四类快照回归及核心 gate 9/9/57 符号对拍通过。
这仍是 POSIX 隔离原型，未交付并发原子捕获、原生 GUI 接线、生产 worker/IPC 或
性能校准，P0 未通过。

## 不可变捕获与后台编码（0026）

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-frozen-checkpoint.py --mutations
```

owner 独占源时，捕获 canonical 元数据、continuation、页面与资源的独立副本；
逐页 wire 编码、压缩页解压及资源 CRC 在后台完成。压缩页捕获保留其压缩表示，
不触碰源映射。冻结对象不依赖源 Terminal、节点、字符串或像素的生命周期；它
自身与 allocator 描述符必须活到所有编码调用结束。

这不是上游 Screen.clone（它不保留完整恢复状态），也不是 COW。副本、后台临时
分配和输出各自使用显式 allocator，可用已接纳 scope 计费。copy 上限约束捕获
数据量，wire 上限约束最终两份数据的合计长度；错误输出为 NULL/0，允许新任务
重试。先取消、尚未编码的对象可直接释放；编码中的对象须等编码结束后释放。

探针在真实 encoder 分配回调处设确定性屏障，让源继续输出、重置并销毁，随后
检查编码字节与旧同步接口一致。覆盖 Unicode/组合字/ZWJ、超链接与标题/cwd、
半截输入、分块图片、压缩历史、16 MiB 图像、两阶段额度失败及取消回收。输出
`owner_ms`、`capture_peak`、`encode_peak`；本环境单次 16 MiB 样本 owner 约
1.17 ms，不是最坏延迟或性能承诺，仍需压力测试校准。

尚未交付真实 worker 事件 S 边界、任务取消调度、原生 GUI 预算和 IPC 恢复事务，
P0 仍未通过。完整基线默认返回 3，变异或指定用例验证通过可返回 0。

0026 运行证据：十七类无 UI 基线通过，含双屏同 ID 图片及原屏/备用屏历史顺序；
四组捕获/编码额度阶梯扫描验证回收与重试。十一组定向变异均完成目标红/独立
对照绿、SHA256 复原和最终全量回滚基线。默认 full surface 七类恢复回归、核心
gate 9/9、57 个公开符号及快速构建通过。

### 真实 worker 的 libVT 冻结边界

运行 `python3 scripts/probe-session-worker-vt.py --mutations`。脚本从固定 vendor
版本和实验补丁链创建独立引擎，连接 Core 私有 worker、真实 POSIX PTY 和带预算
libghostty-vt。组合代码位于 `daemon/probes/session-vt-owner.c`，终端侧只提供
`frozen-budget` 适配器，不引用 Core 私有头。此探针不是 daemon 可执行入口。

十类基线覆盖半截 CSI、半个 UTF-8、resize 后捕获、Kitty 红色像素、查询/增量查询应答、编码取消、队列超额、S=L 和 allocator
角色/额度失败。冻结对象带 worker 实际已应用的 S；编码线程停在真实 allocator
回调期间，源继续处理 resize 和 PTY 输出，再销毁源终端。随后编码的 canonical
和资源 payload 必须与 S 时刻的同步参考逐字节相同；另一个终端从已知子进程
前缀独立构造期望状态，避免 source feed 同时漏掉时双方假绿。图片恢复后检查
实际 RGBA 像素。最后全部映射和承诺额度归零。

`--case` 可选 `continuation/utf8/resize/resources/roles/query/cancel/overflow/idle/query-delta/query-caps/isolation/clipboard/input`。全基线未指定单项时仍
返回 3，表示 P0 未完成；`--mutations` 成功返回 0 只表示二十组定向变异和回滚验证
通过。query 用例由真实子进程校验六字节 DSR 应答且无重复输入；应答经 worker 的
有界 sink 暂存，退出 parser 后再写 PTY。Core 另验超额/投递失败/调用归属，运行
`python3 scripts/mutation-check-session-worker.py` 可验证十三组 worker 变异。
编码现在经 Core 固定槽位执行器运行；cancel 用例在真实 allocator 屏障内取消，
确认输入在编码结束前存活、结果不会交付、映射最终归零。运行
`python3 scripts/mutation-check-checkpoint-task.py` 可验证十二组任务变异；Core 的
八类任务基线覆盖排队/执行/就绪取消、借用结果、失败和关闭。取消不强停编码，
只阻止发布并在编码退出后回收；join 只供非 UI 生命周期 owner 调用。
cancel 现经 attachment 控制器执行 detach：旧 generation 立即失效，encoder
屏障内可建立新代，源继续 resize/输出，旧任务不交付新代。运行
`python3 scripts/mutation-check-attachment.py` 验证十类基线与十三组代际/所有权
变异。current 仍只表示成员身份；input_allowed 另外要求完整 S/L 握手。
恢复流已在真实 owner 的 S 开始保留 output/resize；导入 continuation/资源、
重放到 L 并完成两个 READY 后才通过输入闸门。最终状态与权威端销毁前的独立
快照逐字节一致。idle 验 S=L，overflow 验队列超额只撤销 attachment、PTY 继续。
恢复中途掉线按整代作废：detach 清空队列并落 DETACHED 相位，这一代的取事件、
live_begin、READY(L) 与输入全部 STALE，下一代从新的 S 从头重来。
`python3 scripts/mutation-check-restore-stream.py` 验证八类基线、十四组队列/闸门
变异；环形缓冲区另经八类 ASan/UBSan 检查。此小节重放使用同步 VT；下节另述 full surface barrier 组合证据。
attachment 接线仍限组合探针，尚无公开 session/attachment API、生产 full surface
S/L 事务、IPC framing/有序 writer 或 GUI 恢复；
同步参考编码和显式 encoder 屏障仅用于测试，不进入生产捕获路径。

### 真实 full surface 的 S/L 屏障

运行 `python3 scripts/probe-session-worker-surface.py --mutations`，需要 Apple
Silicon macOS 和完整 Xcode SDK。脚本从 pin/实验补丁链干净重建 headless VT 与
full Metal，引擎头分 translation unit，经 `daemon/probes/surface-restore-probe.h`
的 POD view 组合。VT dylib 与 full 静态库用于探针，生产目标仍是分进程。

十二个完整恢复用例为 `pixels/continuation/utf8/resize/resources/idle/query/query-delta/query-caps/isolation/clipboard/input`；
`import-only` 是只检查导入的独立对照。消费增量在独立线程，主线程只做 surface
构造、tick/draw 和输入；S/L 两次 READY 来自实际 engine applied sequence，
并验证取出尚未应用时的旧序号。query-delta 验快照后 DSR 只有源回应。query-caps 用确定性脚本复刻复用器开机
时那串能力探测——DA1/DA2/XTVERSION/DECRQM 四问四答共 48 字节，逐字校验且
收完确认没有多余字节；DECRQM 问的是 detach 之后才补完的 2004。DA1/DA2 要
宿主装 `GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES` 回调才有答复，不装就静默不
答，复用器会停在 DA1 上等；恢复出来的副本对这四类查询全程不答。不直接跑
tmux 是因为本机只有 Homebrew 版，会让闸门依赖机器。isolation 管另一半——有
副作用但没有回复的宿主动作：detach 前后各发一次 OSC 9 桌面通知与配色查询
`CSI ? 996 n`，权威装 DESKTOP_NOTIFICATION/COLOR_SCHEME 回调计数（两次通知、
两次配色，答复逐字为 `CSI ? 997;1 n`），detach 后那一份落进增量流被原样重放
给副本，副本的 host_write 与宿主动作计数全程为 0，而同段字节里的 `SECOND`
确实进了副本最终 canonical，所以零不是字节没到。clipboard 另起一刀覆盖 OSC 52 两个方向：写 `HELLO`/`SECONDCLIP`（权威断言拿到
的是解完 base64 的原文），读两次都答 `PASTE`、程序收到的 17 字节答复逐字校验；
回调是同步的，不回复等于拒绝或空剪贴板。detach 后那一轮进增量流，副本重放时
剪贴板动作计数保持为 0。input 把方向反过来，验恢复后用户真打字点鼠标时字节
按**被恢复的状态**编码：脚本在 detach 前开 `?1000h`/`?1006h`，两个鼠标模式随
快照回来；恢复后中文 IME 组合中（`composing=true`）零字节、提交走键入语义出
原始 UTF-8 六字节且不带 bracketed paste（与 GUI 的
`gt_ghostty_surface_committed_text` 同一调用形状）、粘贴是否包 `?2004` 由恢复
状态决定、鼠标左上角按下得 `CSI <0;1;1 M` 抬起得 `CSI <0;1;1 m`，全程
legacy 写与宿主动作计数为 0。焦点同样随快照回来（detach 前开 `?1004h`）：切走
得 `CSI O`、切回得 `CSI I`，而重复设同一个值必须零字节。这条原是缺口——0004 让
`focusGained` 在副本模式整个提前返回，而它不是 `write_*` 消息、不经 0015 改道
——补丁 `0029-replica-focus-input` 把丢弃改成改道，按 stamped input 交回权威，
同时跳过 `backend.focusGained`。LIVE 后
stamped 输入核对 g/S 与字节，但不向测试已关闭的 PTY 投递。

`pixels` 另给恢复链路补渲染证据：真实 PTY 在 detach 前写 truecolor 红底
白字 `FIRST`，detach 后继续写绿底 `SECOND`。快照导入与 LIVE 后分别抓自己
进程的 Metal 视图（不需要屏幕录制权限），前者必须有红色与白色像素且绿色为零，
后者保留红色并新增绿色。脚本隐藏光标，白色像素因此不能由光标冒充文字。
仍同时检查 canonical/resources 逐字节相等、S/L 屏障与输入归属；像素不替代状态
验收，也不声称字形质量。PNG 留在 `tmp/worker-surface/pixels-{import,live}.png`。
新增变异省掉抓帧的 CGContext 绘制，让位图全零，`pixels` 红而 `continuation`
绿，回滚后复绿。可单独运行 `python3 scripts/probe-session-worker-surface.py --case pixels`。

另有七个 GUI 预算用例：`gui-default` 与 `gui-default-utf8` 用默认 allocator
导入（后者重放同一段 UTF-8，使字体归属故障不能藏在“UTF-8 本身坏了”背后），
`gui-denied/gui-pages/gui-partial` 验准入不足时 surface 构造失败，
`gui-growth/gui-revoked` 验额度冻结后增长被拒并置 sticky 故障。增长杠杆是
回滚行而非 OSC 标题：标题增长的是共享字体缓存，按归属修复它不计入每 surface
预算。释放 surface 后预算在 app tick 才归还（纯 sleep 无效），夹具按 tick
轮询到账本平衡再断言 `live=0`、`mapped==unmapped`。

另有两个真实 TUI 用例 `tui-less`/`tui-vim`：子进程先在主屏滚出 100 行
scrollback，再 exec 真实的 less/vim 进副屏整屏绘制；detach 之后继续 resize
重绘并接收按键（less 的 `G`、vim 的 `iHELLO<Esc>`），最后恢复进全新 surface。
程序输出不是固定字节串，镜像 oracle 因此改用一份独立事件日志重放，并断言
副屏已激活、`SCROLL001` 仍在 checkpoint 里——副屏自己没有 scrollback，所以它
只可能来自主屏滚出去的那段看不见的历史。这组用例逼出了 0028：恢复页按逻辑
行数精确分配、没有 reflow 余量，detach 期间的 resize 会把它拆页，文字相同而
canonical 字节不等。tmux 暂未纳入（读用户 `~/.tmux.conf`、留服务端进程），
它的 DA/XTVERSION/DECRQM 查询覆盖后续用确定性脚本补。

还有一个恢复中断用例 `gui-interrupt`：只放完第一条增量就 detach，断言这一代
就此终结——队列清空、相位 DETACHED，取事件/live_begin/READY(L)/输入全部
STALE，`current` 不再承认这一代，surface 的 canonical 必然不等于权威最后状态
（半恢复的屏幕停在半路）。之后敲键的字节被闸门就地丢弃且不缓存重放，用例按
「字节有没有到」判而不是按返回码判；权威侧槽位立即可被新一代占用。夹具变异
把 detach 推迟到排空之后，`gui-interrupt` 红而 `continuation` 绿，证明中断确实
发生在恢复中途。GUI 进程被 SIGKILL 需要跨进程 IPC，同进程夹具做不出来。

`gui-live` 则让权威在恢复期间继续跑：其余用例都在重放前停掉 worker，恢复面对
的是一份不再变化的历史，这一类不停——READY(S) 之后 worker 再做一次 resize，
权威线程在消费线程排空增量的同时继续 push。断言 L 冻在 READY(S) 那一刻的
`last` 上不往前跑、越过 L 的事件在 READY(L) 之后沿同一条流送达并上屏、最后
由恢复线程停掉权威且 surface 与权威停机时的状态逐字节一致。活着的程序什么
时候写 PTY 不归恢复侧决定，所以停机前后各排空一轮。两组夹具变异：把 emit 挪
到 READY(S) 之前、越过 L 的事件只出队不上屏，都是 `gui-live` 红、`continuation`
绿。该模式下权威作用域与 GUI 作用域同时在册，夹具总额放宽到 320 MiB。

`gui-slow` 是同一套活权威夹具下的慢客户端：恢复流事件上限收到 8，surface 吃进
S 和第一条增量后消费线程就不再取事件，权威继续提交 resize。队列撑满时作废的是
这一代而不是会话——相位 FAILED/LIMIT、队列清空，取事件/`live_begin`/READY(L)
全返回 LIMIT，输入不被允许，`current` 不再承认这一代，半恢复的屏幕拒绝按键且
legacy 写与副作用为零，槽位立即可被新一代占用；权威在队列满之后仍继续推进，
之后才由恢复线程停掉。与 VT 的 `overflow` 不同：那里溢出在恢复开始前、没有
surface 参与，这里溢出发生在排空途中。夹具变异让这一代撑满后照常往下走，
`live_begin` 立刻返回 LIMIT，`gui-slow` 红、`continuation` 绿。

`gui-race` 则在恢复全程放一个抢注线程：从 `ghostty_surface_new` 导入快照到
READY(L) 之后敲键，第二个客户端一直在抢同一个会话。在位的一代不会被顶掉——
抢注方每次 `attach` 只拿到 BUSY 且不写出 generation，`take` 返回 INVALID，
输入闸门不认它，取事件只得到 STALE；在位这一代照常走到 LIVE 且逐字节一致，
恢复结束后抢注方仍是 BUSY，只有在位的一代 detach 之后才接得上。核心侧两线程
同时 attach 由 `core/tests/test_attachment.c` 的 `race` 覆盖。夹具变异让抢注方
改用在位那一代的号，它立刻就能取到东西，`gui-race` 红、`continuation` 绿。

探针使用显式 10×20 像素格，`p0_budget_resize_cells` 为 source/GUI 对齐格尺寸；
零格尺寸只用于原 VT-only 接口。二十三组 full surface 变异、十一组共享预算变异、
八组引擎预算变异与最终回滚通过，另有 VT 十六类/二十组变异回归。引擎变异含字体
缓存归属：目标 utf8 以信号中止，对照 gui-default-utf8 保持绿，只有该变异允许
目标非零退出，超时或构建失败不算通过。默认全用例退出 3；指定
`--case` 或成功 `--mutations` 返回 0 只表示局部验证通过。没有 GUI 全局预算、
完整其他事件源、IPC writer 或生产崩溃重连，P0 尚未通过。

### 独立 GUI 进程死亡后接回同一个 PTY

```bash
python3 scripts/probe-session-process.py --case kill
python3 scripts/probe-session-process.py --mutations
```

此 P0 探针需要 macOS arm64、完整 Xcode 与同一实验补丁链。脚本总是从 pin
重建，复用 `tmp/worker-surface` 的固定执行路径/缓存，**不可与 worker-surface
探针同时运行**。默认两类用例通过后仍返回 3（P0 未完成）；指定单例或完整变异
通过返回 0。实验 codec 不进入 core，也不等于生产 IPC v1。

`process-server` 仅链接 libghostty-vt 与 libSystem；真正的 PTY、state owner、
checkpoint executor 和 attachment 留在这个无 UI 进程。`process-client` 由
posix_spawn/exec 启动，是全新 AppKit/Metal 进程，只有继承的专用 socketpair，
没有共享 terminal/attachment 指针。快照分块读、增量应用、S/L 屏障和 stamped
输入都在 GUI I/O 线程；主线程只构造 surface、tick/draw 与生成用户输入。

`roundtrip` 是单个新 GUI 的对照。`kill` 先导入带未完成 CSI 的红底白字快照，
应用 S 后的 resize，进入 LIVE 并抓到红色像素才报告就绪；权威随后 SIGKILL
该 GUI，waitpid 核对信号。PTY 此时空闲、不产事件，因而槽位释放确实来自 peer
观察连接死亡，而不是队列溢出。旧 peer 销毁不影响新代。原 PTY 在没有 GUI 时
继续 resize、写绿底 `SECOND`，提出 DSR 并逐字核对唯一回复，确认无额外字节后
写 `VERIFIED`；新代捕获更晚的 S，启动第二个全新 GUI，经 socket 接收快照和
S 后的 resize，按引擎实际 applied sequence 走完两次 READY。第二个 GUI 抓到
红/绿/白像素后生成 `USER` 粘贴；g/sequence 和被恢复的 bracketed-paste 编码
经 socket 回到权威，真实 shell 逐字核对十六字节后才输出蓝底 `THIRD`。
最后抓帧必须出现蓝色像素，证明这条 LIVE 输出也实际绘制。GUI 最终 canonical/resources 与权威停机时
的快照逐字节相等，legacy 回写与宿主动作计数均为零。

PNG 在 `tmp/worker-surface/process-round-{1,2}.png` 与 `process-final.png`。像素仅验证颜色/文字存在，
不验证字形质量。客户端 CPU/页 allocator 继续用 admitted provider，正常退出
须 tick 到预算回收；SIGKILL 的进程内存由 OS 回收。接收快照的临时 buffer 与多 surface 的 CPU/页额度现已纳入下述共享池；
App/共享字体/GPU 及跨 GUI 进程总量尚未统一计入预算。

帧测试另有十一类：独立字节常量 oracle、任意短 I/O/WAIT、分块 blob、拒绝
magic/version/flags/kind/超长/截断/错误 blob kind 与越界。十五组进程变异、九组
codec 变异与十四组 GUI 预算变异都要求目标红、对照绿、SHA256 回滚；最后复跑两类进程、十一类 codec
与二十六类已有 full-surface 基线。尚无生产 listener、身份/token、发现/列表、
公开生命周期 ABI、helper 打包或 GhostermApp 的恢复 UI，不能声称产品已支持崩溃重连。


### GUI 进程内共享恢复准入池

`terminal/session-state/gui-budget.{c,h}` 取代每 surface 独立 ledger。一个 GUI
进程显式创建一个池，每个恢复 lease 在同一 admission 锁内预留 CPU、原生页及
wire 三档额度；部分失败连同失败 scope 一起回滚，不占 surface 槽位。总量与
surface 数量分别限制。已承诺的增长余量计入 committed，不能用“当前还没分配”
多收一个恢复任务。池/lease 都不移动，allocator descriptor 生命周期随 lease。

进程探针示例策略为总额 320 MiB，每 lease CPU 64 MiB、页 64 MiB、wire 32 MiB，
surface 数上限 3。两份真实 Metal surface 占满字节额度，第三份在引擎构造前
拒绝；tick 到一个 surface 的延迟释放完成后，额度可再次导入。第一次辅助
surface 的 wire 额度故意耗尽，健康位 sticky 失败使引擎拒绝序号发布和增量；
主 surface 状态仍相等、继续恢复与输入。失败 lease 空闲后允许释放，避免永久
占住额度。snapshot/expected 的 canonical/resources 接收缓冲使用 wire allocator，
按原长度归还；还活着的接收缓冲必须阻止 lease 释放。OS 页取整照常计费。

八类独立基线覆盖字节/数量限额、两线程竞争、wire 故障隔离、存活寿命、部分
准入回滚及非法策略；十四组预算变异和三组新增原生接线变异证明断言有牙。
仍用 `python3 scripts/probe-session-process.py --mutations` 运行：两类进程、
八类预算、十一类 codec、十五组进程/九组 codec/十四组预算变异及二十六类
legacy surface 回滚复验全部通过。不是生产 GUI 的内存总上限：App、共享字体、
系统/GPU、截图和固定线程栈/帧缓冲尚未计入，也没有跨 GUI 进程的统一准入。


### 接收快照的发送端内容校验

实验 `GTP0` 的 SNAPSHOT/EXPECT 元数据从 40 字节改为 104 字节，末尾分别追加
canonical 与 resources 的 32 字节 SHA-256。权威对实际发送缓冲计算摘要；GUI
在有界接收完成后、构造 surface 或对拍之前核对两份摘要。此前由 GUI 自行生成
引擎导入 digest，只能绑定其本地收到的字节，不能检查收到的内容是否仍与发送端
一致。新增两组篡改变异分别翻转发送端摘要的一位，目标恢复在导入前失败，同库
legacy 对照仍绿。引擎已有的 generation/sequence 绑定保持独立。

这只检查内容损坏，不提供身份认证、抗恶意篡改或跨构建兼容；专用继承 socketpair
仍是实验前提。生产协议仍须实例/构建标识、token、完整性终止标记与失败回收。


### READY 等待期间继续消费 L 后的事件

`python3 scripts/probe-session-process.py --case crossing` 在 READY(S) 固定 L 后，
让仍运行的权威 worker 再提交一次 resize。S+1..L 排空后先发送 LIVE_BEGIN(L)，
随后在等待 READY(L) 的同一条连接上发送 L 后事件。客户端发出的 READY(L) 仍
确认原来的 L，但等待 LIVE_OK 时继续读取/应用增量，LIVE_OK 的边界仍须等于 L。
客户端报告绘制就绪与生成输入时使用更新后的实际 applied sequence，权威校验
输入 stamp，而不强迫它退回 L。最终状态和像素验证保持不变。

该用例仅控制一条越过 L 的 resize，证明恢复阶段能跨越固定边界并在 LIVE 确认
前消费增量；不代表已完成持续大流量/双向 socket 背压下的生产有序 writer。
漏发越过 L 的事件、等待 LIVE_OK 时只接受确认不读事件，两组变异分别使 crossing
失败、roundtrip 对照通过，回滚后复绿。


### 分块快照的总传输 deadline

实验 codec 的单帧 header/body 共用 5 秒单调 deadline；现在一个 blob 的所有
帧也共用同一个 deadline，发送/接收都不能靠每块进度重置预算。此前至多 16 MiB
的快照可被切成很多逐块未超时的帧，总耗时却远超 5 秒。超过 blob deadline
返回失败，已经传出的前缀不可当作完整快照，也不能在同一流上重启解析。

新增 slow-send/slow-receive：每次 I/O 回调最多等待 30 ms 后返回 WAIT，每个
body 延迟 2.7 秒。单帧均可在 5 秒内完成，但两帧 blob 总时间超限必须失败；
发送/接收分别恢复逐帧 deadline 的变异必须使目标红、正常 blobs 对照绿。
该上限属于单个 blob，不冒充整代恢复事务 deadline、生产取消回收或高流量
双向背压验收；canonical/resources 两个 blob 仍各有独立上限。


### 同一引擎构建才允许导入

进程探针构建时生成 `tmp/worker-surface/session-process-build.h`，指纹域为
`GHOSTERM-P0-ENGINE-BUILD-1`：按顺序记录 pin、`macos-arm64`、实验快照契约、
实验补丁链原文以及实际链接的 VT dylib/full 静态 archive 的 SHA-256，每条
记录的名称和值均有大端 u64 长度前缀，最后取 SHA-256。实际产物摘要也覆盖
基础补丁、工具链/编译选项对引擎二进制的影响，不只依赖版本字符串。
指纹文本留在 `tmp/worker-surface/process-engine-build.txt`。

SNAPSHOT/EXPECT 元数据扩为 136 字节，末尾 32 字节携带该指纹。GUI 先验证
构建一致，再分配/接收 canonical/resources 和解码，拒绝不同构建、不做兼容
读取。翻转发送端构建指纹的一位必须在接收快照内容前失败，同库 legacy
对照仍通过。生产连接的版本握手、发现、认证与人工清理入口仍未实现；摘要
也不是身份认证。该双引擎组合指纹服务于实验，尚未固定正式协议字段。


### 恢复原型的阶段测量

`process-server` 输出 `METRIC` 行，不设置性能通过阈值。`freeze_ms` 只包住
owner 上的不可变捕获调用（这段期间 owner 不能读 PTY）；`encode_ms` 只包住
编码线程的编码调用。`send_ms` 是快照元数据/两份 blob 被本地 socket 接受的
时间，**不是**客户端已导入时间；`restore_to_drawn_ms` 从开始发送快照到收到
CLIENT_LIVE，含 GUI 导入、辅助 surface 准入测试、S/L 握手、绘制及主动等帧。
该指标不含前置 spawn 调用，也不等于纯引擎恢复延迟。

同一行记录实际 canonical/resources 长度、capture/encoder scope 的计费峰值、
整个权威 ledger 的 live/committed 峰值。映射 provider 按 OS 页取整，计费峰值
不等于 RSS；已预留增长空间也不是 OS 实际分配。正常结束另输出 authority
`getrusage(RUSAGE_SELF)` 峰值 RSS（macOS 字节）与累计 user/system CPU 秒；不
包括 GUI 进程，也不是空闲 CPU 测量。时钟不可用时耗时显式为 -1，rusage 错误
明确打印 unavailable，不能用零冒充成功采样。

当前测的是 FIRST/SECOND/VERIFIED/THIRD 小负载，不能代表长 scrollback、图像
或真实 TUI 的内存/捕获延迟。没有旧进程内路径的同环境对比，也未测输入延迟
分位数与空闲 CPU，因此 P0/P3 性能校准要求仍未完成。


### 千行 scrollback 的跨进程恢复测量

`python3 scripts/probe-session-process.py --case history` 先通过真实 PTY/awk
输出 1000 行固定宽度历史，再清可见屏并回到原 FIRST/SECOND/THIRD 交互。清屏
不清历史，owner 捕获时直接查询引擎的 scrollback 行数并要求至少 900，避免
把没运行的历史负载当成性能样本。固定输出观察缓冲由 4 KiB 增为 128 KiB；
它属于探针的测试观察，不是生产 scrollback 或无界事件日志。

其余恢复链路相同：不可变捕获、异步编码、共享预算、引擎构建/内容摘要、
分块传输、真实 Metal 两 surface 导入对拍及原 PTY 的 stamped 输入/蓝色像素。
canonical 大于单帧，多帧 blob 路径实际运行。删除历史生成的变异使 history
失败、roundtrip 对照绿；回滚后重跑四类进程用例及原有 surface 基线。

这是 1000 行确定性文本的测量点，仍不代表大图像、最大 scrollback、延迟分位数
或旧路径性能对比，也没有据此设定生产预算阈值。


### 标题与 cwd 的完整快照恢复

所有跨进程用例的真实 PTY 现在在 FIRST 前发 OSC 2 标题 `Ghosterm initial` 与
OSC 7 cwd `file:///ghosterm/initial`；补完半截 CSI 后发 detached 版本的两个值。
owner 捕获前用 VT 的 TITLE/PWD 字段逐字查询并核对，最终停机捕获前再核对
最新值。OSC 7 存储的是原始 URI，探针不把它冒充已解析的本地路径。

第一被杀 GUI 的快照因此含初始元数据，接回 GUI 的新快照含 detached 更新，
完整 canonical/resources 对拍会覆盖副本导入；标题/cwd 不依赖历史宿主回调
重放才能恢复。首次/更新的两个字段共四组删除变异分别拦住遗漏，初始阶段
目标 kill、roundtrip 对照绿，更新阶段目标 roundtrip、legacy 对照绿。
这不代表 GhostermApp 的标题/目录 UI 或 OSC 133/7501 摘要迁移已交付；它只是
引擎元数据在真实 PTY/跨进程快照中的恢复证据。


### 权威 VT 回调接入程序状态表与捕获

`process-server` 在 headless VT 安装 PROGRAM_STATUS 回调，真实 PTY 在初始、
detached 和恢复后输入三个阶段发 OSC 7501：`build` 的 working/25%、
blocked/permission/未知进度、done。回调只消费引擎解码的字段，检查尺寸、字段
长度及内嵌 NUL，转换成临时 NUL 字符串后由 C 表复制；不能直接保留回调指针。
拒绝报告形成 sticky 错误，并在当前 owner apply 返回，不能继续假装状态有效。

worker 在同一次捕获回调里复制程序表与不可变 VT 状态，记录同一个 S；编码
线程读取复制后的表。首次 GUI 代的快照摘要是 working，接回代是 blocked，
即使权威后来已更新为 done，借出的摘要仍必须是 blocked。成功结果释放时一并
释放摘要；取消/编码无结果时由 input cleanup 释放，权威表随 session 清理。

三组新增变异分别移除回调安装、移除 detached blocked 序列、遗漏摘要捕获，
目标失败、legacy 对照通过。这仅证明服务端 owner 与编码任务的寿命接线，
程序摘要尚未编码进实验 IPC、尚未由 GUI 展示；OSC 133 命令摘要也仍未接入。
表/副本用有界 malloc，尚未接共享 allocator ledger，不能计作全局预算完成。


### 程序摘要的实验快照传输与客户端导入

`session-summary-wire` 编码所有记录（不是只传测试 root）：GPS1 magic、u32
条数，每条 state/kind/progress+1/reserved，四个 u32 长度前缀文本字段，无指针
或内嵌 NUL。最多 32 条、65536 持有字符串字节，序列化最大 66184 字节。
解码事务性创建独立表，拒绝 clear 记录、重复 id、未知字段值、超长/截断、NUL
和尾随数据；失败不发布部分表。15 类测试包含独立常量编码 oracle 和缓冲保护。

快照元数据扩为 176 字节，附带摘要长度与 SHA-256，SUMMARY blob 紧随资源
blob。canonical/resources/summary 总和不得超过 16 MiB。客户端用 admitted
wire scope 接收和校验，然后解码摘要并在 READY(S) 前核对字段。第一个 GUI
恢复 working/25%，接回 GUI 恢复 blocked/permission/-1，直接导入、不依赖
历史 OSC 回调。EXPECT 里的 done 只用于最终诊断对拍，不是 LIVE 摘要推送。

增加摘要损坏/遗漏导入两组进程变异和八组 codec 变异。摘要接收字节已计入 wire，
但解码表/深拷贝及服务器序列化栈缓冲仍未纳入统一 ledger；尚无实时 SUMMARY
事件、OSC 133 或产品 UI 接线。该格式是 GTP0 实验，不作为生产协议。


### 与输出配对的实时程序摘要

权威回调更新程序表时标记 dirty；owner 成功 apply 后，将触发该变更的输出和
当时编码的摘要作为一个有界 attachment 队列项提交，保留同一序号。发送线程
不读取仍在变化的 owner 表，避免发送时把较新摘要错配到较旧输出。该实验项
使用私有 kind=3，payload 为 u64 输出长度、输出字节与 GPS1 摘要；不新增第二个
state sequence。没有摘要变化的输出和 resize 仍走原路径。

writer 发送 OUTPUT_SUMMARY、带 g/sequence/length/SHA-256 的 SUMMARY_UPDATE 和
分块 SUMMARY；客户端接到配对输出后，在同一 I/O 线程读取、校验和事务替换
摘要，再推进客户端流 cursor，READY/后续协议不会跨过半条配对项。引擎自身
applied sequence 仍在输出应用时推进，尚不提供通用的 UI 多对象原子发布。
接收字节仍计入 GUI wire scope；表及临时编码栈未纳入总 ledger。

真实 PTY 接受用户输入后发 done 与 THIRD。客户端必须在收到最终 EXPECT 后、
导入其摘要之前检查实时表已是 done/-1/Done，证明更新来自 LIVE 配对流而非
最终诊断快照。禁用 dirty 配对、篡改实时 generation、篡改摘要 digest 三组
变异均目标红、legacy 对照绿。尚无产品 UI、OSC 133、通用高流量 writer 或
完整预算。单条最大配对项与累计积压的容量验证见下节；生产错误路由仍待实现。


### 最大程序摘要/输出配对项与累计背压

实验队列由固定 64 KiB 调整为 `PSW_RESTORE_BYTES`（8 + 16384 + PSW_MAX，
82576 字节），至少容纳一条最大配对项。它仍是有界队列，不随积压增长。
owner event 回调接受核心的 LIMIT 返回，不再因为 attachment 队列超额直接
触发进程断言；真实服务端的断连/错误结果路由仍未交付。

`session-summary-queue-test` 用完整 32 条记录恰好占满 65536 字符串字节，
生成 66056 字节 GPS1，独立解码后仍为 32 条。配上 16384 字节输出后的单项
为 82448 字节，使用真实 attachment/tasks/restore-stream 验证入队和计费。
第二条同样大小的项必须返回 LIMIT、清空待恢复字节/事件、撤销本代；新一代
可再次接受完整配对项。这里没有假队列，也未声称已经通过真实 Metal 最大
摘要压力或累计背压后自动重连。

两类基线、三组隔离变异（旧 64 KiB、超额不撤权、失败不清队列）与 SHA256
回滚均经 process 探针 `--mutations` 运行。独立 ASan/UBSan 构建覆盖该测试
使用的整个 C 记录表/codec/attachment/stream/task 路径。固定栈编码缓冲、
记录表及队列 arena 尚未统一纳入总 ledger，P0 预算验收仍未完成。


### 真实 OSC 133 回调与命令捕获

进程权威安装 SEMANTIC_PROMPT 回调，使用引擎解码的 command/error 与可选
exit_code。回调有界转换 span 后交给 C 命令模型，单调纳秒时钟另带有效位；
处理错误沿 owner sticky 错误返回。真实 PTY 初始发送 `133;C;cmdline=ghosterm test`，
detached 后 `133;D;7`，收到恢复后输入再发下一条命令 start/end 且不报退出码。

owner 在同一 freeze 回调中按值捕获命令模型与程序表/VT 状态，并在编码线程
读取捕获值。首次捕获运行中，接回捕获已结束/退出码 7；原 owner 更新为下一条
已结束但退出码未知时，借出的旧命令仍须是 `ghosterm test`/7/有效耗时。
三个变异覆盖回调缺失、真实序列缺失退出码和捕获遗漏，目标红/legacy 对照绿。

当前只完成权威回调和捕获，命令模型尚未加入 GPS1/IPC，客户端并未接收这些
命令字段；不能用现有程序状态恢复来宣称 OSC 133 已完整恢复。prompt_start 对
working/blocked 表清理、错误文本真实负载、产品 LastCommand 迁移仍待补齐。


### 命令快照的独立实验编码与导入

GCS1 命令 codec 显式编码运行中/退出码有效/耗时有效三个 bit、i32 退出码、
u64 duration_ns、命令和错误文本的两个 u32 长度；字段均大端，无指针/结构
padding。最多 8220 字节，拒绝未知 bit、非零保留字节、运行中却带结束结果、
无有效位却有数值、NUL、超长/截断和尾随字节。负退出码保留，0 ns 有效耗时
与未知耗时分开。start_ns/has_start_time 只属于 daemon owner，不进入 wire。
解码失败保留调用者原值，encode 容量不足不写前缀。

快照元数据扩为 216 字节，追加命令长度/内容 SHA-256，COMMAND blob 在程序
表后发送；全部 blob 的总长仍受 16 MiB 上限。接收缓冲走 GUI wire scope，
READY(S) 前解码并核对：第一代是运行中命令，接回代是已结束/退出码 7/有效
耗时。最终 EXPECT 导入下一条命令、退出码未知，这仍是诊断快照，不是实时
命令事件；目前客户端 LIVE 期间的命令状态仍不会主动更新。

13 类独立 codec 基线与八组变异覆盖固定字节 oracle、负退出码、未知/零值、
内存容量与非法输入；另有快照哈希损坏、客户端遗漏命令解码两组进程变异。
OSC 133 的实时传输、prompt_start 程序清理联动、真实 error 文本及产品 UI
迁移仍待完成，不能宣称完整命令生命周期恢复已交付。


### 实时命令摘要独立于 OSC 7501 变更

输出配对项现在包含两个 u64 长度、输出、GPS1 程序表与 GCS1 命令状态。
程序回调或命令 start/end 任一个变更都触发配对，发送线程只用已入队的值。
更新 metadata 扩为 88 字节，两个摘要各有长度/哈希，并共享 generation 与
输出序号。客户端读取/校验/解码两份摘要后再推进流 cursor，daemon 的时间
起点仍不传输。固定容量相应为 90804 字节；最大实际文本组合配对为 90676。

为排除命令更新借用程序 dirty 的假绿，真实 shell 先在 USER 输入后发 OSC 7501
的 done/THIRD，然后等待 fixture gate；权威观察 THIRD 后投递门控换行，shell
另发只有 OSC 133 的下一条 start/end（无退出码）和 FOURTH。该因果门控让两段
至少经过不同的 owner read/apply。客户端在最终 EXPECT 导入前必须已见下一条
命令停止运行、退出码未知、耗时有效，不能靠最终诊断快照补齐。

新增移除 command dirty、篡改实时命令 digest 两组变异。最大配对/压力测试
也加入 4096 字节命令、4096 字节错误文本、负退出码和最大 u64 耗时，并用
ASan/UBSan 验证。仍不是 UI 多对象原子提交或完整 production writer；OSC 133
prompt_start 联动、真实错误报告、全局预算与产品 UI 尚未完成。


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

GWR1 的 56-byte payload（所有整数大端；外层 GTP0 frame kind=PW_RESULT）：

| 偏移 | 长度 | 内容 |
|---|---|---|
| 0 | 4 | ASCII `GWR1` |
| 4 | 4 | flags：bit 0 has_sequence，bit 1 has_exit_code，其余拒绝 |
| 8 | 8 | 非零 attachment generation |
| 16 | 8 | 最后成功 authority apply 序号 |
| 24 | 4 | worker end kind |
| 28 | 4 | failure stage |
| 32 | 4 | primary GT status |
| 36 | 4 | 冗余 exit_code，仅已知 CODE 可非零 |
| 40 | 4 | termination observation status |
| 44 | 4 | UNKNOWN / CODE / SIGNAL |
| 48 | 4 | 观测值：CODE >= 0，SIGNAL > 0，未知必须 0 |
| 52 | 4 | 保留，必须 0 |

worker 终态与退出观测取值来自私有 C 枚举；这只是实验 schema，尚非生产
session-ipc 字段表。非 EOF 不得附带退出观测，ERROR 必须有非零主错误和
失败阶段，STOP/EOF 主状态必须 GT_OK 且无失败阶段。

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
