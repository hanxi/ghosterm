# 路线图

更新：2026-10-29，按当前源码核对。优先顺序为 **macOS 现有缺口收尾 → Android 原生竖切 → 其余平台**。
已交付、部分实现和待验证分开记账；历史切片的测试数量不能当作当前测试总量。
能力与证据边界见[能力矩阵](capability-matrix.md)，验收规则以 [AGENTS.md](../../AGENTS.md) 为准。

## 当前进度总览

| 里程碑 | 当前状态 | 后续重点 |
|---|---|---|
| M1 macOS 终端竖切 | 已交付 | 普通本地 PTY/SSH、full libghostty + Metal |
| M2 macOS 认证与分发 | 主要功能已交付，安全存储和发行仍有缺口 | 私钥材料接线、Developer ID/公证、降低发行包系统下限 |
| M3 ssh_config 图形管理 | UI/无损编辑已交付，写盘事务未完全满足铁律 | 备份失败必须阻断、真正原子替换、失败注入 |
| M4/M4.6 文件管理 | macOS 队列/文件操作/系统集成已交付 | 统一工作区替代旧双栏组件；端点活性与 UI 验收仍有遗留 |
| M4.5 终端 UX | 标签、分屏、设置、IME、焦点与停画已交付 | 布局/标签跨重启持久化未做 |
| 统一文件窗格工作区 | 已交付主要实现 | 真拖动、Finder 多选、目录冲突及尺寸证据补齐 |
| 标签与窗格界面重构 | 本机 macOS 26 已交付并验收 | macOS 15 本轮 CI 证据仍需核验 |
| 可选 tmux | macOS/真实 Linux SSH 已交付 | 仅已测版本/目标；普通入口不依赖 tmux |
| M5 Android | 最终 HTML 与详细实现计划完成；原生未开始 | A0–A8、A10；环境包 A9 独立延后 |
| M6+ 其余端 | 未交付 | iOS full 引擎/宿主前置，其他平台分别立项 |

## macOS 已交付范围

### 终端、认证与共享核心

- [x] 普通本地 PTY、SSH shell 的 Transport；输出直达 TerminalSurface，不经过 UI 事件队列。
- [x] `ghosterm.h` 聚合 ABI、公开子头独立编译、符号双向对拍；当前 71 个公共符号，Apple 引用 69 个、2 个登记未用。接口尚未冻结。
- [x] `ssh_config` 解析、Include、42 键 × 6 host 求值对拍、未编辑逐字节往返与编辑稀疏补丁。
- [x] 可取消的异步 SSH 认证、主机密钥确认、方法发现、凭据请求归属；普通 SSH/SFTP/tmux 复用 `SSHAuthenticationFlow`。
- [x] macOS full libghostty + Metal；字节→网格、渲染层与自进程像素验证；中文等 IME 组合/提交链路。
- [x] 有界输入写入、尾队列、异步关闭；PTY 控制终端、描述符隔离与退出状态未知不折叠。
- [x] 本地 cwd 内核查询；OSC 133/7501 上报、程序状态与上一命令失败提示。

### 标签、窗格与设置

- [x] 单个原生窗口 `TabWindowController`；顶部/侧边标签切换，宿主、surface、PID、焦点保持；旧多窗口原生标签组退役。
- [x] 顶部胶囊标签、唯一加号固定右端；侧边纯标签列表、唯一新建入口紧随末尾；无工作/个人分组、无精简侧栏。
- [x] 标签 UUID 选择、关闭、拖拽重排、右键落点、标签搜索与横向滚轮；主机管理、设置、文件、传输等主菜单入口可达。
- [x] `WorkspaceLayout` 为唯一可写布局，终端与文件窗格共用 `WorkspaceHostView`；四边停靠/中央交换、缩放、分隔拖动、隐藏文件窗格与纯文件标签。
- [x] 当前分屏顶部贯穿蓝线；有间距时细边框，间距为 0 时共享分隔线；标题栏隐藏仍可操作。间距默认 0，保留用户已有值。
- [x] 非当前窗格悬停动作、菜单与快捷键按 PaneID 路由；目标半区分屏预览不改变连接/焦点，Shift 动作可达。
- [x] 焦点下发、非聚焦变暗、后台标签停画；设置的应用外观与已有终端主题即时更新。
- [x] 关闭确认根据作业/传输与 UNKNOWN 判定；普通 shell 自行退出移除终端叶子，有文件叶子时保留标签；失败保留错误，tmux detach 保留断开入口。

当前结构与本机证据见[标签界面重构](plan-tab-chrome-refactor.md)；工作区仍缺的证据见[工作区交接](workspace-handoff.md)。旧 `PaneLayout` 只是终端投影，不能再描述成独立可写布局树。

### 文件管理与传输

- [x] SFTP list/stat/lstat/readlink/realpath/mkdir/rmdir/remove/remove_all/rename/chmod/walk/upload/download/pause/cancel；SSH exec 通道与拒绝时显式错误。
- [x] `FileBackend`、本地/SFTP 后端、目录导航、排序、过滤、隐藏文件、树/列表、搜索、文件操作与权限查看/修改。
- [x] `CoreEventPump` 唯一消费 Core 事件；`TransferManager` 跨标签队列、目录父任务、进度/速度/ETA、暂停/继续/取消/重试、冲突处理。
- [x] 本地→远端、远端→本地、Finder 拖入远端、本地代表条目拖出 Finder、Quick Look；文件条目拖放与窗格拖放采用不同 UTI。
- [x] 独立 SFTP 标签在统一工作区中创建本地+远端文件窗格；`SFTPBrowser`、`DualPaneFilesView`、`DualPaneSession` 已删除。终端内文件窗格也进入同一布局，可重排/缩放。
- [x] 文件工具条折行、关闭入口、原生列表单击选择/双击打开、文件键盘归属；文件窗格关闭不销毁已入队传输。

M4.6 的 S1–S7 与 macOS S6 已完成，不能再写“队列 UI 未接”。iOS S6 未做。
最初设计及其替代关系见[文件管理器计划](plan-files-manager.md)。**macOS 尚无本地↔本地与远端↔远端的统一复制队列**；Android 原型四方向是新需求，不回填为 macOS 已交付。

### 可选 tmux 与分发

- [x] 普通 PTY/SSH 为默认；旧 sessiond、私有恢复协议、快照、安装及随包 runtime 整体退役，无兼容层。
- [x] `gt_tmux_*` 探测/列表/管理、普通 PTY attach；用户明确选择、关闭只断客户端、断线后刷新重选，不重放输入。
- [x] tmux 3.2a/3.6a 的本机与真实 SSH 证据、网络中断/未知结果/竞态/任务保留与干净打包；具体支持范围见[实施清单](optional-tmux-implementation.md)。
- [x] `package-mac.sh` 自包含 `.app/.dmg/.zip`、递归依赖、图标/主题、签名档位、校验和与 BUILD-INFO。
- [x] main dev 与 PR/tag release 双轨 CI；Homebrew cask 对拍、一键安装与回滚校验。
- [x] 关于窗口手动更新检测，commit 为主锚；未知不能报最新；只提供下载，不自动安装。

## macOS 下一步与共享缺口

这些项目仍未完成；本轮只修正文档，没有修改相应代码。

| 优先级 | 工作 | 当前源码事实 | 完成条件 |
|---|---|---|---|
| P0 | 配置保存事务 | `ConfigStore.save` 对备份异常 `try?`，再 remove 旧文件、move 新文件；mtime/size 检测不是完整事务 | 备份失败停止；同目录原子替换；故障/并发变化注入证明原文件始终保留；与 `AGENTS.md` 铁律 #4 对齐 |
| P0 | 私钥安全存储接线 | `SSHCredentialSelector.Storage.readKey` 每次读取受限磁盘 PEM；Keychain 实际保存口令/密码，旧 PEM helper 不等于生产接线 | 明确导入与存储所有权，平台安全存储承接私钥；取消/拒绝清理与既有认证回归；不得更改铁律 #5 迁就现状 |
| P1 | 非覆盖文件复制与覆盖语义 | 公有 SFTP job API 无独占创建/非覆盖提交参数；上传 WRITE\|CREAT，无 TRUNC 契约 | 防并发重名覆盖与旧尾残留的共享契约、C/Apple/后续 JNI 绑定和真 sshd 变异；Android A5 依赖此项 |
| P1 | 事件终态可靠性 | `core.c` 的 256 项队列溢出会覆盖最旧事件；没有 job 结果查询 ABI | 压力下终态可保留或可查询；不能以百分比推断成功；Android A6 与 macOS 队列共同回归 |
| P1 | 统一工作区验收缺口 | 真 NSDraggingSession 起拖/图像、多选 Finder 导出、端点连接级活性、目录冲突 UI、跨标签最小尺寸仍不完整 | 按交接文档逐项补生产路径证据和有指向性的变异 |
| P1 | macOS 15 CI | 本机 macOS 26 有证据，不能代替另一系统本轮 CI | 核验当前提交的 workflow 运行与原生结果，不只看配置已接入 |
| P2 | 发行签名/系统下限 | dev 为 ad-hoc；包依赖抬高到 macOS 26；源码声明 macOS 13 不等于发行包可运行 | Developer ID/公证的实际包证据；自建依赖降低 minos 后在目标系统验证 |
| P2 | 不可变版本 cask | 滚动 dev URL 使用 `:no_check` | 发布不可变资产后改真实 SHA256，并同步 cask 对拍规则 |
| 延后 | 标签/布局跨重启保存 | 仅进程/标签存活期内保持，普通会话无跨退出保活 | 独立设计；不自动恢复连接，不重新引入旧 daemon |
| 延后 | 远端 cwd 与 shell integration R2 | 本地 cwd 已有；embedded host IO 尚未交付远端 cwd | 明确上游/补丁接缝及注入方案，有真实远端路径证据后再做同目录跳转 |
| 延后 | 其他共享能力 | 转发 -L/-R/-D、hashed known_hosts/@revoked 未实现 | 分别立项，能力缺失显式报错 |

“dev 包签名+公证”不再作为一个永远未完成的 dev 任务：dev 的 ad-hoc 是当前渠道设计；正式可信分发由 Developer ID/公证项承担。macOS 细节可参考 [dev 构建说明](../how-to/dev-build.md)。

## M5 Android 实施进度

设计遵循主机首页、单一终端键盘入口、长按多选与持续可见的文件复制清单；不添加主机分组。
[最终 HTML](../design/android-prototype.html) 为页面基准，[详细实现计划](android-implementation.md) 为开发与验收入口。

- [x] 单文件交互原型：主机/终端/文件/完整设置；旧三份 HTML 已合并删除。
- [x] 实现文档：线程/模块、配置/认证、输入法、SAF、四方向复制、16 项设置、生命周期、阶段与验收。
- [ ] A0 工程、锁工具链、NDK/core/vt 依赖与 JNI ABI 对拍。
- [ ] A1 导航与主机配置、安全写盘事务。
- [ ] A2 真本地系统 shell、VT 自绘、输入法与尺寸。
- [ ] A3 SSH/身份/主机验证、取消与多会话。
- [ ] A4 私有目录/SAF/远端浏览、打开与多选。
- [ ] A5 共享安全复制前置与本机↔本机、本机↔远端、远端↔远端。
- [ ] A6 任务队列、进度、暂停取消、失败重试与清单身份。
- [ ] A7 设置持久化、已有 surface 即时生效、身份详情与恢复默认。
- [ ] A8 可选远端 tmux 管理与 attach。
- [ ] A10 设备矩阵、APK、CI 与发行验证。
- [ ] A9 环境包独立 PoC（延后）：Termux/proot 执行限制、下载/哈希/原子激活/回滚；不阻塞普通 SSH。

当前 `android/` 与 `terminal/android/` 都只有 README，无原生工程、JNI、APK 或 CI。
普通本地入口拟为系统 `/system/bin/sh`，不是打包一个新 shell；不能用桌面 PTY 证据代替 Android 真机验证。
旧 attachment ABI 已退役，Android 走现有 Transport + 可选 tmux；共享接口未冻结，缺口须同步各端后再使用。

## M6 及其他平台

- [ ] iOS：full libghostty iOS 构建/Metal 宿主先行；当前 SwiftPM App 仅 macOS，不能只加 `#if` 声称已兼容。iSH 环境包按需下载，不随 IPA 打包。
- [ ] HarmonyOS：libghostty-vt + ArkUI Canvas 与 NAPI 绑定。
- [ ] Linux：full libghostty + OpenGL 4.3 与 GTK4 原生宿主。
- [ ] Web：libghostty-vt wasm + 自绘；full 引擎等待上游 WebGL 后端。
- [ ] Windows：渲染引擎/后端待验证，ConPTY 尚无实现。

## 验收记账与历史记录

2026-10-29 文档核对本轮运行：`scripts/gate.sh` 的 **32 项 CTest、71 个公共符号通过**；
`check-abi-apple.sh` 的 **69 个引用、2 个登记未用通过**；`swift build --package-path apple/GhostermApp` 通过。
本轮为文档变更，没有新增 Android 原生验证，也没有把过去 macOS 测试当作本轮重跑。

最近一次完整 macOS 宿主记录见[标签界面重构末尾](plan-tab-chrome-refactor.md)：
341 项测试、1 项开发态资源跳过、0 失败，以及相应变异/像素证据；此前真实系统激活与全屏的测试范围另列。
统一工作区变异目前 21 组，属于手工证据；full CI 目前接入 20 份脚本，准确清单以 AGENTS 与工作流为准。

历史决策与当时测试规模分别保留在：

- [文件管理器 M4.6](plan-files-manager.md)、[面板/标签菜单](plan-panel-tab-menu.md)。
- [关闭确认、⌘T 与 cwd](plan-tabs-close-and-newtab.md)。
- [工作区交接](workspace-handoff.md)。
- [标签与窗格界面](plan-tab-chrome-refactor.md)。
- [可选 tmux 重构](plan-optional-tmux.md)及[实施清单](optional-tmux-implementation.md)。

旧脚本退役、测试数变化、历史 UI 被替代不能拿来删除仍有效的业务断言；每个新增闸门都需目标红、同变异独立对照绿、回滚复绿及 SHA256 一致。
