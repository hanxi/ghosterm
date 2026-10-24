# P0：surface 导入与资源 I/O 的隔离原型

精确上游 pin：`81598385d608dda4946093d6d6a234d204a2f225`。
先按顺序应用父目录 0001–0003，再应用本目录的 `0004-replica-surface.patch`。
`0005-resource-io-hooks.patch` 可单独应用到原始 pin，与 0004 的构造接口独立。

## 为什么需要

上游 full surface 的嵌入 API 没有 snapshot 导入入口，且其 StreamHandler 会将
终端查询和用户输入合并至 host_write。daemon 的权威 VT 实例已经应答查询时，
直接将相同输出喂给普通 surface 会重复应答，并可能重做剪贴板、通知等宿主动作。

本补丁只供 `scripts/probe-session-surface.py` 的隔离源码副本使用。
生产 `scripts/build-ghostty.sh` 只读取父目录的 `*.patch`，不会启用此原型，
已有 Swift/C 绑定与生产 xcframework 均不修改。

## 实验边界

- 新增的 config 字段、快照观察入口是 P0 实验接口，不是生产恢复 ABI。
- 解码在 surface 构造期间完成，此时 renderer/IO 线程尚未启动。Terminal 先放到
  `Termio.terminal` 的最终地址，再建立持久只读 TerminalStream、补 continuation
  并核验其规范字节。借用的输入快照不保留到构造调用结束之后。
- 仍使用 full libghostty 的 Metal renderer 和输入编码器；只读标准 parser 消费
  副本输出，不调用普通 StreamHandler 的查询/宿主动作路径。
- 副本从构造起即为只读模式，故没有从旧解析器遗留的应答 mailbox。
  自动 visibility/color/size 报告也抑制；用户输入仍通过原 host backend。
  焦点报告例外，见 0029：它是用户输入而非宿主动作，抑制改为改道。
- parser 必须先于 terminal 销毁：标准 Handler 的 deinit 仍需 terminal allocator。
  该顺序已在构造、绘制、销毁的重复流程中验证，不能照搬普通 StreamHandler 的顺序。
- 固定缓冲快照观察入口在 renderer 锁内同步读取，可验证当前已应用状态，
  但没有引入生产序号、取消或跨 mailbox barrier。

## 运行与验收

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface.py --mutations
```

探针从未改动的 pin 构建 VT 夹具，再从独立 archive 构建实验 full 库；
不链接、覆盖或复用生产 xcframework。测试和构建临时文件均在工程 `tmp/`。
原生 full 构建关闭 Sentry，避免实验崩溃产生项目外的报告。

夹具覆盖 ground、CSI、UTF-8、OSC、ESC、DCS、APC query，包含主/备用屏幕、
历史、宽/组合字符、超链接及输入模式。导入前后和增量后的快照逐字节对拍，
检查查询/剪贴板/通知回调为零，粘贴输入必须带恢复模式要求的 bracketed-paste 包裹。
额外的真实 PTY 夹具在 GUI 不存在期间继续驱动最小 TUI，权威实例处理查询，
随后将捕获的实际增量与快照导入 full surface 对拍；子进程确认 resize 并正常退出。
Vim 夹具通过 `/usr/bin/vim -u NONE -U NONE -i NONE -n -N --noplugin` 启动，
文件路径限于工程临时目录，读取真实 VT 网格确认测试文本已编辑，再验证 full surface
导入及增量后的状态；正常退出和失败清理均回收子进程。
tmux（本机验证 3.6a）使用唯一 socket 和空配置，实际输入进入 `/bin/cat` pane；
客户端 detach 后 kill-server，并用 `has-session` 验证 server 已停止。不会连接用户
既有 tmux server，也不靠 socket 文件删除来推断进程状态。
变异分别破坏 continuation、重新启用副本应答、丢弃用户输入、遗漏 PTY/Vim 增量及
实际 Vim 编辑，另覆盖 tmux 增量与 server 清理；每组需目标红、
同一实验库上的独立对照绿、回滚复绿及源文件 SHA256 一致。

## 尚未解决，不能据此通过 P0

图像/placement/loading/animation 尚未进入快照；此原型不验证图像恢复或文件装载
副作用。只读 effects 不意味着所有资源路径均已隔离。16 MiB 输入上限和 1 MiB
continuation 上限不能替代解码后的内存预算，decode 仍会依据快照分配内存。
同步 encode 的 worker 停顿、快照双份内存、持续并发输出尚无预算证据。

renderer 仍可能推进动画；副本 UI 事件、配置及 resize 的统一排序，主副本间的
host action 事件路由、失败可观测 feed API、持续并发的恢复事务及 GUI 崩溃重连均未实现。
不能用本补丁发布“可准确重连”或固定 IPC 恢复协议。

## 0005：外部资源 I/O observer/resolver

Kitty 临时文件被第一次装载消费删除后，GUI 不能重读同一路径。只修 snapshot
不解决实时输出；只读查询/剪贴板 effects 也不禁止 Kitty 的文件装载。
0005 在 `LoadingImage.init` 内部、保留原装载验证的基础上增加可选 hook：

- 普通/未配置实例保持原行为。权威实例 observer 接收外部读取的原始载荷，
  或明确的读取拒绝结果；载荷尚未完成 PNG 解码、配额决策和 placement 提交。
- 副本 resolver 接收 medium/path 并提供已收到的载荷或源读取失败结果。
  该分支不打开文件、不执行原文件策略，也不回退到本地 I/O。
- 回调返回 false、无效结果或复制失败设置原有 sticky `VT_PROCESSING_ERROR`，
  适配器必须停止失效的恢复/会话。有效的源读取失败保留原本协议的优雅失败语义。
- C setter 对每个 terminal 只允许配置一次，调用方必须在新输入前安装。
  callback/ctx 必须活到 terminal 释放；observer 需在返回前复制借用数据，
  resolver 的借用数据须保持到当前 feed 完成，不能在另一线程提前复用队列槽。

```bash
python3 scripts/probe-session-resource-hooks.py --mutations
```

脚本从独立原始 pin archive 构建无 UI VT 库，只应用 0005，不链接 AppKit/Metal。
测试验证真实临时文件仅由权威实例消费；副本禁用临时文件装载后，仍能通过原始同一命令
和 resolver 得到相同像素、placement 与规范状态。另验证源读取失败、observer 拒绝、
事件路径不匹配及重复消费；四组 native 变异护成功事件、载荷、失败标记和失败事件，
各有仍使用 hook 的独立对照与 SHA256 回滚。

这不是生产资源协议：未定义序号/generation、command token、异步完成、资源总预算，
未涵盖图像最终解码/配额结果和加载中/动画的 snapshot 状态，也未接到 full surface。
当前 C 夹具是一槽有界队列且串行调用，不能充当并发资源队列实现。
P0 仍未通过，正式 ABI/IPC 在这些边界验证后再定稿。

## 0006：无 UI VT 复用 full 引擎的真实 PNG decoder

0006 叠在 0005 后，导出实验 C callback `ghostty_p0_decode_png`，通过已有
`ghostty_sys_set(DECODE_PNG, ...)` 显式安装。它复用 full 引擎的 Wuffs decoder，
不引入 AppKit、Metal 或假解码器；默认行为不变。返回的 RGBA 缓冲区归调用方所有，
使用传入的 GhosttyAllocator 分配；失败时输出全部清零，不转交半成品。

```bash
python3 scripts/probe-session-resource-hooks.py --png-mutations
python3 scripts/probe-session-resource-hooks.py --case file
```

真实 1×1 红色 PNG 经权威临时文件装载和原始资源事件，在副本中解码成同样的 RGBA
与 placement。普通文件用例在权威读取后把源文件改成绿色；副本仍必须显示红色，
证明没有重读路径。坏 PNG 与拒绝分配验证失败输出和资源回收。
三组 PNG 变异分别短路真实 decoder、留下失败输出、绕过调用方 allocator，
每组目标红、同库独立对照绿、回滚 SHA256 一致。
这仍只证明局部解码路径；跨平台预算/最终提交一致性、完整图像快照及生产协议未完成。

## 0007：full Metal surface 的同步资源 resolver

0007 叠在生产 0001–0003、实验 0004–0006 后，只用于隔离原型。
构造选项追加 POD callback/ctx；只允许和 replica snapshot 一同安装。
Termio 在最终内存地址建立只读 stream 后、回放 continuation 前，为两张 screen
安装同一 resolver，并把失败接到该 stream 的 sticky semantic failure。
ctx 活到 surface free，返回的借用载荷至少活到当前 process_output 返回；
当前夹具串行运行，不能据此声称并发队列安全。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-resources.py --mutations
```

无 UI 权威实例先完成真实临时文件消费和 PNG 解码；GUI 此时尚未构造。
测试保存空图像 checkpoint、实际原始命令和显式小端编码的有界测试事件，随后构造
原生 full Metal surface，在引擎 LoadingImage 内消费 resolver 事件。
RGBA/PNG 必须有真实红色像素及 viewport pin (4,2)，源读取失败必须无图像；
同时对拍 canonical state、确认无副本应答、验证 bracketed-paste 输入和绘制/释放。
额外的实验同步观察函数在 renderer lock 内复制单张图像及 pin，只支持一个 pin
placement，不是生产 API；不能用 canonical snapshot 代替独立像素断言。
缺失或重复资源事件必须使 sticky 语义失败可观测：snapshot 返回失败且长度为零。
借用事件的 NSData 显式保持到 surface 释放，不能依赖 ARC 的最后一次对象使用时机。
三组变异破坏引擎 resolved payload、真实原始命令的位置及 snapshot 的失败检查，
同一实验库上的源失败对照仍绿，回滚后全例复绿并校验 SHA256。

这证明 checkpoint 后新增资源的局部路径。checkpoint 内既有图像/placement、
分块 loading、动画、最终提交预算、并发恢复事务和生产 IPC/GUI 重连仍缺失；
默认运行仍退出 3，不得据此通过 P0。

## 0008：在解码前导入源图像额度，重载时保留 resolver

0008 叠在 0007 后。原始 I/O 成功不是最终图像提交成功：权威额度 3 字节时会拒绝
4 字节 RGBA，额度较大的副本却接受。为隔离并修正这一类配置分歧，实验构造选项
追加显式 `limit_set` 与 uint64 额度；配置 resolver 时缺少额度必须拒绝构造。
零额度与未提供额度不折叠。内部 snapshot DecodeOptions 在 screen 解码前传入
源额度，Termio 保存它，GUI 配置重载不能把它改回宿主偏好。

重载还会重设 image loading limits，原先会丢掉 resolver。副本现在重建 direct-only
策略并保留 resolver，普通非副本保持原装载策略；不允许重载悄悄恢复本地 I/O。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-resources.py --quota-mutations
```

额度仅在测试事件清单的显式小端字段中传递，不是生产协议。新增拒绝夹具验证权威和
副本在 3 字节额度下都没有图像，且资源命令确实调用 resolver；配置重载后仍成立。
首次导入/增量继续对拍 canonical state。重载本身会修改其他终端配置，重载后只验证
本切片的额度、无图像和 resolver 调用，不声称整个状态仍与未重载的权威相同。
四组变异覆盖解码前额度、重载覆盖额度、缺省额度误接纳及重载丢失 resolver，
均要求目标红、同库 RGBA 成功对照绿、回滚 SHA256 一致。

这只能消除测试覆盖的额度/策略漂移，不能替代最终解码/分配/提交结果同步。
页面/图像总预算、资源快照、分块与动画及持续并发恢复仍缺失，P0 未通过。

## 0009：有界静态图像/placement 快照原型

0009 叠在 0005–0006 后，仅扩展无 UI VT 实验 ABI，不进入生产构建。
原生模块导出完整静态 RGB/RGBA 像素、image id/number、usage 标记、按 transmission
age 排序的图像、两张 screen 的额度/自动 ID 计数器，以及 pinned/virtual placement
的显式几何字段。pin 编成完整 screen 坐标，在已恢复页面上重建，绝不复制指针。
restore 为 image/cache 生成新的 generation，按原顺序保持淘汰行为。

使用显式小端字段、版本 magic、长度和 CRC32C；调用者指定 wire 上限，内部另限
1024 images / 4096 placements。解码先完整 preflight，再构造临时资源图，全部验证后
一次发布。晚期非法输入不能先改变页面 pin 池；分配失败不得报告成功，临时资源须回收。
图像 loading、动画、pending 以及 relative placement 尚不支持，export 明确拒绝，
不能只保留首帧或把 relative 位置改成 virtual 后假装恢复成功。

```bash
python3 scripts/probe-session-resource-snapshot.py --mutations
```

C 夹具独立观察真实像素、pin/virtual、主备用屏幕、后续删除、自动 image/placement
ID、淘汰年龄与 transient 优先级，另验 CRC、晚期额外字节、分配拒绝与 wire 上限。
16 组 native 变异逐一验证目标红、同库独立对照绿、SHA256 回滚。
分配失败另遍历实际四个分配位置，逐次确认资源图未发布、可重试且最终释放后回调分配清零；
删除临时图清理的变异必须变红，正常静态导入仍绿。
分配用例只统计 C allocator 回调；页面 mmap 和 pin 池保留容量不能冒充总内存预算。

这是静态子集，未支持 loading/animation/relative 的恢复，也未定义 checkpoint/resource
跨包绑定或并发恢复事务。C ABI 的实验状态码独立定义：0 成功、1 非法、2 分配失败、
3 未支持、4 超限、5 需要输出空间；未来 Ghosterm 适配必须映射为明确的核心错误。
默认探针仍退出 3，不得据此宣布 P0 通过。

## 0010：在 full surface 构造时恢复 checkpoint 静态资源

0010 叠在 0004–0009 后，追加借用的资源快照指针/长度，仅允许与 canonical snapshot
和显式源额度一同使用。在 renderer/IO 线程启动前，先恢复 canonical terminal，
再完整 preflight 资源图（额度须与构造元数据一致），导入后才绑定 stream 并发布 surface。
错误时释放临时 terminal/资源，不能留下一个被当成成功的空白副本。

实际拒绝测试还暴露上游 Surface.init 的字体引用回滚缺口：拿到 SharedGridSet.ref
后缺少 errdefer，资源导入失败返回 NULL 后 App.deinit 会因字体引用未清空而 panic。
本实验增加对应 errdefer；正常 surface 仍由正常 deinit 释放一次。尚未进入生产补丁集。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-resources.py --snapshot-mutations
```

测试在增量前独立读取 full 引擎的既有 RGB/RGBA/真实 PNG 像素和 pin；双屏夹具验证
主/备用资源均在，随后真实外部资源命令更新 primary，再返回 alternate 对拍。
保留输入、无副本应答和资源事件失效验证；额度不一致、缺少资源缓冲区必须拒绝构造，
并能正常销毁 App。四组变异护遗漏 import、额度不匹配误接纳、NULL/长度矛盾及
字体引用回滚，均带独立成功对照与 SHA256 回滚。最后一项明确期待上游断言 SIGABRT，
不能把随机崩溃当成目标断言通过。

这是构造时静态资源恢复，不含 loading/动画/relative、并发恢复事务、checkpoint 跨包
绑定或总内存预算。默认完整运行仍退出 3，P0 尚未通过。

## 0011：保留 relative placement 父链

0011 叠在 0009/0010 后，将实验资源格式升级到 GTIR0002。相对 placement 编码
父 image/placement ID、internal/external 命名空间及有符号水平/垂直偏移；不能把
当前像素坐标展开成 pin，否则删除父节点后的孤儿回收语义会改变。
导入在分配 tracked pin 前验证完整父链，拒绝缺失父节点、循环和超深链；父节点
可以排在子节点后面，不以序列化顺序决定合法性。继续使用临时图事务式发布。

```bash
python3 scripts/probe-session-relative-resources.py --mutations
```

夹具覆盖静态对照、单层/多层父链、父节点后置和内部 ID，观察恢复坐标及删除父节点
后的 placement 清理。坏包重算 CRC，独立验证缺失父节点和循环拒绝、
状态不变且可重试；循环用例同时禁止 allocator 分配。四组变异分别破坏水平偏移、父 ID 命名空间、祖先验证及父子关系，
要求目标红、同库独立对照绿、回滚 SHA256 一致。

本切片只有无 UI 资源 codec，未验证 relative 在 full Metal surface 的构造时恢复；
loading/animation/pending、跨包绑定、页面预算和生产恢复事务仍未交付。默认退出 3。

## 0012：观察 full surface 的完整资源图

0012 叠在 0011 后，增加仅供实验的资源图导出入口。在 renderer mutex 内读取实际
surface terminal；只有 replica 且语义状态有效时才允许导出，大小限制为 16 MiB。
该入口用来证明 full Metal 构造确实保留了 relative 关系，而不是仅能比较忽略图像的
canonical snapshot。它不把分开的 canonical/resource 导出变成一致 checkpoint。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-relative.py --mutations
```

权威无 UI 夹具先通过 VT API 验证单层/多层/父节点后置/内部父 ID 的位置，再保存
canonical 和资源图；full surface 在增量前、绘制调用后以及删除父节点后分别逐字节对拍
实际资源图。static 用例作为独立对照。另有 import-only 对照不启动 App tick。
四个变异分别丢失偏移、破坏父 ID 命名空间、允许 GUI 提前 reflow、删错父节点，
必须目标红、同库对照绿、回滚复绿。资源图对拍不是屏幕像素断言。

运行 App tick 暴露原型原有的尺寸 owner 缺口：恢复的 80×24 会被 GUI 的首个
resize 改成 99×35，canonical 快照从 1157 变成 1227 字节。0012 因此还限制
replica 的本地 resize：仍经 backend 向权威提出请求、更新 renderer 的视图尺寸，
但不自行修改 terminal 的逻辑网格。原有直连路径继续执行 resize。该保护不等于
完整尺寸事务；权威接受 resize 后的有序回传/应用接口尚未交付，不能用于生产重连。

默认完整运行仍退出 3；未覆盖 loading/animation/pending、跨包绑定、总内存预算、
并发恢复事务或 GUI 崩溃重连，也没有将实验入口加入生产构建或固定恢复 ABI。

## 0013：权威输出与 resize 的同步有序应用

0013 叠在 0012 后，增加仅用于 P0 的 output/resize 事件入口与 applied sequence
查询。序号从 1 开始，必须逐个连续；跳号、重复和 0 在修改状态前拒绝，不消耗序号。
事件在 renderer mutex 内真正执行 parser 或 terminal.resize，成功后才发布序号；
输入 paste 编码也在该锁下读取模式，已应用与仅收到字节不再混淆。这里不确认 GPU
绘制完成，也没有定义 IPC 或生产恢复 ABI。

权威 resize 显式携带 cols/rows/cell 像素尺寸，允许在 0012 禁止 GUI 自行 reflow
之后应用接受的尺寸。空尺寸、像素乘法溢出及超过 1 Mi cell 的实验网格直接拒绝。
resize 内部失败时标记语义失效，避免导出潜在部分状态；该错误路径尚无 allocator
故障注入证据，不能表述为完整分配失败验收。已开始有序事件后，旧的原始输出入口
会将副本标记为失效；canonical/资源图导出、序号查询和后续事件均不得假装成功。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-events.py --mutations
```

无 UI 权威生成输出、resize、交错序列的各步 canonical/资源图。full surface 逐步
对拍，运行 App tick 后保持接受的权威尺寸；查询 applied sequence 后立即输入，
核验 bracketed-paste 开/关对应的真实 host_write 字节。另测跳号/重复/零序号与
非法尺寸拒绝后状态不变、可继续接受正确事件，以及原始输出绕过后的显式失效。
七组变异护 resize/output 真的应用、序号校验/发布、原始输出失效、输入模式读取
和非法 resize 不得假成功，每组均有独立对照及 SHA256 回滚。

序号仅属于当前实验 surface，初始为 0；没有 attachment generation、checkpoint
序号绑定、输入请求标记或并发捕获事务。GUI resize 请求的接受策略、真实 IPC、
loading/动画、总内存预算及崩溃重连仍未完成，默认退出 3，P0 保持未通过。

## 0014：checkpoint 成对绑定与 surface generation 隔离

0014 叠在 0013 后，增加显式 bound checkpoint 构造元数据。SHA256 输入依次为
域串 `GHOSTERM-P0-CHECKPOINT-1`、little-endian u64 的 generation/sequence/源图像
额度/canonical 长度、canonical 原字节、资源图长度及资源图原字节。构造在创建
core surface 前检查摘要与必要部分；generation 必须非零。未启用 bound 却携带
元数据时明确拒绝，避免静默忽略。摘要由 Python hashlib 独立产生，非引擎自证。

generation 和起始 sequence 传入最终 Termio，事件与序号查询在状态锁内校验
generation；错误 identity 不消耗序号、不污染仍可用的当前副本。无 generation
的旧实验事件/查询仅能用于 generation=0 的未绑定实验，不能绕过已绑定实例。
bound sequence=0 时也禁止原始输出绕过，绕过后沿用语义失效标记。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-checkpoint.py --mutations
```

夹具从权威同线程生成的静态状态中选取一对快照，指定实验 generation=81/S=40。
刻意混入另一份合法 canonical、CRC 有效的蓝色图像资源、错误 generation/sequence
或摘要时必须拒绝构造；同一蓝色资源重算摘要后必须接受并对拍实际资源图。
合法实例从 S=40 继续应用 41/42，逐步 canonical/资源图/序号对拍；错误 generation、
旧入口与 sequence=0 原始绕过均有独立用例。八组 native 变异覆盖摘要校验、事件/
查询 generation、起始序号、零序号原始入口、未绑定元数据、零 generation 和旧入口，
均要求目标红、同库独立对照绿及 SHA256 回滚。

这是 surface 侧身份与成对内容校验，不是 daemon attachment 生命周期或 IPC 认证。
generation 目前由调用方指定，摘要不证明来源，也不能证明两份快照在并发输出中
原子捕获；同线程夹具的 S=40 是实验元数据，不是生产 worker 的事件日志证据。
未实现输入字节的 generation/已应用序号标记、总内存预算、loading/动画或生产恢复
事务，默认仍退出 3。旧实验入口保留仅用于先前原型，不作为生产兼容层发布。

## 0015：输入在编码点保存 generation/sequence/linefeed

0015 叠在 0014 后。普通按键在读取 key encoding options 的同一状态锁内保存标记；
粘贴在读取 paste options 时保存一次，所有分片共用该标记。按键序列的等待队列保存
完整编码请求和标记，不能在 `endKeySequence` flush 时追补。鼠标/提示符点击等已
持锁排队的路径在该锁内保存；`cursor_key` 将模式选择与排队放在同一锁内。
不依赖检查转义字节来区分输入与回复；title/OSC52/Kitty reply 的宿主回复来源对
bound replica 明确报错，用户的 `csi:6n` 原始绑定仍可发送。

IO mailbox 使用独立拥有 payload 的 stamped 请求，保留 generation、编码点 sequence
和 linefeed。Host 按保存的 linefeed 转换 CR→CRLF，每个输出分片都传递同一标记。
drain 时只检查身份和序号范围（不早于 checkpoint、不晚于当前已应用序号），允许
同一 generation 内编码后才到达的新输出；不能把旧输入改标成较新序号。回调在锁外
调用。缺少带标记回调、未标记写入或非法标记会显式使副本失效，不降级到普通写入。
未绑定的旧实验/直连路径保留原语义。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-input.py --mutations
```

测试 hook 只用于已编码并释放状态锁、尚未排队的边界观测，不在锁内调用：探针让引擎从
S=40 推进到 41，再让旧编码请求进入 IO 队列。十五类原生用例观察真实写出字节和
每个分片的 generation/sequence，包括普通键、延迟按键序列、短/长粘贴、跨 4096
字节的 linefeed 分片、鼠标、cursor 动作、回复来源、只读、缺失回调和非法请求。
另跑上游过滤单测验证队列尺寸与只读释放。内部 Message 从 40 增为实测 72 字节，
明确更新尺寸预算；它不是 IPC 布局，也未证明总内存预算。
探针使用随本轮临时目录清理的独立 full-cache；空间低于 6 GiB 时仅回收本轮
完整引擎产物并失效其缓存索引，保留依赖产物。共享构建 helper 的默认缓存策略不变。

十五组 native 变异覆盖编码点标记、延迟/分片保留、linefeed、来源与错误拒绝；
另两组分别改坏消息预算和丢弃请求的释放，要求目标单测红、独立对照绿、SHA256
回滚。只读释放用 testing allocator 观测泄漏，不以“没有写出去”冒充释放证据。

该证据仍限于实验 surface 的输入封装与校验；没有 daemon worker/attachment 管理、
真实 IPC 输入验证、原子捕获或真实 GUI 重连。Kitty clipboard 授权动作、其他宿主
副作用与所有平台 UI 接线仍须逐项验收；不能把此切片称为全量输入/副作用恢复完成。
loading/动画和总内存预算未完成，默认退出 3，P0 保持未通过。

## 0016：UI reset 交由权威事件应用

额外诊断复现：bound replica 的 UI `reset` 原先直接 `Terminal.fullReset()`，将
bracketed-paste 清掉，却仍报告 applied=40；随后的 USER 只有 4 字节而非原模式的
16 字节，输入标记与模式已不再对应。0016 因此把 bound UI reset 改成低频动作请求：
携带 generation、GUI 已应用序号与 kind=1，回调在状态锁外执行。true 只表示请求
已排队，不表示状态已应用；回调缺失显式 unsupported，拒绝不修改当前副本。

权威通过 `event_reset_for(generation, sequence)` 在既有有序事件接缝中执行 reset，
成功后才发布新序号。复用 pin 中 fullReset 的语义：保留 stream parser continuation，
与无 UI `ghostty_terminal_reset` 一致。未绑定旧实验仍使用原来的本地 reset。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-reset.py --mutations
```

七类原生用例覆盖只排队、同步回调应用、拒绝、缺失回调、旧 generation、半截 CSI
及未绑定对照。无 UI 权威产生 reset 前后 canonical/资源图，副本在真正接受事件前
必须保持旧模式/旧序号，之后清图像并使用新模式/新序号。同步回调内查询状态并应用
事件，证明回调不在状态锁内；动作请求不能泄漏为终端输入字节。七组变异护上述
接缝，每组均要求目标红、同库独立对照绿、SHA256 回滚复绿。

该动作回调仍是实验注入，尚无真实 worker/IPC；不覆盖 clear_screen、配置等其他
可改变 VT 状态的 UI/宿主动作，也不代表全量副作用或恢复事务完成。原子捕获、
loading/动画、总内存预算和 GUI 重连仍待交付，默认退出 3，P0 保持未通过。

## 0017：清屏由权威状态应用，提示符重绘只有一个 owner

`clear_screen` 会修改历史、光标周围的网格及图像状态；提示符处还会写 FF 请求 shell
重绘。bound GUI 不能自行改 VT，也不能与权威端重复发送 FF。因此该实验增加 kind=2
动作请求和有序 clear 事件：请求只排队；事件携带 history 与权威实际 outcome，
副本执行同一纯状态 helper 并验证 outcome 后才发布序号。分歧使副本显式失效。
FF 只由无 UI `ghostty_terminal_p0_clear` 的 authority 入口发送；旧未绑定路径保持
原来的行为。备用屏幕保持 no-op，但已提交的事件仍推进序号。

原生基线另外发现 pin 中 `PageList.eraseActive` 假设删除范围从页面第 0 行开始。
导入后的页面同时容纳历史与活动区时，保留历史的 clear 在 `chunk.start == 0`
处崩溃。0017 修正区间删除：保留前缀，压紧后缀并调整 pins，重置回收行，也允许
移除非首尾的整页。新增单页及跨多页回归对拍历史、尾部内容、空白 regrow 行和
tracked pin 位置。删除图片沿用上游 visible-only 语义，保留历史时可能保留图片；
不把“所有图片都消失”当成正确的验收期望。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-clear.py --mutations
```

探针覆盖普通/提示符/备用屏幕/保留历史、拒绝/缺失回调、旧 generation、outcome
分歧及两种未绑定对照。变异分别破坏本地动作隔离、实际应用、历史/图片删除、FF
唯一 owner、outcome/generation 校验、共享历史页删除和动作 kind；另单独破坏
authority FF，以冻结夹具的 GUI 对照证明指向性。每项要求目标红、独立对照绿、
SHA256 回滚与复验。页面修复的新回归、既有删除回归和十类原生基线已通过；
十组变异及最终回滚也已通过；页面删除过滤集共 103 项通过。

本补丁仍是隔离原型；没有真实 worker/IPC、原子 checkpoint、全量宿主动作接线、
loading/动画与总内存预算，也不证明 GUI 重连。P0 保持未通过。

## 0018：保存尚未完成的图像分块传输

VT parser 回到 ground 不代表 Kitty 图像已完成：多个完整 APC 可以共同组成一条
`m=1` 传输，已收字节、初始 display、quiet、应答标识和压缩方式仍留在 LoadingImage。
仅复制 parser continuation 或最终 ImageMap 会丢掉这些状态。0018 增加专用 loading
codec，并将实验资源格式从 GTIR0002 改为 GTIR0003；不提供旧实验格式兼容。

每个 screen 在 image/placement 图之后编码可选的 LoadingImage。字段逐一编码为
有界整数/字节串，不复制 C/Zig struct 的内存布局。支持原始 RGB/RGBA、PNG、zlib、
初始 display（含有符号偏移）、quiet、response 标识与 implicit_id；保留已推进的
隐式 ID 候选计数。源路径和回调指针不进快照，临时目录上下文从目标创建策略取得。
先完成全记录的 preflight，再分配 loading 对象和已收 payload，最后与资源图一起
发布；分配失败销毁暂存对象，保留目标原状态，允许重试。

动画帧 loading、动画 Image 和异步 pending 仍显式拒绝，不序列化成空图或完成图。
快照总线长受调用者 max_bytes 限制，单条已收图像分块另受上游 400 MiB 上限约束；
这不等于实现会话总内存预算，也不证明并发原子 checkpoint。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-loading.py --mutations
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-loading.py --mutations
```

无 UI 基线已验证原始/压缩/PNG、quiet、无 display、半截 APC、双屏、隐式 ID 及
后续分配、动画帧拒绝、线长限制、非法枚举和两处分配失败后的回收/重试；另有静态
图像及动画拒绝对照。十一组无 UI 变异、四组 full surface 变异及最终回滚复验均已通过，
含隐式 ID 的十一类原生用例全部通过。
full surface 夹具还包含同 ID 新传输开始即删除旧图、其他 ID 的已完成图并存及
无 loading 的独立对照；由同一权威实例产生初始与续传后的 canonical/资源图。

仍无生产 daemon/IPC、并发原子捕获、动画/pending 恢复或总内存预算；P0 保持未通过。

## 0019：加载记录必须有非零身份，非法记录不得先分配

上游在把 LoadingImage 放入 screen 之前就分配非零图片 ID（隐式 ID 也已分配），
因此 wire 中 id=0 不是合法加载状态。接收它会把后续完成路径变成重新分配身份，
与捕获的协议状态不一致。0019 在读取加载字段后立即拒绝 id=0，不改格式或生产 ABI。

新增验证夹具在加载对象前放一张已完成图片与 placement，再对有正确 CRC 的记录
分别注入零 ID、非法 presence/format/compression/flags/quiet/display/cursor/virtual、
超宽整数、超大尺寸/长度及截断。每种错误既要返回对应错误，又必须在 allocator
调用前发生，目标 canonical/资源状态保持不变。已有图片让“跳过完整 preflight”
真正触发分配后再失败，防止只测空图时的假绿。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-loading-validation.py --mutations
```

十三类非法记录和独立静态对照已通过基线，补丁后的 full surface direct 控制也通过。
五组变异分别破坏零 ID、完整 preflight、枚举、保留标志和尺寸校验；均已通过
目标红、独立对照绿及最终 SHA256 回滚复验。
共享 VT build helper 增加可选日志路径，原默认保持不变；本套件用独立日志，避免
并行构建覆盖失败诊断。该证据不涵盖动画/pending、并发捕获、总预算或生产恢复。

## 0020：动画快照与唯一权威时钟

静态 ImageMap 不足以恢复动画：需保留已组合帧、gap、当前帧、播放/加载/暂停状态、
循环预算和已完成循环数，以及当前帧显示时刻。0020 将实验资源格式升级为 GTIR0004，
逐字段编码完整动画及最后权威 tick，并以完整 preflight/暂存发布维持失败回收。
限制附加帧数为 4096、每帧像素必须恰为宽×高×4，所有帧计入源图片存储额度与记录限额。

运行探针发现 `ImageStorage.addImage` 原本只按基础 data.len 计费：完整动画导入后
会漏算帧，下一次导出便与 total_bytes 不一致。改为 storageSize 后基础图和动画帧
共同计费。无限循环计数改为饱和累加，以免导入的合法 u32 最大计数在下一轮回绕。

时钟由无 UI `ghostty_terminal_p0_animation_tick` 推进，返回下一次延迟（无 deadline
为 UINT64_MAX）。倒退时间显式拒绝且不改变状态。副本在既有 generation/连续序号
接缝中应用同样的 tick；导入策略设置外部时钟标记，renderer 不再自行推进它。
外部时钟策略不属于协议状态，最后 tick 属于资源 checkpoint，二者分别处理。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-animation.py --mutations
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-animation.py --mutations
```

无 UI 基线覆盖运行/暂停/加载态、有限循环、gapless、只有根帧、未放置动画、
parser continuation、最大循环计数及八处分配失败的回收/重试，并拒绝帧数、播放状态、
当前帧、像素长度、clock presence 和宽整数的非法记录。十一组无 UI 变异及最终回滚
复验已通过。
原生夹具包含双屏与 output/tick 混排、旧 generation、倒退时间、import-only 与
clock-only 对照；十类原生用例、五组原生变异及最终回滚复验全部通过。
资源/规范状态不是像素截图。

动画帧的未完成 loading 和异步 pending 仍拒绝；没有真实 worker/IPC、并发原子捕获、
总内存预算或 GUI 崩溃重连，P0 保持未通过。

## 0021：未完成动画帧的恢复与资源身份重绑

FrameContext 保存起始命令和目标 image generation。直接复制该 generation 会引用
另一个实例的缓存身份；无条件改成新 generation 又会复活捕获前已失效的上传。
GTIR0005 因此保存“捕获时引用仍匹配”这一事实：匹配时在导入后重绑实际目标，
已失效时保持不可能匹配的零 generation。最后一块数据仍走上游真实完成/错误路径。

新增帧命令 codec 逐字段保存 transmission、edit/create frame、gap、合成方式和
背景颜色，quiet/应答标识仍由 LoadingImage 保存。帧按 image number 引用时，
原始 image.id 可以为零，实际目标由 number 解析；普通 image loading 继续要求
已分配非零 ID。preflight 用图片 ID/number 表校验匹配引用，之后才分配对象/载荷。
不序列化路径、函数指针或缓存 generation；动画帧和静态资源统一事务发布。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-frame-loading.py --mutations
```

二十八类无 UI 基线通过：追加/编辑、按编号、合成/背景、PNG/zlib、quiet、负 gap、
parser continuation、捕获前失效/恢复后 tick 失效/目标被额度淘汰，以及静态对照、
九处分配失败回收/重试与十二类畸形记录分配前拒绝。十四组无 UI 变异及最终回滚均已通过。十五类 full surface 基线通过，
六组原生变异及最终回滚均通过，覆盖续传、有效/失效引用和副本不得写出应答。异步 pending、原子捕获、总预算和生产恢复
仍未交付，P0 保持未通过。

## 0022：pending 像素与权威生产者身份

PendingImage 的预留长度不是像素。GTIR0006 为每张图片显式保存 complete/pending
判别和非零 authority origin；pending 不携带伪造的零字节载荷，但仍占用完整像素
额度。导入时保留 origin，另外分配本地 renderer generation。完成只接受匹配的
origin，随后通过本地 PendingImage token 接管真实像素；不会改变图片 generation，
因此此前有效的动画 frame loading 引用仍然有效。取消、删除、替换或重复完成的
晚到结果明确返回 applied=false，不覆盖新图片。尺寸/长度和额度失败明确报错。

实验 surface 添加有序 begin/ready/cancel 事件，受现有 generation/sequence barrier
约束，并验证副本结果与 authority 的 applied 结果相符。结果不符使副本失效，
不发布已应用序号。这些仅是 P0 实验 API，未修改正式 ghosterm C ABI。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-pending.py --mutations
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-pending.py --mutations
```

延迟生产者探针用 pthread 条件变量阻塞真实 PNG 解码，在捕获/导入或取消后才释放，
对比权威与副本的规范状态、资源字节和像素。生产线程不跨线程调用 Terminal，
解码内存由自身 allocator 回收，结果由 owner 提交。上游正常 stream 解码仍同步；
本补丁没有将生产解码器改造成异步任务，也未交付真实 worker/IPC、原子捕获、
全局内存预算或 GUI 重连。当前 pending 事件只作用于活动 screen，跨 screen 的
异步生产者路由仍待补齐，不能宣称 pending 生命周期已完整交付。P0 仍未通过。

0022 运行证据：十六类无 UI、九类原生 surface 基线通过；五组无 UI、四组原生
变异均有目标红/独立对照绿与最终回滚复验。原生额外验证错误 generation 不推进
序号、完成结果不符使副本失效。核心 gate 9/9、57 个公开符号对拍与快速构建通过。

## 0023：跨 screen 的 pending 生产者归属

0022 的完成和取消按当前活动 screen 查图片，程序进入备用屏后返回的主屏解码结果
会被丢弃。同一个图片 ID 可同时存在于两屏，不能用 ID 猜测归属，也不能为处理结果
临时切屏，否则会更改终端状态。0023 在两个已初始化的 screen 中按 authority origin
解析原始生产者，再核对图片 ID；不改 active screen、光标或模式。

取消使用 PendingImage 的本地 generation 复核，显式传原 screen 释放 placement pins、
相对后代和资源并标记变更。导入 preflight 在分配前拒绝整个快照中的重复 pending
origin；有序 begin 也拒绝复用仍存活的 origin。格式仍为 GTIR0006，无正式 ABI 改动。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-resource-pending-screen.py --mutations
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-surface-pending-screen.py --mutations
```

这仍是隔离引擎证据，尚无真实异步 worker/IPC、原子捕获、完整内存预算和 GUI 重连。
P0 仍未通过。

0023 运行证据：九类无 UI、七类原生 surface 基线通过；三组无 UI、两组原生
变异均完成目标红/独立对照绿、SHA256 复原和最终全量基线复验。0022 十六类
pending 用例回归通过。核心 gate 9/9、57 个公开符号及快速构建通过。

## 0024：显式原生页面供给器

PageList 默认直接使用 OS page allocator，C allocator 计数看不到这些 mmap 页面。
不能将它替换为普通 malloc：池内页面要能独立 decommit/recommit，ReleaseFast 还
依赖新映射清零。本补丁增加只属于宿主策略的可选 page allocator，要求提供零填充、
页对齐、支持原生页面操作的 OS 映射；描述符和 context 活到所有终端/克隆销毁。

策略从 Terminal/Screen 进入 PageList，覆盖创建、快照 TERMINAL 临时屏、SCREEN
builder、历史页、备用屏懒创建/重建与克隆。不序列化 allocator、指针或预算。
C 实验入口可创建显式供页终端，或在 decoder 开始前设置供页器；开始解码后冻结。
默认策略保持上游 OS allocator。正式 ghosterm.h 与资源/规范快照格式未改变。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-page-provider.py --mutations
```

探针以真实 mmap/munmap 提供页面，按实际页粒度向上取整计费，包括预热池未用容量。
创建用量、精确额度、少一字节拒绝、恢复和备用屏预算拒绝均由该供给器观测；这
是保守的映射额度，不是 RSS 测量。恢复逐页分配失败与销毁对拍映射/解除映射字节。
Zig 克隆测试用 OS page allocator 的 FailingAllocator 验证拒绝及完整计费回收。

本补丁尚未把 full GUI surface 的 allocator 接入这套预算，也未组合一般分配、
像素/动画、快照临时副本和 OS 页的统一全局额度。上游某些 resize 恢复路径会在
分配失败后降级到主屏；生产 worker 仍需将预算拒绝变成显式会话资源错误，不能
把供页接口等同于完整错误事务。并发原子捕获和生产恢复仍缺失，P0 保持未通过。

0024 运行证据：八类无 UI 基线、十六个页面 OOM 点与克隆定向测试通过；六组
变异均完成目标红/独立对照绿、SHA256 复原及最终全量回滚复验。默认 full surface
七类恢复回归通过，核心 gate 9/9、57 个公开符号和快速构建通过。

## 0025：快照临时分配与源终端隔离

原 C encode_alloc 只把最终输出交给调用方 allocator，continuation、record scratch
及压缩页检查仍借用源终端 allocator。共享额度下，恢复任务 OOM 会污染权威 scope。
新增两个 P0 显式分配入口，把 canonical/continuation/资源 wire 的分配全部交给
scratch allocator；压缩 SCREEN/HISTORY 检查跟随 record writer 的 allocator，
PreservedPage 仍不改变源页面表示。原有 API 的默认分配语义保留。

私有 checkpoint-budget 适配器要求 source/scratch 是同一账本下的不同 scope；
先编码到局部 staged 对象，两份完整且通过合计 wire 上限后才发布。任一阶段失败
释放全部暂存缓冲区，输出保持空，保留阶段和原始错误码；源健康与任务失败分开。
输出归 scratch scope，必须在关闭 descriptor 前用对应 allocator 释放。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-scratch-checkpoint.py --mutations
```

调用者仍须在整个捕获期间独占源状态；这里没有并发原子/COW 捕获。原生 GUI 的
自定义预算、worker/IPC 接线、活动会话增长余量预留和性能校准也尚未完成。
普通 scope 是按需计费而非额度预留，任务成功占用全局额度后仍可能与后续会话增长
竞争；不能据此宣称所有现存会话都有增长保证。P0 仍未通过。

0025 运行证据：十四类无 UI 基线与压缩活动页定向测试通过；五组任务额度阶梯
扫描覆盖 canonical 和 resources 两阶段拒绝，未发布半份输出。十组变异均完成
目标红/独立对照绿、SHA256 复原及最终全量回滚复验。默认 full surface 七类恢复
回归通过；核心 gate 9/9、57 个公开符号及快速构建通过。

后续宿主适配器已增加 owner grant 预留：checkpoint 的 source/task 现在均须先经
`p0_budget_scope_admit` 接纳。未使用的增长空间计入 committed，任务不能占用它；
free 不归还 grant，close 才归还。十类基线、十二组变异及最终回滚通过，详见
`terminal/session-state/README.md` 的峰值额度预留章节。该能力不修改 0025 引擎补丁。

## 0026：不可变副本与延后编码

上游 Screen.clone 服务只读显示，会省略部分恢复状态，不能冒充完整终端 clone。
本补丁复用 canonical 编码器捕获完整 TERMINAL/SCREEN/continuation 元数据，复制
原生页面的自含 offset backing；压缩页只复制元数据和 LZ4 块，不在 owner 上解压。
清除压缩页元数据的源 memory slice，后台以独立缓冲区解压。所有片段归 capture
allocator，不保留 Terminal/Screen/Node、字符串、continuation 或资源的借用指针。

资源捕获仍复制完整 GTIR0006 payload，但把 CRC 完成推迟到后台输出的独立副本。
后台逐页编码和检查合计 wire 上限，分配失败不交出半份输出；捕获和编码的 allocator
可分别计费。冻结对象只读，编码不改它；销毁必须等所有编码调用结束。该机制是
独立复制，不是 COW，规范和资源 wire 格式不变。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
python3 scripts/probe-session-frozen-checkpoint.py --mutations
```

并发探针让真实 encoder 在 allocator 回调中停下，owner 继续输入、重置、销毁后
才放行，结果必须逐字节等于捕获前的 canonical/resources。包含 CJK、组合字与 ZWJ、
超链接/标题/cwd、continuation、分块加载、压缩历史和 16 MiB 图像。分别按 OS 页
扫描捕获/编码额度并验证回收、重试，另验证开始编码前取消、copy/wire 上限。

捕获时调用者仍须独占源；复制成本受源/任务额度及 copy 上限约束，未做生产性能
预算校准。实测 owner 时间和两阶段映射峰值保存在探针日志，不用单次样本冒充
延迟上限。真实 session event S 边界、取消任务调度、GUI 预算及 worker/IPC 仍缺失。
P0 保持未通过。

0026 运行证据：十七类无 UI 基线通过，含双屏同 ID 图片及原屏/备用屏历史顺序；
四组捕获/编码额度阶梯扫描验证回收与重试。十一组定向变异均完成目标红/独立
对照绿、SHA256 复原和最终全量回滚基线。默认 full surface 七类恢复回归、核心
gate 9/9、57 个公开符号及快速构建通过。

## 0027：surface 宿主预算

为恢复用 full surface 注入 general/page allocator 与 sticky health 回调，使预算失败可阻止序号发布和输入。共享 Metal Device 必须由 App allocator 管理；共享字体 fallback 缓存也必须由 SharedGridSet 的 allocator 管理（`SharedGrid.owner_alloc`），否则 surface allocator 分配、App allocator 释放，UTF-8 回退字形会触发 SIGSEGV。独立 SharedGrid 保持调用方 allocator 行为。

此补丁只覆盖每 surface 的 CPU 与原生页预算，不含 App、共享字体、系统或 GPU 内存。另有一条宿主契约由本轮实测确认：`ghostty_surface_free` 之后预算不会立刻归还，必须让 app 继续 tick 才会回到 live=0 且 mapped==unmapped，纯 sleep 300ms 无效；探针因此按 tick 轮询（上限 100 次）后再断言账本平衡。

0027 运行证据：十五类 full surface 基线通过——七类完整恢复、import-only 导入对照、gui-default 与 gui-default-utf8 两类默认 allocator 对照，以及 gui-denied/gui-pages/gui-partial/gui-growth/gui-revoked 五类预算故障。十一组 full surface 变异、十一组共享预算变异、六组引擎预算变异（general 绕过、页 provider 绕过、sticky 故障忽略、共享 Device 记账、provider 不完整、字体缓存归属丢失）均达成目标红/独立对照绿，并以 SHA256 校验最终回滚。

字体归属变异把 `grid.owner_alloc` 改回 null 后 utf8 以信号中止，而同样输出 UTF-8 的 gui-default-utf8 仍为绿，红因此落在所有权而不是 UTF-8 处理；它是唯一允许目标以非零（可能是信号）退出的变异，超时与构建失败一律不算通过。gui-growth 的增长杠杆由 OSC 标题改为回滚行：标题增长的是共享字体缓存，归属修复后它已正确不在每 surface 预算内，继续沿用会让该场景失去牙齿；现在冻结 general 额度后写入 4096 行，首次行增长即被拒绝并置 sticky 故障。

核心 `scripts/gate.sh` 4/4（45 项 CTest、57 个公开符号）、`scripts/build-core.sh`、`scripts/gate-ghostty-patch.sh` 9/9 及 VT 十类场景/十四组变异回归通过。P0 保持未通过：GUI 全局预算、其余事件源、IPC/生产 manager 与 GUI 重连仍缺失，同进程组合探针不是像素验收。
## 0028：恢复页的 reflow 余量

PAGE 头只带逻辑行数，不带行容量。按逻辑行数精确分配，恢复出来的页一开始就是满的：
detach 期间的一次 resize 必须把它拆成两页，而同样内容的原生页还留着标准容量余量、
原地 reflow 完成。两边文字完全一样，canonical 字节却不相等——实测主屏 100 行
scrollback、80→91 列时权威是 `PAGE(1227)`，副本是 `PAGE(1188)+PAGE(63)`。
`decode→encode` 本身幂等，所以再规范化一遍也救不回来：差异来自分配容量，不是编码。
之前的用例都只写几十字节、没有真实 scrollback，页远没装满，所以一直没撞上。

补丁把行容量补到同样列宽下原生页会拿到的标准容量（其余容量字段仍按 header 的原生
提示），并让 `decodePayloadBody` 从 header 取逻辑尺寸而不是从 `init` 的容量取——
少了后一处，上游 snapshot 测试会以 EndOfStream 红（grid 按 `page.size.rows` 解码），
这就是它自带的回归证据。编码只写逻辑行数，补容量不改变再编码的字节，wire 格式不变。

代价：恢复的页按标准页大小分配，和原生页一致，不再是“刚好够装”。恢复侧内存因此
与原生对齐，仍在 0027 的每 surface 原生页预算内计费。

0028 运行证据：上游 `zig build test -Dtest-filter=snapshot` 与 `-Dtest-filter=terminal`
通过。VT 十二类基线（新增 tui-less/tui-vim）、十五组变异与最终回滚通过；full surface
十七类基线、十二组 full surface 变异、十一组共享预算变异、七组引擎变异与最终回滚通过。
新增引擎变异 `restored page without reflow headroom` 把容量补足改回精确分配，tui-less
红、continuation 绿——没有真实 scrollback 的用例压根走不到那次分裂，这正是它的边界。
核心 `scripts/gate.sh` 4/4（45 项 CTest、57 个公开符号）、`scripts/build-core.sh`、
`scripts/gate-ghostty-patch.sh` 9/9 通过。P0 保持未通过。

## 0029-replica-focus-input：焦点报告是用户输入，不是宿主动作

0004 为了挡住副本直接写权威 PTY，把 `Termio.focusGained` 整个提前返回。鼠标报告
没受影响——它走 `write_small`，被 0015 在 `Surface.queueIo` 里改道进 stamped
replica input；焦点报告却是 `.focused` 消息，不是 `write_*`，于是被整条丢掉。
结果是程序开了 `?1004h`、用户切到恢复出来的窗口，`CSI I`/`CSI O` 永远收不到。

焦点是用户输入，不该由「副本不许写」兜底。0029 把提前返回换成分支：副本模式下
照样读 `focus_event` 模式位并编码，但不调 `queueWrite`，而是取同一个锁内快照的
stamp，按 `queueReplicaInput` 交回权威——和鼠标走同一条通道、同一套 generation/
sequence 校验。`backend.focusGained` 仍然跳过，0004 「副本不碰 backend」的意图不变。

stamp 与 `focus_event` 的读取放在同一次持锁里：分两次取，模式位和代际就可能跨
一次 attach 切换，交出去的字节会挂在错误的一代上。

0029 运行证据：full surface `input` 用例在真实 Metal surface 上验 `CSI O`/重复
设同值零字节/`CSI I` 三步，全程 legacy 写与宿主动作计数为 0。引擎变异把副本分支
改回丢弃，`input` 红、`continuation` 绿。P0 保持未通过。

## 0030-replica-scheme-barrier：配置应用确认不得提前推进序号

上游 surface 的 updateConfig 在主线程派生配置，再异步提交 renderer/Termio。
普通 surface_set_color_scheme 只触发条件重载；返回时不能承诺 I/O 状态已应用。
恢复副本若把这个返回当作 applied，就可能让后续输出或输入跨过尚未更新的配置。

实验接口 scheme_begin_for 仅接受连续事件并保留旧序号；scheme_apply_for 必须在
宿主主线程执行，提交携带该序号的配置消息。Termio.changeConfig 在锁内校验精确
消息、主题与预算，实际更新默认 palette/background/foreground 等状态后才发布
新序号。配置派生失败、错误标签或主题不符使副本失效；绑定副本拒绝普通未标记配置；它既不能冒充
正在等待的事件，也不能在事件流外改状态。未使用条件配置时，上游可能不更新 conditional state，故标签
消息仍显式携带权威主题，不能把“没有主题条件”解释为主题未知。

等待期间后续有序 output/resize 立即拒绝，不消费序号；新编码输入的 stamp 标记
blocked 并丢弃，不排队重放、不把可预期的阻塞当作流损坏。先前已经编码的输入
仍保留其原 stamp。焦点的 I/O 编码路径和宿主动作请求也遵守该阻塞。没有 cancel
接口，调用方超时后必须作废连接，不能自行跳过事件继续 LIVE。

这是实验 full surface 引擎接缝，不是生产配置 API。确认范围是 I/O 配置与终端
状态实际应用，不承诺 renderer 完成绘制，也不代表任意字体或配置变更已经具备
权威事件模型。主进程 IPC 的配色能力仍禁用；一致快照中的外部配置、增量载荷和
GUI 宿主调度仍须另接。公开 core ABI 与上游 vendor 均未修改。

0030 运行证据：`python3 scripts/probe-session-worker-surface.py --scheme-barrier`
七项变异均目标明确退出 1、普通 continuation 对照退出 0，SHA256 回滚及两条
原生基线复绿。实际验证四次主题事件（含同主题）、I/O 主题与终端背景 0x123456、
待应用输入/输出/resize 隔离和无序重载拒绝；日志 `tmp/scheme-barrier-verification.log`。
同一最终构建的十三类原生进程场景与 26 类旧 surface 基线通过；核心 gate
113/113、57 个公共符号通过。尚非配色 IPC、绘制完成屏障或生产交付，P0 未通过。

## 0031-replica-checkpoint-scheme：外部查询配置必须随 S 恢复

VT 规范快照不持有宿主 COLOR_SCHEME 回调的外部主题值。只有终端网格而没有
同一序号的外部配置，新 GUI 会按自己的全局外观初始化 I/O；随后重放查询或
配置增量时两边状态可能分叉。0031 在 bound checkpoint 构造参数中加入可选
主题，域分隔后把主题纳入原 generation/S/规范状态/资源摘要。翻转主题但保留
摘要、携带无绑定主题、或 dark 没有显式设置标记均拒绝。旧档默认零初始化仍
保留原摘要算法，不静默把缺失主题解释为某种已知主题。

构造时从捕获主题推导 Surface 条件状态及 host I/O conditional state；即使上游
认为配置未用主题条件而跳过派生，I/O 仍使用捕获值。显式主题派生失败返回错误，
不回落到 GUI 全局配置。增量继续使用 0030 的实际应用屏障，0031 不偷消费 S+1。

运行证据：进程 `--scheme-mutations` 十三项变异（含构造 I/O 主题落回全局、跳过
主题摘要校验两项引擎变异）均 scheme 退出 1、roundtrip 退出 0，SHA256 回滚和
基线复绿。固定实验档的 dark 快照、S→L light、L 后 dark、LIVE light 最终与
源 VT 完整 canonical/resources 逐字一致，两次真实查询各有唯一正确应答。
十四类原生进程与 26 类旧 surface 复验通过。日志
`tmp/process-scheme-verification.log`、`tmp/process-scheme-compat.log`。
这不是任意产品主题配置、绘制完成屏障或生产 IPC；P0 未通过。

0031 后续像素验收：scheme 在初始快照、三次配色增量、LIVE 与终态六个时点
验证真实背景像素。`--scheme-pixel-mutations` 四项均 scheme 退出 1、roundtrip
退出 0；含隔离 renderer RenderState 锁在旧色（I/O 状态保持正确）的真实渲染
变异，以及混入 256 个旧色像素、主体仍为新色的残留检查。SHA256 回滚与原生
基线复绿，十四类进程和 26 类旧 surface 复验通过。无新增持久引擎补丁；临时
renderer 变异严格复原。日志 `tmp/process-scheme-pixel-verification.log`。


### 0032：full surface 原生线程栈观测

P0 的 CPU/page allocator 未覆盖 Zig pthread 栈。renderer 与 I/O 均用
SpawnConfig 默认栈（当前 Zig 0.16.0 为 16 MiB）。提供只读默认大小及 Darwin
活线程 handle 查询，让宿主在 surface 构造前承诺容量、构造后核验真实栈和
VM guard。handle 只在当前进程、surface_free 前借用，不序列化、不作为输入
授权。其他平台 handle 查询显式 false；未改变线程实现、生命周期或生产构建。
线程入口 Args 的 libc 分配、TLS、搜索/框架线程及 GPU 等仍不在此范围，
不能据此宣称完整 runtime ledger。

### 0033：预算型 surface 的线程入口参数与失败清理

Zig 0.16.0 的 POSIX spawn 即使设置 SpawnConfig.allocator 仍使用 libc，入口
Args 持有至线程结束。预算型 Darwin surface 改用专用 pthread 启动器，入口帧
由可选 p0_thread_allocator 或宿主 general allocator 分配，保留默认栈/guard、
maybeAttachSignalStack 与线程命名；未启用预算的 surface 保留 std.Thread。
新 Thread 包装不构造或依赖 std.Thread 私有 Impl，提供统一 join/getHandle。
非 Darwin 预算线程路径显式不支持，尚未有其他平台编译或运行证据。

Surface.init 在完成 IO 对象初始化后集中转移清理责任；若第二个入口帧分配
失败，先停止/join 已启动线程，再按 renderer→线程对象→terminal 顺序销毁，
避免线程读到已释放的终端/事件句柄或遗留其入口帧。分配失败不回落 libc。
Thread allocator 单独出现时拒绝创建；所有域与回调需存活至 surface_free。
只读 allocator descriptor 查询用于核验真实分配域，不序列化地址。
TLS/备用信号栈、libpthread 内部及框架线程等开销仍未计入完整账本。

### 0034：宿主拥有的备用信号栈

预算 pthread 原来调用 std.Thread.maybeAttachSignalStack，注册 std 的 TLS 数组，
这份在用缓冲区不能由宿主 allocator 拒绝。现在按同一 std.options 大小把缓冲
放进入口 Frame，由既有线程 allocator 计费；线程工作期间注册该缓冲，返回时
先恢复旧注册，再释放 Frame。没有降低配置大小或关掉信号栈。
Darwin 对 SS_DISABLE 也校验 MINSIGSTKSZ，新 pthread 的旧配置却返回 size=0，
因此恢复禁用配置前补足最小 size，保留禁用语义；活动旧栈按原指针/大小恢复。
单元检查内核登记地址落在真实分配内、free 时不再注册、嵌套活动栈恢复。
这不证明 TLS 开销消失：标准库链接的未使用 TLS 数组及其余 TLS/框架内存仍
可能分配，不能宣称 RSS 下降或完整资源账本完成。

### 0035：搜索线程预算与失败状态撤销

搜索仍使用 std.Thread.spawn，绕开预算入口帧/信号缓冲；且入口启动失败时只
销毁 state，却把 self.search 留在 Some，后续查询/关闭可能访问未初始化的
thread。预算型搜索改用既有 p0_thread，失败时同时销毁 state 并清除可选槽。
未预算路径保持标准线程。宿主为 renderer/IO/至多一个 search 预留三份栈容量；
搜索关闭后帧/状态归还，但该 surface 的线程容量保留至 lease 释放以供重开。
第三角色只在搜索存在期间借用，end_search 立即使其失效；新增 presence 查询
让失败断言不必读取陈旧 thread。所有查询在 surface 的 main owner 上调用。
这不代表其他系统线程、TLS、框架/GPU 或全局资源账本已完成。

### 0036：App 与共享字体的宿主分配域

surface 的预算原来不覆盖 CoreApp、embedded App、配置副本及共享字体容器，
这些对象依旧用 global.alloc。runtime config 新增可选宿主 allocator，创建、
失败回滚与销毁必须用同一域；未提供仍用默认域。allocator 描述符/上下文须
存活到全部 surface 与 app_free 完成，只读查询仅观察进程内 descriptor。
这不接管原始 config、全局初始化、CoreText/Metal 内部或两个 detached 预热
线程；不能把宿主容器已计费等同于完整字体/GPU/线程资源已受控。

### 0037：App 预热的所有权与预算

原来的字体/Metal 预热由 std.Thread.spawn 后 detach，入口帧、备用信号栈和
线程栈容量均不受 App 预算管理，销毁也不能 join。预算型 App 先完成配置
克隆，再通过 p0_warmup.Group 启动两个真实工作；入口帧使用显式 thread
allocator（省略则继承 App 域），不回退 libc。任一次启动失败都 join 已启动
线程，之后销毁 runtime/core；正常 App.deinit 在释放共享状态前 join 两个。
宿主另为两份栈/guard 预留容量。观察回调区分 worker 的开始/完成与 owner
实际 join；无 App 域却配置线程选项明确拒绝。未预算路径维持原上游预热。
仅证明这些线程与宿主缓冲的准入/回收，CoreText/Metal 内部、libpthread/TLS
及其他全局资源仍不受本补丁接管，不代表完整资源账本或 RSS 改善。

### 0038：真实历史淘汰的源 owner 通知

仅比较 scrollback 行数会把清屏、reset 和 reflow 当成限额截断；且行数可能
在同一输入块内先减少再被 reset 清零。给 PageList 加可选 owner 通知，分别
挂到字节回收真正丢弃历史页之后、Limits.enforce 确实删除历史页之后。回调
只记录标记，不分配、不重入、不调用 UI；不宣称精确的逻辑行丢弃数量。
primary reset 保留 PageList 与通知；clone/快照 builder 使用默认空通知，
指针和上下文不入快照。C setter 可明确注销，借用期止于替换/terminal_free。
现有 0028 仍负责恢复页的增长余量，保证未来限额淘汰与源状态一致。
这是诊断接缝，不是 session 截断摘要或 UI 的交付；也不把页级近似限额宣传为
精确行上限。零历史策略、真实会话接线和生产构建仍须各自验证。

### 0039：原始配置的 allocator 与显式加载结果

原始 C Config 的对象、默认 arena、克隆与加载暂存原来使用 global.alloc，
App 的宿主 allocator 只覆盖后来的配置副本。新增 P0 constructor/clone，
借用的 allocator 描述符须活到所有对应配置销毁；释放先保存 arena 的 owner，
再 deinit 并归还对象。克隆可选择独立目标域，避免替换配置的额度失败污染
已有域。原 C API 的默认创建仍使用 global；配置实例的后续加载使用其 owner。

P0 加载器在该域暂存，配置内容至多 1 MiB，额外一字节探测越界；拒绝超出
4094 字节的物理行（首行 BOM 另行识别），再解析内存 reader；目录、读取失败、过长与诊断明确返回 false，
空文件允许保留默认配置。这样不会把原 LineIterator 吞掉的读取错误当成 EOF。
P0 finalize 也有显式结果；失败后的部分配置必须销毁，不能发布给 App。
旧 streaming loader 的行为保留。这里不宣称任意文件系统 I/O 有期限，亦不
接管全局初始化/environ、框架/GPU 或其他全局分配；仍属实验接口。

## 0040-config-environment-owner.patch

0039 的配置 arena 已归宿主，但请求中的环境映射副本仍经 global allocator
分配。这里新增内部 allocator 参数入口，工作目录/主题展开、普通配置路径、
主题位置和默认 XDG 路径均把副本放进调用方域；原全局入口保留其他调用者。
工作目录和两档主题的 `~/` 展开失败现在向上传播，避免日志之后把未展开路径
当作成功。P0 的显式 finalize 能接住该错误；失败的临时配置仍必须整体销毁。
普通 Path.expand 原有的诊断/清空路径语义不变，P0 严格加载器拒绝诊断。

`python3 scripts/probe-session-config-env.py --mutations` 在干净钉版本副本应用
整条实验补丁链，仅向隔离 Config 模块注入测试。六条定向单元覆盖指定 allocator
耗尽、无需分配的绝对路径、HOME 展开/短缓冲区，以及工作目录、浅/深色主题
调用点；synthetic HOME 仅传给测试子进程。五项变异要求目标红/独立对照绿，
编译与运行分离，编译失败或信号不能冒充断言失败；INT/TERM 恢复源码并以起跑
SHA256 校验，编译子进程归独立进程组。

这不接管全局环境的初始化/驻留，也未证明任意环境大小、系统 home 查询期限、
全部平台路径错误或跨进程总预算；没有新增生产 daemon 或公共 Ghosterm ABI。

## 0041-global-allocator-lifecycle.patch

0039/0040 已约束请求内配置分配，但 global.init 仍选择自己的 GPA/libc，
C 接口也没有结束该所有权的入口。这里让内部初始化接受可选 allocator，
并增加实验 `ghostty_p0_init` / `ghostty_p0_deinit`。宿主 descriptor 保持地址
稳定，直到全部 App/config/surface 已销毁、其线程已 join 且全局释放完成。
释放不是仍有 borrower 时可调用的 BUSY 探针，调用方负责上述生命周期顺序；
释放后不得再调用依赖全局状态的 API。

C 初始化入口共享原子单次状态：NULL 宿主参数不消费尝试；真实初始化尝试
失败或成功释放后均不可重试，避免未知第三方库在部分初始化后的重复进入。
已有 legacy 初始化不能被 owned deinit 接管，两个入口也不能相互覆盖全局。
非 C 的 main/tool 路径仍按原方式选 allocator。本补丁不提供跨版本兼容入口。

`python3 scripts/probe-session-global-owner.py --mutations` 在干净 pin 副本
应用完整实验链，验证冷态释放拒绝、成功初始化/全局释放、首/次分配拒绝、
真实容量耗尽与 legacy 隔离六类。成功路径还创建/销毁使用全局 allocator 的
配置对象，再确认全局域归零及额度可释放。九项变异分别要求目标断言红和
独立对照绿，编译/信号失败不得计为红；源码按起跑 SHA256 恢复，编译及链接
子进程由独立进程组拥有。该脚本与 process/worker-surface 原型共享目录和
构建缓存，必须串行运行。

这只覆盖全局状态与 I/O 实现通过 Zig allocator 申请的对象；全局 I/O 工作
线程的原生栈/guard 尚未计费，借用的 argv/environ、信号/locale 等进程副作用
以及 glslang/oniguruma/i18n/系统框架内部内存也不在
宿主域覆盖声明内。P0 GUI 尚未把该入口接入同一个 pool，总预算与生产 daemon
仍未完成。本补丁不证明第三方库可在同进程重新初始化，恰恰禁止该行为。

## 0042-global-allocator-observer.patch

0041 的全局 allocator 入口现在接入 P0 GUI 的同一个 pool：初始化前持有独立
8 MiB 租约，App/config/surface 与其线程清理后再全局 deinit，最后才释放该域。
旧 surface 用例仍走其既有一次初始化，不提前消费进程原型的 owned 初始化。
原生 I/O 工作线程的栈/guard、std.Thread 的 C allocator 入口参数帧及框架
内部仍不在这个 8 MiB allocator 域声明中。

新增的 `ghostty_p0_global_allocator` 读取实际 global.alloc 的身份，只在 owned
全局存活时返回非零。它是进程内的观测值，不是可跨 IPC 转移的指针，不延长
任何寿命。用它防止“预留了容量、引擎却仍走别处”的假计费；legacy、冷态和
释放后返回零。原始全局 API 六类测试同步检查这些状态。

私有 `session-global-owner.h` 管理初始化事务、失败回收和 BUSY 保留；五类
租约测试验证无效 owner、成功、池满后释放额度重试、真实分配失败，以及
全局结束后仍有 allocator 借用块时保留租约、归还块后再次 close。
这不允许带着存活的 Ghostty borrower 调用全局 deinit。

`probe-session-process.py --global-owner-mutations` 验证十五项定向变异，
要求目标断言红、独立对照绿；编译错误、panic、信号均不得充当有效断言红。
容量测试实际同时预留两块 surface 与一份临时配置，进程级域一直持有，
再多一页必须拒绝，释放后恢复原承诺；它不复制 total 算式来充当 oracle。
新脚本在构建前检查既有隔离源标记；编译器受进程组管理，INT/TERM 恢复四份
私有源并对拍起跑 SHA256。仍只属于实验资源接线，不构成 P0 或 daemon 完成。

## 0043-owned-global-io.patch

全局 I/O 的 worker 之前由 std.Thread.spawn 创建，入口参数不走宿主 allocator，
原生栈也没有共享准入。这里使用 `patches/zig/0.16.0/` 的隔离标准库副本，
为 owned 全局实例挂接固定两槽 registry：两种调度限额为 2、请求栈 16 MiB，
入口帧与 256 KiB 信号缓冲仍复用 0033/0034 的宿主 pthread 入口。

registry 的句柄必须在 Threaded.deinit **之后** join。后者的 wait_group
只覆盖 worker 函数，外层帧清理还没结束；仅改成 owned 帧却继续 detach 会
使全局 allocator 的释放与 frame.free 竞争。join 保证退出回调、信号恢复与
帧释放都结束，才允许 GUI 释放全局租约和原生线程容量。

实验 C 观察/探针只在 owned 全局存活时触发实际 future/group 的四条调度路径
及取消；allocator 身份、实际栈/guard、信号缓冲所在范围由独立 C 代码核验。
结束后只允许读取保留的 POD 统计，不暴露已死线程句柄。probe 是 P0 验证入口，
不是公开 session API。非 Darwin owned 初始化明确失败，无 registry 的
legacy/main/tool 路径保留标准线程分支。

GUI 为每槽 stack+两页的容量单独在同池预留，帧和信号缓冲计入原有全局
8 MiB 域；实际线程在全局初始化之后、创建 App 前验证，并留到最后全局
释放时再退出。该计费不涵盖 CoreText/Metal/GPU、其余系统/TLS 内部开销，
也不意味着完整 P0、生产 daemon 或其他平台已经交付。

预热探针在发布前有界等待队列空闲；Future/Group 完成通知早于 busy_count
递减，连续复用曾触发一次合法但不适合发布预热状态的瞬时 unavailable。
这是探针/预热屏障，不把普通 I/O 的 ConcurrencyUnavailable 自动重试或改成
成功。生成器还拒绝子目录符号链接逃出隔离 engine，拒绝覆盖已修改的生成
副本；这些检查不修改工具链来源文件。

## 0044：UI 创建空副本，IO owner 一次性导入

原生重连需要让快照/增量避开 UI 线程。新增 empty readonly surface：构造不接收
checkpoint 字节，平台可以先保留逻辑 pane，在主线程创建新的空视图，再由 IO
owner 在该 Surface 一次性导入 canonical/resources。不是把正在显示的旧 Terminal
原地覆盖，旧视图保持只读，协议应用屏障完成后再切换显示；避免复用旧 selection/
search/renderer 引用。导入失败的隐藏占位不允许复用，不伪称恢复成功。

新入口只允许 empty、未导入、非零 generation；解码在 renderer 锁外，最终地址
安装 parser/continuation、资源 hook、实际序号后才确认应用。CPU/page 分配继续
走预先准入的同一 scope；资源默认 deny resolver，不回落到宿主文件访问。普通
host output 不能混入这个接缝。原构造时导入的探针不改变语义。

导入本身不开放输入，LIVE 后显式启用，失效时同步关闭。旧代/禁用期间排队输入
丢弃，不污染新代；资源、序号、应用 barrier 与权限失败保持可观测。当前仍是
实验引擎接缝，需接公开 ABI/原生 UI 并完成其余 P0 闸门才算产品交付。

## 0045：空副本独立配置

WHY：UI 创建空 Surface 时不能临时修改共享 App 配置，否则一个 pane 的
checkpoint 配色会改到其他 pane。新增只允许 empty replica 使用的构造期借用
config 指针，原有 newConfig 深复制生命周期不变。平台在 UI 线程用自有预算
clone 配置、设置 checkpoint 元数据中的配色，创建后立即释放临时配置；快照
字节依旧只在 IO owner 导入，不把整个 checkpoint 交给 UI。

此字段改变 strict engine contract，客户端/宿主必须一起重建；不加版本协商。

## 0046：空副本真实输入屏障

WHY：真实 `ghostty_surface_text` 在导入前走 Surface.queueIo → queueEncodedInput，
当动态副本尚无 generation 时会重进原始 write 分支，形成递归栈溢出。旧的
p0_surface_test_input 直接入 Termio，未覆盖这个入口。未开放的动态副本现在
在编码 stamp 上即 blocked；后台写路径仍核对 valid/generation/input_enabled。
普通 Surface 不受影响。原生探针新增真实键入和 legacy 对照，不能把 crash
算作变异通过；另用“把普通输入一并封锁”变异证明对照断言有指向性。

## 0047 — ordered replica style

App.updateConfig must skip empty replicas: the untagged update poisons the replica
I/O sequence. Source STYLE owns background/foreground/cursor/256 palette defaults;
the native owner clones font configuration only at the existing tagged scheme
barrier. The two C helpers resolve conditional theme colors without mutating the
App, and install explicit colors in a private, budgeted replica config.
No vendor edit; strict contract changes with this patch.

## 0048 — input during LIVE configuration barriers

After initial LIVE admission, dynamic replicas can encode input while their local
configuration application is pending. The bounded native Transport retains those
same-generation stamps, submits STYLE first and waits for actual engine apply
before draining input. Detach still revokes/discards the queue; no disconnected
replay is added. Initial/pre-LIVE and old surfaces remain input-disabled. Fixed
experimental replicas keep their old blocked-scheme policy. Without this change,
opening another window or updating font/theme can silently drop a typed key.
