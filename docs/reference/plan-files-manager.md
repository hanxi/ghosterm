# 计划：SFTP → 统一文件浏览器（M4.6）

> 决策记录 + 进度。**分阶段切片**，每片独立提交、闸门绿才提交（AGENTS.md）。
> 状态：**S1 ✅ / S2 ✅ / S3 ✅ / S4 ✅ / S5 ✅ / S6（macOS）✅ / S6-iOS ⏳ / S7 ✅（文档收口）**。
> 相关：`roadmap.md`（M4.6）、`capability-matrix.md`（SFTP / exec / files 行）。

## 当前实现与后续替代（2026-10-30）

M4.6 的 macOS S1–S7 已交付，iOS S6 未开始。后续[统一工作区](workspace-handoff.md)已将固定内嵌面板和 `DualPaneFilesView` / `DualPaneSession` 替换为 `WorkspaceLayout` / `WorkspaceHostView`；终端、本地文件、远端文件都是同一布局中的窗格。`FilesPanelView`、后端接缝与全局 `TransferManager` 继续使用。

独立 SFTP 标签仍默认本地+远端两栏，但这由工作区组合生成，不再有独立双栏渲染组件。当前侧栏只列活动标签，主机管理在连接选择器与菜单中；界面入口以[标签界面计划](plan-tab-chrome-refactor.md)为准。

队列、暂停/继续/取消/重试、目录父任务、拖放和 Quick Look 的正常操作入口已实现；控制失败与未知结果、目录请求归属/历史/缓存、预览限额与回收仍未收尾，本地多选拖出 Finder、连接级活性、目录冲突窗口端到端等也有验收缺口。路径式 SFTP job 已按 §9.53 补齐覆盖截断和关闭结果检查，旧 job 仍不提供非覆盖提交；§9.54 新增独立的 fd→远端暂存创建接口，尚无最终发布/原子提交；不能将 UI 冲突选择当成并发写入安全保证。当前进度见[路线图](roadmap.md)。

上述传输控制已交付的是正常路由。当前 `TransferManager.pause/resume/cancel` 用 `try?` 吞掉后端控制错误，随后仍改变显示状态；`handle` 把未知 job 结果当作失败。控制失败、取消确认与任务结果未知的状态机尚待修复，不能沿用“所有三态均已正确处理”的口径。历史测试的暂停/取消 fake 后端不抛错，不能作为该异常路径的证据。

目录复制当前排除内部符号链接：远端 walk 的链接不生成下载子任务，本地枚举的链接直接跳过。现有下载测试明确断言两个普通文件入队、链接不入队，但没有用户可见的排除明细或计数；仅包含链接的目录可按空文件集合报完成。下文“不创建符号链接”的范围约定仍有效，不能据此把未复制的条目记为完整成功；结果呈现与验收待办见路线图。

macOS 保持原生列表单击选择、双击打开；Android 最终方案采用点按打开、长按选择与可见复制清单，两端手势不能混写。Android 的本地↔本地、跨远端复制属于新增 A5 契约，不代表这里的 macOS `TransferManager` 已具备四方向统一队列。两端共同依赖的非覆盖提交与事件终态可靠性仍列在路线图，原始 S1–S7 完成不消除这些新发现的缺口。

当前搜索也未完全兑现下文原始设计的错误语义：`SFTPFileBackend.searchLocked` 对任意 exec 打开错误回退 walk，并非仅处理 `GT_ERR_UNSUPPORTED`；`readFind` 忽略 stderr、退出状态和单条 lstat 失败。页面取消只让旧 token 的结果不可见，后端搜索没有随之停止。与此同时，`exists` 将所有查询错误折为不存在，`TransferManager` 的主线程调用会同步等待后端串行队列。详细调用链见 [Apple 文件查询与线程边界](../../apple/README.md#文件查询与线程边界)，三项均在路线图 P1 中待修，历史正常路径测试不能代替失败/慢网络/取消验收。

下文是 **M4.6 当时的设计与切片记录**：旧组件名、截图入口和测试数量仅用于追溯，不代表当前调用路径或当前测试总数。

树视图、本地搜索和递归权限也有待修异常路径：`FilesPanelModel` 将树加载失败缓存为空节点，忽略本地搜索及递归权限的 walk 错误；本地后端 lstat 失败条目被跳过。历史主流程验收不包含这些失败的可见性、重试及部分结果，详见[Apple 文件查询说明](../../apple/README.md#文件查询与线程边界)和[路线图 P1](roadmap.md#macos-下一步与共享缺口)。

§9.53 已修复下载打开顺序、上传截断、源定位和关闭错误；非覆盖/原子提交仍未实现，负深度 walk 仍没有硬护栏。正常 job/目录测试不等于目标提交和任意深度都可靠，修复条件见[路线图](roadmap.md#macos-下一步与共享缺口)。

队列是全局聚合，实际 FIFO worker 属于每个 SFTP 会话，跨连接没有全局限并发；下文“并发 v1 = 1”仅指同一会话。重试要求原 backend 仍存活，不自动重连或断点续传；终态释放强引用后，关闭的文件端点可能已无法重试。详见 [Apple 传输边界](../../apple/README.md#传输提交调度与重试边界)。

“应用到全部”当前保存在全局队列，完成/清空后不重置，作用域超过本次批量操作；目录重试重新扫描整目录，不仅提交旧失败项。两者仍是待修的产品行为，S4 的历史完成标记不表示已有批次隔离或失败项重试保证。实现落点为 `GhostermApp` 的冲突回调和 `TransferManager.resolveConflict/retry`，验收场景见[路线图 P1](roadmap.md#macos-下一步与共享缺口)。

递归权限与目录复制的链接策略不同：前者对 walk 返回的链接也调用 chmod，本地实现没有不跟随链接的修改路径。因此 S3 的正常 chmod 交付不表示目录外链接目标受到保护，须按[权限边界](../../apple/README.md#递归权限与链接边界)补契约与验收。

S6 的 Quick Look 已接本地打开及远端下载完成后打开；远端副本没有应用层回收、大小限额和请求代次保护，离页后下载回调仍可能弹窗。现有路径形状测试不覆盖实际预览窗口及这些生命周期；当前范围见 [Quick Look 边界](../../apple/README.md#quick-look-预览边界)，不能将历史 S6 完成标记理解为预览缓存管理也已完成。

目录浏览还须补最新请求归属、失败历史回滚与树缓存失效：`FilesPanelModel.navigate` 的每个完成回调都更新显示并清除加载状态，`goBack/goForward` 在读取前改历史，`refresh` 只更新列表而保留旧树缓存。普通导航与树懒加载的历史通过不覆盖这些交互；源码与退出条件见 [Apple 导航边界](../../apple/README.md#目录导航与缓存边界)和[路线图 P1](roadmap.md#macos-下一步与共享缺口)。本次仅文档核对，未修复或新增原生验收。

§9.50 新增有界只读预览：LSTAT/只读 OPEN/FSTAT/READ/CLOSE 共用预算，46 项协议与 20 组变异通过；只返回原始字节，macOS Quick Look 未消费，缓存与迟到结果保护仍待实现。共享受控目录浏览及大小/类型标志已按 §9.49 通过协议/变异。macOS `SFTPFileBackend` 仍用旧 list/realpath，未向 `FileEntry` 传递标志；须独立迁移 owner 和显示/预览/任务语义。提交 UAF 和 exec 超时泄漏已修复，不提高整个文件产品的完成度，见 [§9.49](android-implementation.md#949-受控目录浏览与共享会话修复2026-10-30)。

共享 SFTP 受控打开与主机依赖已按 §9.47 验收：17 项协议、十组接口变异、十一组依赖守卫和既有打包变异通过。macOS 文件工厂仍调用旧 `ssh.sftp()`，Android 已由 §9.51/§9.52 接入专用文件 owner/JNI/Kotlin 与只读页面。后续产品打开/列表/预览/关闭与资源回收仍独立验收，见[主机依赖状态](host-ssh2-build.md)及 [Apple SFTP 边界](../../apple/README.md#sftp-打开与取消边界)。

## M4.6 原始目标形态

三层形态，共享同一套「文件浏览器」UI 与后端接缝：

- **内嵌侧栏面板**（终端标签页右侧，默认收起、⌘⇧E 唤出、宽度按标签记忆）：
  单栏轻活。SSH 标签页后端=远程 SFTP；**本地 shell 标签页后端=本地文件系统**
  （只换后端、零分叉）。
- **独立 SFTP 标签页**（全窗口）：**左右双栏**（左本地 / 右远程），拖放即传。
- **全局传输队列**：跨标签聚合、暂停/继续/取消/重试、速度/ETA、冲突
  「询问 + 应用到全部」。

## 关键决策（含取舍）

- **后端接缝 `FileBackend`**：`LocalFileBackend`（Foundation/POSIX）与
  `SFTPFileBackend`（core `gt_sftp_*`）挂在同一面板后面——延伸铁律 #1。
- **SSH 面板懒开独立第二会话**（不复用终端会话）：libssh2「一会话一线程」，
  复用会互抢通道；代价是多一次握手/认证。
- **远端加速走 exec 通道**，不装常驻守护进程：递归 SFTP 遍历包含多次目录/属性请求，远端 `find`/`grep` 可减少宿主发起的请求；实际收益依赖 RTT、目录结构与服务器。
  服务器禁 exec → `GT_ERR_UNSUPPORTED`，**显式回退** SFTP walk（档位在 UI 标注），
  绝不静默降级（铁律 #6）。不做 vscode-server 式常驻代理（另一套安全模型）。
- **单会话串行**：每会话一把 I/O 锁 + 单条 I/O worker 线程驱动 SFTP job；
  请求/响应在锁内完成，传输分块推进、块间释放锁——浏览与传输交替推进。
  单会话传输并发 v1 = 1（正确性优先），不是全应用只运行一个传输。
- **不做**：队列跨启动持久化；远程↔远程传输；v1 不创建符号链接；权限框 v1
  只做 chmod 模式位（uid/gid 仅只读数字）。

## 切片与进度

### 性能证据（exec vs walk）
e2e 里造 300 文件宽目录，对比「远端 exec `find`」与「SFTP 逐层 walk」：两条路
都找全；**环回上 exec 反而慢（~40ms vs ~3ms，起远端进程的开销）**，exec 的
优势需要在**真实 RTT** 链路另行测量，不能把每次 readdir 调用简单计为一次网络往返。故只断言正确性，
耗时不设速度断言（环回会得出误导结论）。

### S1 ✅ core
完整文件操作原语 + exec 通道 + 单 I/O worker。见 `git log`。
闸门：`gate.sh`（57 符号）、真 sshd e2e 75 断言、`mutation-check-sftp`（F1~F8）、
`mutation-check-exec`（E1~E2）。

### S2 ✅ bindings + 接缝 + 面板
`SFTPSession`/`SSHExec` 绑定；`FileBackend`/`FileEntry`/`LocalFileBackend`/
`SFTPFileBackend`/`FilesPanelModel`；内嵌面板 `FilesPanelView`（⌘⇧E）；本地 shell
面板跟随终端 cwd，SSH 面板懒开第二会话；`RemoteSearch`（exec 优先 / walk 回退）。
闸门：`FilesPanelTests` 7 项、`mutation-check-files`（F1~F4）、全量 `swift test` 171 项。

### S3 ✅
文件操作与右键菜单（新建/重命名/权限/信息/拷贝路径/打开/删除**带三态确认**；
符号链接图标区分、删链接不删目标）。
补齐（后补切片）：chmod「应用到子项（递归）」、多选批量删除/下载入口。

### S4 ✅ 全局传输队列
- **前置已完成**：`CoreEventPump` 是唯一取事件者，扇出给 `SFTPBrowser` 与
  `TransferManager`（`gt_core_poll_events` 一次性消费，两个轮询者会互相抢）。
- `TransferManager`：递归目录传输在 App 层用 `walk`（远端）/ Foundation
  枚举（本地）展开成文件级 job + mkdir；暂停/继续/取消/重试；速度/ETA 由
  PROGRESS 事件算；冲突「覆盖/跳过/重命名 + 应用到全部」；传输窗口。

### S5 ✅ 独立标签页双栏
`.sftp(target)` 改 `DualPaneFilesView`（左本地右远程），栏间拖放即传；保留侧栏
右键与 ⌘T 继承入口。旧 `SFTPBrowser`/`SFTPBrowserView` 已删除。
**树/列表可切**（补齐）：面板顶部有列表/树切换；树模式懒展开、只显示目录
（`FilesPanelModel.loadTreeChildren` / `treeChildrenVisible`，离线断言 + F7 有牙）。

### S6 ✅（macOS）/ ⏳（iOS） 系统集成
macOS ✅：拖放路由 `DropRouter`（栏间拖：远端→本地=下载、本地→远程=上传；
Finder 拖入远程栏=上传）；本地条目拖出到 Finder；Quick Look（空格 + 右键）。
iOS ⏳：底部抽屉 + 本地/远程分段——**未做**。根因（实测）：钉住的 libghostty
**没有全量 iOS 构建路径**（`GhosttyLib.initMacOSUniversal` 只有 macOS；
xcframework 只有 `macos-arm64` slice）；`swift build --triple ...-ios...-simulator`
在 `GhostermTerminal` 就因 AppKit 失败。前置为独立上游里程碑。当前排期见 roadmap 的 M6 及其他平台。

### 面板键盘（补齐）✅
⌘⇧E 唤出面板（菜单）；面板内 ⌘F（搜索）/⇧⌘N（新建文件夹）/⌘R（重命名）/
⌘I（信息）/⌫（删除）/空格（Quick Look）——**只挂在文件列表上**（`PanelKeyHandler`，
macOS 14+ 的 `onKeyPress`），不挂窗口级，免得把 ⌫/空格 从同屏的终端里偷走；
macOS 13 上退化为右键菜单。映射抽成纯函数 `PanelKeymap`（离线断言 + F9 有牙）。

### S7 ✅ 收尾
⌘T 沿用当前类型、侧栏右键「用 SFTP 浏览文件…」入口与新形态对齐（均指向
双栏）。文档收口：roadmap（M4.6）/ capability-matrix / 本文件。
**SSH↔SFTP「同目录落地」**仍按既定取舍不做（依赖远端 cwd，OSC 7 未达宿主）。

## 已排除项（**不做**）

- vscode-server 式常驻远端守护进程（会话持久化 / 远端索引）：独立子项目，
  exec 通道覆盖本轮加速诉求。
- 「SSH↔SFTP 同目录落地」：依赖远端 cwd（OSC 7 到不了宿主，见 M4.5 遗留 / R2）。
