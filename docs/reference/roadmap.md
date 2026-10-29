# 路线图

更新：2026-10-30。Android 已完成 A0，A1 已交付真实主机/配置、字段来源/编辑归属，以及三字段显式覆盖与恢复继承（实现计划 §9.7）。A1 别名重命名及配置错误恢复已交付（实现计划 §9.9–§9.10），SAF UI 与其余表单校验已由 §9.11 收尾，A1 在 API 36 arm64 模拟器档完成；A2 本地终端首片已交付真 PTY/Canvas/InputConnection 与会话生命周期，完整设备套件 63 项通过，文本操作/真实中文 IME/性能仍待收尾；macOS 表单、写盘、安全存储与传输控制缺口独立记录，其余平台后续实施。
已交付、部分实现和待验证分开记账；历史切片的测试数量不能当作当前测试总量。
能力与证据边界见[能力矩阵](capability-matrix.md)，验收规则以 [AGENTS.md](../../AGENTS.md) 为准。

## 当前进度总览

| 里程碑 | 当前状态 | 后续重点 |
|---|---|---|
| M1 macOS 终端竖切 | 已交付 | 普通本地 PTY/SSH、full libghostty + Metal |
| M2 macOS 认证与分发 | 主要功能已交付，主机信任、安全存储和发行仍有缺口 | known_hosts 标记处理、私钥材料接线、更新清单校验、Developer ID/公证、降低发行包系统下限 |
| M3 ssh_config 图形管理 | UI/Core 稀疏编辑及重复指令修复已交付，macOS 表单保真与写盘事务仍有缺口 | 表单读写使用共享语法和真实脏字段；备份失败阻断、原子替换与失败注入 |
| M4/M4.6 文件管理 | macOS 队列/文件操作/系统集成主流程已交付 | 统一工作区替代旧双栏组件；控制失败/未知终态、端点活性与 UI 验收仍有遗留 |
| M4.5 终端 UX | 标签、分屏、IME、焦点与停画已交付；设置正常路径已接入 | 终端配置重复键、读取/保存失败及过时提示待修；布局/标签跨重启持久化未做 |
| 统一文件窗格工作区 | 已交付主要实现 | 真拖动、Finder 多选、目录冲突及尺寸证据补齐 |
| 标签与窗格界面重构 | 本机 macOS 26 已交付并验收 | macOS 15 本轮 CI 证据仍需核验 |
| 可选 tmux | macOS/真实 Linux SSH 已交付 | 仅已测版本/目标；普通入口不依赖 tmux |
| M5 Android | A0/A1 完成；A2 本地终端首片已验（API 36 arm64 模拟器） | A2 文本操作、Ctrl、真实中文 IME 与性能收尾；A3–A8、A10；环境包 A9 独立延后 |
| M6+ 其余端 | 未交付 | iOS full 引擎/宿主前置，其他平台分别立项 |

## macOS 已交付范围

### 终端、认证与共享核心

- [x] 普通本地 PTY、SSH shell 的 Transport；输出直达 TerminalSurface，不经过 UI 事件队列。
- [x] `ghosterm.h` 聚合 ABI、公开子头独立编译、符号双向对拍；当前 73 个公共符号，Apple 引用 71 个、2 个登记未用；包含字段来源与别名重命名接口，各切片独立记账。接口尚未冻结；来源查询及编辑归属的新契约见文末独立记录。
- [x] `ssh_config` 解析、Include、42 键 × 6 host 求值对拍与未编辑逐字节往返；基本稀疏编辑和非连续重复指令修复已交付。14 个写回用例不扩大为全部配置形态已验证，已增加三个连接字段的来源与歧义编辑保护，完整块视图仍待补齐。
- [x] 可取消的异步 SSH 认证、主机密钥确认、方法发现、凭据请求归属；普通 SSH/SFTP/tmux 复用 `SSHAuthenticationFlow`。
- [x] macOS full libghostty + Metal；字节→网格、渲染层与自进程像素验证；中文等 IME 组合/提交链路。
- [x] 有界输入写入、尾队列、异步关闭；PTY 控制终端、描述符隔离与退出状态未知不折叠。
- [x] 本地 cwd 内核查询；OSC 133/7501 上报、程序状态与上一命令失败提示。

### 标签、窗格与设置

- [x] 单个原生窗口 `TabWindowController`；顶部/侧边标签切换，宿主、surface、PID、焦点保持；旧多窗口原生标签组退役。
- [x] 顶部胶囊标签、唯一加号固定右端；侧边纯标签列表、唯一新建入口紧随末尾；无工作/个人分组、无精简侧栏。
- [x] 标签 UUID 选择、关闭、拖拽重排、右键落点、标签搜索与横向滚轮；主机管理、设置、文件、传输等主菜单入口可达。
- [x] Ctrl+Tab / Ctrl+Shift+Tab 按当前显示顺序循环切换标签，重排后跟随新顺序；菜单与设置快捷键表已接入。
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
暂停/继续/取消的入口与正常路由已实现，但 `TransferManager` 用 `try?` 忽略控制错误后仍切换显示状态，且把未知 job 结果映射为失败；控制成功、控制拒绝和执行结果未知尚未完整区分。该缺口与目录冲突 UI 验收分开跟踪，不以历史主流程通过代替异常路径完成。
最初设计及其替代关系见[文件管理器计划](plan-files-manager.md)。**macOS 尚无本地↔本地与远端↔远端的统一复制队列**；Android 原型四方向是新需求，不回填为 macOS 已交付。

### 可选 tmux 与分发

- [x] 普通 PTY/SSH 为默认；旧 sessiond、私有恢复协议、快照、安装及随包 runtime 整体退役，无兼容层。
- [x] `gt_tmux_*` 探测/列表/管理、普通 PTY attach；用户明确选择、关闭只断客户端、断线后刷新重选，不重放输入。
- [x] tmux 3.2a/3.6a 的本机与真实 SSH 证据、网络中断/未知结果/竞态/任务保留与干净打包；具体支持范围见[实施清单](optional-tmux-implementation.md)。
- [x] `package-mac.sh` 自包含 `.app/.dmg/.zip`、递归依赖、图标/主题、签名档位、校验和与 BUILD-INFO。
- [x] main dev 与 PR/tag release 双轨 CI；Homebrew cask 对拍、一键安装与回滚校验。
- [x] 关于窗口手动更新检测，commit 为主锚；未知不能报最新；只提供下载，不自动安装。

更新检测已交付的是当前状态判据和入口：`RemoteBuild.decode` 尚未严格校验远端 commit、摘要字符集及下载来源，`compare` 不验证提交祖先关系。不能把上面的完成项理解为所有畸形清单已经覆盖；新增缺口单列如下。

## macOS 下一步与共享缺口

以下项目仍未完成；共享非连续重复指令误删已修复，从待办移至上方已交付范围，未因此改变其他缺口状态。

| 优先级 | 工作 | 当前源码事实 | 完成条件 |
|---|---|---|---|
| P0 | 配置保存事务 | `ConfigStore.save` 对备份异常 `try?`，再 remove 旧文件、move 新文件；mtime/size 检测不是完整事务 | 备份失败停止；同目录原子替换；故障/并发变化注入证明原文件始终保留；与 `AGENTS.md` 铁律 #4 对齐 |
| P0 | macOS 主机表单保真 | `HostListModel.directives` 用 Swift 按空格扫描原文；`HostEditorSheet.save` 提交全部显示字段；`AppModel.applyEdits/addHost` 先改共享内存模型再写盘，失败路径未重建模型 | 用共享块/指令契约读取，正确处理 tab、`=`、引号与重复指令；仅改目标字段，未改保存字节不变；候选配置成功落盘后才发布，失败不污染后续编辑；真实表单回归与变异 |
| P0 | known_hosts 标记语义 | `gt__kh_visit` 丢弃所有 `@…` 标记，`kh_check` 对余下的同主机同密钥判 MATCH；同步连接与异步认证均共用此路径 | 保留并区分撤销/CA/未知标记，不能把它们当普通信任记录；撤销与普通信任记录并存时也不得绕过拒绝；未实现的证书能力显式报错。真 sshd 验同步/异步及 macOS 认证路径，目标红、普通已知主机对照绿、复原绿；Android A3 接入前同步收口 |
| P0 | 私钥安全存储接线 | `SSHCredentialSelector.Storage.readKey` 每次读取受限磁盘 PEM；Keychain 实际保存口令/密码，旧 PEM helper 不等于生产接线 | 明确导入与存储所有权，平台安全存储承接私钥；取消/拒绝清理与既有认证回归；不得更改铁律 #5 迁就现状 |
| P1 | macOS 终端配置读写 | `SettingsStore.rawConfig` 把读取失败折为空串；初始 stat 为空时不检测后来创建的文件；`merging` 只改首个同名键而 `value` 取最后一个；`applyTerminalSettings` 先写盘再验证引擎，全文替换的恢复错误被 `try?` 忽略 | 区分不存在/读取失败，外部创建也触发冲突；按引擎键语义处理重复项，保存后实际字号/主题符合用户意图；验证失败保留旧文件及 surface，提交/恢复失败明确报告；故障注入和变异见下文 |
| P1 | macOS 别名重命名 | 共享 `gt_config_rename_host` 与 Swift `SSHConfig.rename` 已实现并定向验证，`HostEditorSheet` 仍仅新建时显示别名输入框 | 配置事务/表单保真修复后接入；仅替换目标 token，多别名/Include/冲突明确处理；重新求值与活动连接快照分离，验证连接 UUID/PID 不变 |
| P1 | 配置来源与编辑归属 | Core/Swift/JNI 已增加三个连接字段的来源，Android 页面已消费并验只读保护；macOS `HostEditorSheet` 尚未消费 | 明确显示当前块/继承/默认及多别名影响；包含文件/重名块拒绝错误写入；两端定向验证与变异，不能只验符号存在；三字段查询不能替代完整块指令读取 |
| P1 | 非覆盖文件复制与覆盖语义 | 公有 SFTP job API 无独占创建/非覆盖提交参数；上传 WRITE\|CREAT，无 TRUNC 契约 | 防并发重名覆盖与旧尾残留的共享契约、C/Apple/后续 JNI 绑定和真 sshd 变异；Android A5 依赖此项 |
| P1 | 事件终态可靠性 | `core.c` 的 256 项队列溢出会覆盖最旧事件；没有 job 结果查询 ABI | 压力下终态可保留或可查询；不能以百分比推断成功；Android A6 与 macOS 队列共同回归 |
| P1 | macOS 传输控制与未知结果 | `TransferManager.pause/resume/cancel` 忽略后端错误仍更新状态；`handle` 将 `.other` 结果折为 `.failed` | 区分用户请求、控制返回与任务终态；拒绝/未知可见，取消未确认不释放任务所有权；注入控制异常和未知事件，验证状态、回调与后端生命周期，另保留正常传输对照 |
| P1 | macOS 可连接主机过滤 | `HostListModel.reload` 排除 `*`/`?`，未排除 `!secret` 这类否定模式；配置规则不等于连接目标 | 否定规则留在配置中，不生成可点击主机；混合别名块、通配块与普通别名分别验证，Core 求值和无损往返保持通过 |
| P1 | 统一工作区验收缺口 | 真 NSDraggingSession 起拖/图像、多选 Finder 导出、端点连接级活性、目录冲突 UI、跨标签最小尺寸仍不完整 | 按交接文档逐项补生产路径证据和有指向性的变异 |
| P1 | macOS 15 CI | 本机 macOS 26 有证据，不能代替另一系统本轮 CI | 核验当前提交的 workflow 运行与原生结果，不只看配置已接入 |
| P1 | CI 跳过审计 | 图标脚本缺依赖直接失败，workflow 显式拦截打包/认证变异的 SKIP；Apple 步骤检查 Swift，但 `apple-app-gate.sh` 可在 sshd 失败后跳过 SSH e2e 并成功退出 | 明确每个必跑项目和允许的资源跳过；缺 SSH 证据时 full 档必须失败，注入 sshd 启动失败证明目标红、独立本地对照绿；不能把所有跳过一概算通过 |
| P1 | 更新清单输入校验 | `RemoteBuild.decode` 的远端 commit 只要求非空，摘要只验长度 64，下载来源未约束；`compare` 只比较前缀 | 验 40 位十六进制 commit、64 位十六进制摘要、合法资产与下载来源；畸形清单返回“查不出来”，合法同/异 commit 对照保留；另明确“不同提交”与“更新提交”的产品文案，不声称已查祖先关系 |
| P2 | 发行签名/系统下限 | dev 为 ad-hoc；包依赖抬高到 macOS 26；源码声明 macOS 13 不等于发行包可运行 | Developer ID/公证的实际包证据；自建依赖降低 minos 后在目标系统验证 |
| P2 | 不可变版本 cask | 滚动 dev URL 使用 `:no_check` | 发布不可变资产后改真实 SHA256，并同步 cask 对拍规则 |
| P2 | 设置页提示与实际应用一致 | 外观字号/配置文件页仍提示仅新标签生效；标题栏提示只在当前格显示按钮；通用页仍提恢复会话、关闭必结束及后台助手 | 对齐已有 surface 重载、悬停/键盘动作、普通启动无恢复与显式 tmux 关闭只断客户端；不把 SSH 路径展示称为 SSH 全文编辑器 |
| 延后 | 标签/布局跨重启保存 | 仅进程/标签存活期内保持，普通会话无跨退出保活 | 独立设计；不自动恢复连接，不重新引入旧 daemon |
| 延后 | 远端 cwd 与 shell integration R2 | 本地 cwd 已有；embedded host IO 尚未交付远端 cwd | 明确上游/补丁接缝及注入方案，有真实远端路径证据后再做同目录跳转 |
| 延后 | 其他共享能力 | 转发 -L/-R/-D、hashed known_hosts 未实现 | 分别立项，能力缺失显式报错；marker 误信任已移至 P0，不作为可延后功能 |

“dev 包签名+公证”不再作为一个永远未完成的 dev 任务：dev 的 ad-hoc 是当前渠道设计；正式可信分发由 Developer ID/公证项承担。macOS 细节可参考 [dev 构建说明](../how-to/dev-build.md)。

### macOS 修复顺序与验收触发场景

以下是待实施顺序，不是本轮修复结果。涉及共享 C 的契约须同时同步 Swift/JNI，Android 的 Compose 状态和存储实现不直接搬入 macOS。

认证侧先修共享 known_hosts 的 marker 处理，再将 Android A3 接到该路径。`@revoked` 不能与普通信任条目等同，`@cert-authority` 不能未经证书验证就充当主机密钥；未知标记也不能静默丢弃。覆盖标记条目在普通信任条目前后两种顺序，验证拒绝不会被早返回绕过。macOS 普通 SSH、SFTP、tmux 共用认证流程，需一起回归。

1. **先收口配置事务和模型发布（P0）**：修改 `ConfigStore.save`、`AppModel.applyEdits/addHost`。候选配置成功提交后才发布到列表；备份、暂存、替换任一步失败均可观察。测试先制造备份不可写、替换失败、加载后同长度内容变化以及 stat 读取失败，再检查原文件/模型/草稿状态；不得把读取错误当成文件不存在。替换成功但同步/回读失败要报告已提交状态，不盲目回滚。
2. **再接共享解析、来源与脏字段（P0/P1）**：替换 `HostListModel.directives` 的简化扫描，调整 `HostEditorSheet.save`。用含 tab、等号、引号、多别名、非连续重复指令和 Include 的配置驱动真实表单；未改保存逐字节相同，改一个字段保留其他行；不把未修改的高级指令合并成单值。否定模式不生成可连接行，来源受限时保存入口与模型都拒绝。
3. **在上述基础上接改名（P1）**：以共享重命名 API 生成候选，再走同一保存事务。改名可能改变继承匹配和默认地址，要重新展示求值结果；已有连接继续使用原 UUID 和连接参数快照。用冲突改名、只改大小写、共享 Host 头、保存失败及已连接主机驱动页面，不能仅测 Swift 包装函数。
4. **私钥安全存储单独完成（P0）**：修复生产 `SSHCredentialSelector.Storage.readKey` 的材料来源，验证导入→解锁→认证及取消/拒绝清理。现有 Keychain 口令保存不替代私钥材料接线，日志与快照不得包含材料。
5. **传输状态机与共享终态保障一起收口（P1）**：后端暂停/继续/取消抛错时，页面不可提前报成功；取消确认前不能释放任务后端或调用完成回调。注入未知 `jobResult` 和事件队列压力，分别验目录父子任务状态、终态回调与所有权；保留同变异正常传输对照。
6. **终端配置独立修复（P1）**：`SettingsStore` 与 SSH 的 `ConfigStore` 是不同存储实现，不把后者的先删后移结论套用到前者。覆盖配置路径为目录/不可读、加载时不存在后被外部创建、两处 `font-size` 或 `theme`、无效配置及回滚失败；普通单键保存作为独立对照。稀疏编辑须按标量/可重复键的真实语义处理，不能一律删除所有同名项；先校验候选，再提交与发布。验磁盘、错误状态和已有 surface 的字号/配色，不能仅以 `value` 的字符串回读代替引擎结果。
7. **最后核验系统与发行档位**：上述对应定向/变异、原生编译通过后，再跑受影响的完整宿主及 macOS 15 CI，并逐项核对必跑项目是否被跳过。发布侧清单校验不能代替 App 解码校验；补齐畸形远端 commit/摘要/下载来源用例，继续保留手动检查与无自动安装入口。签名、公证、包 minos 和目标系统装机各自记账。设置页的生效范围、窗格按钮显隐与普通/tmux 生命周期提示单列 UI 修正，不靠修改文档代替修正界面。

每项验收需附目标断言红、同变异独立对照绿、复原绿与 SHA256；上述步骤没有新增已执行闸门，也不改变 AGENTS 的合并要求。

## M5 Android 实施进度

设计遵循主机首页、单一终端键盘入口、长按多选与持续可见的文件复制清单；不添加主机分组。
[最终 HTML](../design/android-prototype.html) 为页面基准，[详细实现计划](android-implementation.md) 为开发与验收入口。

- [x] 单文件交互原型：主机/终端/文件/完整设置；旧三份 HTML 已合并删除。
- [x] 实现文档：线程/模块、配置/认证、输入法、SAF、四方向复制、16 项设置、生命周期、阶段与验收。
- [x] A0 Compose 工程、锁工具链、NDK/core/vt 依赖、JNI ABI 对拍与 API 36 arm64 模拟器冒烟；3 项设备测试、3 组 JNI 与 3 组 ABI 变异。
- [x] A1 导航与主机配置、安全写盘事务（API 36 arm64 模拟器档）：列表/搜索/增改、文本编辑、备份/冲突/原子替换已实现；来源查询、归属保护、多别名提示、显式覆盖/恢复继承与校验失败保留草稿已接入；别名重命名、配置读取/冲突错误恢复及重载放弃确认已交付；SAF UI 与其余表单校验已完成，证据见实现计划 §9.11。
- [ ] A2（首片已验，整个阶段未完成）真 PTY→VT→Canvas、InputConnection、会话切换/关闭、Activity 重建/系统键盘尺寸与后台读流：完整设备 63 项、终端 21 组变异通过；还须文本选择/复制粘贴/清屏、Ctrl、真实中文 IME 与性能，见实现计划 §9.13。
- [ ] A3 SSH/身份/主机验证、取消与多会话；接入前修共享 known_hosts 标记误信任。
- [ ] A4 私有目录/SAF/远端浏览、打开与多选。
- [ ] A5 共享安全复制前置与本机↔本机、本机↔远端、远端↔远端。
- [ ] A6 任务队列、进度、暂停取消、失败重试与清单身份。
- [ ] A7 设置持久化、已有 surface 即时生效、身份详情与恢复默认。
- [ ] A8 可选远端 tmux 管理与 attach。
- [ ] A10 设备矩阵、APK、CI 与发行验证。
- [ ] A9 环境包独立 PoC（延后）：Termux/proot 执行限制、下载/哈希/原子激活/回滚；不阻塞普通 SSH。

当前可构建 arm64 debug APK；首页读取真实配置并管理主机，设置有配置编辑/导入与组件版本；共享重复指令编辑已修复。A1 已按[实现计划 §9.3](android-implementation.md#93-当前代码入口与-a1-退出清单)完成当前设备档位的退出条件；最新 SAF/表单证据见该文 §9.11，§9.8 保留草稿复核历史。A2 首片已接真 PTY、Canvas、移动端输入与生命周期；此前 IME 超时已确认为夹具结束标记折行，修正后完整设备套件通过。整个 A2 仍待收尾，SSH 尚未实现。暂无 Android CI。min API 29、16 KB 页及真机均未验证。

**继承编辑已收口**：此前页面仍引用旧 `HostDraft` 字段造成的编译失败已解决；显式/继承独立状态、JNI 删除语义、候选配置保护和真实页面接线通过 32 项设备测试与 32 组变异。历史失败保留在[§9.6](android-implementation.md#96-文档复核时的工作区草稿2026-10-29)，当前交付见[§9.7](android-implementation.md#97-显式覆盖与恢复继承2026-10-30)。
来源查询与只读归属的代码/页面边界见[§9.5](android-implementation.md#95-字段来源与编辑归属2026-10-29)，不能从共享 Core 测试通过推断 Android 已验收。
A2 首片普通本地入口为系统 `/system/bin/sh`，不是打包一个新 shell；不能用桌面 PTY 证据代替 Android 真机验证。
旧 attachment ABI 已退役，Android 走现有 Transport + 可选 tmux；共享接口未冻结，缺口须同步各端后再使用。

主机改名已收口（2026-10-30）：Core、Swift/JNI 和 Android 表单接线通过 36 项设备测试、36 组配置/页面变异；Core 34 项 CTest/73 ABI、12 C + 1 Swift 改名变异及共享配置/Include 回归通过。**该切片完成时 A1 尚有余项（后由 §9.11 收尾），macOS GUI 仍未接改名**，详情见[§9.9](android-implementation.md#99-主机别名重命名2026-10-30)。

配置错误恢复已收口：读取失败/保存冲突后撤下旧快照，跨设置/关于返回仍保留重试入口；重载先确认草稿，成功刷新编辑目标、失败禁用保存。40 项设备测试和 47 组配置/页面变异通过，见[§9.10](android-implementation.md#910-配置错误恢复与重载确认2026-10-30)。该切片当时未验的 SAF/表单余项已由 §9.11 收尾：新增 6 项系统选择器用例及 1 项表单校验用例，完整设备测试 47 项与配置/页面变异 59 组通过。

## M6 及其他平台

- [ ] iOS：full libghostty iOS 构建/Metal 宿主先行；当前 SwiftPM App 仅 macOS，不能只加 `#if` 声称已兼容。iSH 环境包按需下载，不随 IPA 打包。
- [ ] HarmonyOS：libghostty-vt + ArkUI Canvas 与 NAPI 绑定。
- [ ] Linux：full libghostty + OpenGL 4.3 与 GTK4 原生宿主。
- [ ] Web：libghostty-vt wasm + 自绘；full 引擎等待上游 WebGL 后端。
- [ ] Windows：渲染引擎/后端待验证，ConPTY 尚无实现。

## 验收记账与历史记录

各阶段分别记账，不把同一天的多次验证合并成一次全平台验收：

| 记录 | 范围与结果 | 未覆盖 |
|---|---|---|
| `470c78b` 初始文档审计 | Core 32 项 CTest / 71 个符号、Apple 69 引用 / 2 登记、macOS 原生构建通过 | 无 Android 工程运行证据 |
| `4d83ec6` Android A0 | API 36 arm64、4 KB 页模拟器；3 项 JNI 测试、3 组 JNI 变异、3 组 ABI 变异；Core 与 Include 回归通过 | 自绘/输入/PTY/SSH、真机与 API 29/16 KB 页 |
| `4ed1308` Android A1 首片 | 同档模拟器 17 项设备测试；11 组配置/页面变异；C/Apple/JNI ABI 通过 | A1 完整收尾、交互终端/SSH/文件、Android CI |
| A1 共享重复指令修复（2026-10-29） | 同档模拟器 18 项设备测试、12 组配置变异；Core 写回 14 用例、配置 17 组变异、32 项 CTest；Swift 配置三项定向测试与 macOS 构建通过，C/Apple/JNI ABI 无漂移 | 字段来源/别名/Include 归属与 SAF UI；未重跑完整 Apple 宿主或 CI；日志 `tmp/android-a1-sparse/` |
| `9c0217f` 跨端文档复核（当时为 `adae205` + 来源接口草稿） | 工作区 Core 33 项 CTest、72 个公共符号通过；Apple 70 项引用/2 项登记、JNI 10 项引用/62 项待办对拍通过；macOS App 原生构建通过 | 新增来源接口的变异、两端页面接线与设备运行未验；未重跑完整 Apple 宿主或 CI；不改变 A1 或 macOS GUI 的交付状态；日志 `tmp/docs-current-audit/` |
| A1 字段来源与编辑归属（2026-10-29） | API 36 arm64 / 4 KB 页模拟器 22 项设备测试、20 组配置/页面变异；Core 33 项 CTest/72 ABI、17 组配置和 4 组 Include 变异；新增 8 组 C 元数据变异 + 1 组 Swift 绑定变异、Swift 配置 4 项测试与 macOS 原生构建通过；Apple 70/2、JNI 10/62 对拍及 JNI ABI 3 组变异通过 | A1 显式覆盖/恢复继承、重命名、SAF UI 仍待补；macOS 表单未消费来源；无真机/16 KB 页/Android CI 或本轮完整 Apple 宿主验收。日志 `tmp/android-a1-metadata/`、`tmp/config-metadata-mutations/` |
| 最终方案与跨端文档复核（`169cc60` + 既有继承编辑草稿，2026-10-29） | Core 33 项 CTest/72 ABI、Apple 70/2 与 JNI 10/62 对拍、macOS 原生构建通过；新增记录 macOS 配置失败后内存状态、否定模式过滤和传输控制/未知结果缺口 | Android `:app:compileDebugKotlin` 失败：页面未同步草稿 `HostDraft`；未运行这份草稿的设备/变异或完整 Apple 宿主/CI。仅文档更新，未修复上述实现。日志 `tmp/docs-final-audit/` |
| A1 显式覆盖与恢复继承（2026-10-30） | API 36 arm64 / 4 KB 页模拟器 32 项设备测试、32 组配置/页面变异通过；Core 33 项 CTest / 72 ABI、Apple 70/2、JNI 10/62 对拍与 macOS 原生增量构建通过；旧草稿的 Kotlin 编译失败已解决 | A1 重命名、SAF 与错误页面验收仍待补；无真机/API 29/16 KB 页/Android CI 或本轮完整 Apple 宿主验收；日志 `tmp/android-a1-inheritance/` |
| 跨端文档复核（`f1657e3` + 未提交重命名草稿，2026-10-30） | 本轮 Core 34 项 CTest/73 ABI、Apple 71/2、JNI 11/62 对拍、Android debug/测试 APK 与 macOS 原生增量构建通过；核对现有改名 12 C + 1 Swift 变异、Swift 5 项配置测试及 36 项设备测试日志；完善 macOS 修复顺序和验收条件 | 本轮未重跑设备/变异；当前 36 组配置变异及 parser 相关配置/Include 回归尚待收尾；未验完整 Apple 宿主、CI、像素/全屏、发行；仅提交文档，既有实现保持原样；日志 `tmp/docs-cross-platform-audit/` |
| A1 主机别名重命名（2026-10-30） | API 36 arm64 / 4 KB 页模拟器 36 项设备测试、36 组配置/页面变异；12 C + 1 Swift 改名变异、17 组配置与 4 组 Include 变异；Core 34 CTest/73 ABI、Apple 71/2、JNI 11/62 及 ABI 3 组变异、Swift 配置 5 项测试与 macOS 构建通过 | A1 SAF 与错误页面仍待验；macOS GUI 未接改名；无真机/API 29/16 KB 页/完整 Apple 宿主/CI 新证据；日志 `tmp/android-a1-rename/` |
| A1 配置错误恢复（2026-10-30） | API 36 arm64 / 4 KB 页模拟器 40 项设备测试、47 组配置/页面变异；Core 34 CTest/73 ABI、Apple 71/2、JNI 11/62 对拍通过；错误快照失效、重载确认、主机刷新与跨页错误保留已接入 | SAF UI 与其余表单校验仍待验；未改 Core/Apple 实现，无完整 Apple 宿主/像素/变异、CI 或新设备档位证据；日志 `tmp/android-a1-recovery/` |
| macOS 文档与进度复核（基于 `2386d0e`，2026-10-30） | Core 34 CTest/73 ABI、Apple 71/2、JNI 11/62 对拍及 macOS 增量构建通过；修正手动安装校验示例，离线夹具验正常、缺记录、重复、摘要错误、下载失败及仅其他资产六种路径；以生产 `UpdateCheck.swift` 的隔离编译探针确认清单校验缺口，并核对 CI 的跳过处理 | 仅文档；未修改生产实现/CI，未重跑完整宿主、设备、变异、像素或真实装机。安装示例的下载/解包使用替身，不算发布服务或系统安装证据。日志 `tmp/docs-macos-progress/` |
| A1 SAF 与表单收尾（2026-10-30） | API 36 arm64 / 4 KB 页模拟器 47 项设备测试、59 组配置/页面变异；Core 34 CTest/73 ABI、Apple 71/2、JNI 11/62 对拍通过；实际 DocumentsUI 与可写测试 provider 验源文档零写入和私有副本/备份字节 | A1 当前设备档完成；A2–A8/A10 仍待实施，无新真机/API 29/16 KB 页或 macOS 宿主/CI 证据；日志 `tmp/android-a1-saf/` |

A2 本地终端首片（2026-10-30）：API 36 arm64 / 4 KB 页模拟器完整 **63 项设备测试，0 失败/跳过**（原 A1 47 项 + 本次终端 16 项实际一并重跑）。**21 组终端、3 组 A0 native、3 组 JNI ABI、2 组 VT libc 守卫**变异通过，均目标红、独立对照绿、恢复绿与 SHA256；前两类使用同 APK 对照，后两类使用隔离副本/真实 archive。Core 34 CTest/73 ABI、Apple 71/2、JNI 19/54 及两端构建通过。没有新真机/API 29/16 KB 页、Android CI 或完整 Apple 宿主证据；整个 A2 未完成，剩余范围见实现计划 §9.13。日志 `tmp/android-a2-*.log`、`tmp/android-terminal-mutations/`。文档核验覆盖 30 份 Markdown 的 153 个本地链接/锚点，`git diff --check` 通过；最终 APK 截图复核了键盘显隐后的终端边界与辅助键/底栏，不能替代完整设计或真机验收。

`9497a14` 文档复核基于 `b028b38` 加 A2 未提交实现，未改生产代码。Core **34 项 CTest / 73 ABI**、Apple **71/2**、JNI **19/54** 对拍，以及 macOS 增量构建、Android debug/测试 APK 构建通过。只引用已有 A2 设备日志（9 项中 8 通过、1 项 IME 失败），没有重跑设备、完整 Apple 宿主、变异或 CI；A2 仍在研。29 份已跟踪 Markdown 的 150 个本地链接/锚点及 `git diff --check` 通过；起跑时 19 份既有实现/测试/补丁文件的 SHA256 保持一致。

另直接编译生产 `SettingsStore.swift` 与 `TabPresentation.swift`，在工程 `tmp/` 的隔离目录运行探针：重复 `font-size` 请求改成 18，回读仍为末行的 12；单键对照回读为 18；加载后外部新建文件的冲突检测为 false；将目录当配置读取得到空串。首次创建配置成功，未误记为缺陷。该探针只验证存储/文本行为，不是完整 UI/引擎验收；`AppModel` 先写盘再验证及忽略恢复错误的路径由源码核对。新缺口已列入 macOS P1，相关设置文案列入 P2。日志在 `tmp/docs-workspace-audit/`，不入库。

此前跨端文档复核基于 `186b95c`，更正标签计划残留的“未提交重命名草稿”和架构总览的 A1 首片状态。另从生产 `core/src/ssh.c` 原样抽取 known_hosts 读侧与匹配函数，编译隔离探针：普通、`@revoked`、`@cert-authority`、未知标记四种同主机同密钥条目均返回 MATCH；不同密钥返回 MISMATCH，不同主机返回 UNKNOWN。该证据确认的是本地匹配缺陷，不是新的真实 SSH/平台端到端结果。同步连接和异步认证消费 MATCH 的路径经源码核对；修复列入 P0，生产代码未改。当前核心闸门 34 项 CTest / 73 个公共符号、Apple 71/2 和 JNI 11/62 对拍及 macOS 增量构建通过；29 份 Markdown 的 146 个本地链接及锚点、`git diff --check` 通过。未重跑设备、完整 Apple 宿主、变异或 CI。日志与探针在 `tmp/docs-hostkey-audit/`，不入库。

`b6d6f7d` 的全仓文档链接检查覆盖 29 份已跟踪 Markdown；本地目标及 Markdown 锚点通过。tmux 的旧依赖清单已明确为退役前迁移记录，原 30/70 验收数字保留历史归属；当前共享接口数量按上方最新检出记账。文档中的 T1–T8 范围与终端变异脚本一致，只修正记录，不改变验收规则。

`27b7b64` 的文档检查覆盖 11 份修改文档的 102 个本地链接及锚点，全部通过；`git diff --check` 通过。起跑时 15 份既有实现/测试文件的 SHA256 保持一致。本轮 macOS 构建为增量通过，未重新编译所有文件，不能据此清除历史警告。

文档复核的 macOS 编译仍有 Swift 并发检查警告及 libssh2 的 macOS 26 minos 警告；原生构建通过不记成零警告或发行下限已修复。文档修正只更新事实与证据边界，不改变 `AGENTS.md` 的验收要求。

`1100596` 的文档复核还检查了 9 份修改文档的 75 个本地链接及锚点，`git diff --check` 通过；起跑时五份既有 Android 代码改动的 SHA256 保持一致。macOS 构建为增量编译通过，未重跑全部宿主测试，也不据此清除历史编译警告。Android 草稿构建失败独立记账，不将 ABI 对拍通过视为原生编译通过。

字段来源切片的首轮设备变异在 C15 因变异写法破坏 Kotlin 空值推导而编译失败，未算目标红；修正变异后完整 20 组重跑通过，最后再跑完整 22 项设备套件。共享元数据变异使用隔离副本，Swift 新绑定用独立 scratch 构建；不以旧可执行文件冒充新增 ABI 证据。

`0c60f19` 文档复核重跑：`bash scripts/gate.sh` **32 项 CTest / 71 个公共符号通过**；
`check-abi-apple.sh` **69 项引用 / 2 项登记通过**；`check-abi-android.py` **9 项引用 / 62 项待办通过**；
`swift build --package-path apple/GhostermApp` 通过。构建仍报告 libssh2 依赖的 macOS 26 minos 高于源码 macOS 13 声明，与上文发行下限缺口一致。
修改文档的本地链接与 `git diff --check` 通过；日志在工程 `tmp/docs-audit/`，不入库。
该文档提交未重跑 Android 设备/变异、macOS 完整宿主/像素/变异或 CI；既有结果只按上表与原实施记录引用。

最近一次完整 macOS 宿主记录见[标签界面重构末尾](plan-tab-chrome-refactor.md)：
341 项测试、1 项开发态资源跳过、0 失败，以及相应变异/像素证据；之后 Ctrl+Tab 切片另有六项定向测试和 menu 九组变异，未记为重跑完整 341 项。此前真实系统激活与全屏的测试范围另列。
统一工作区变异目前 21 组，属于手工证据；full CI 目前接入 20 份脚本，准确清单以 AGENTS 与工作流为准。

历史决策与当时测试规模分别保留在：

- [文件管理器 M4.6](plan-files-manager.md)、[面板/标签菜单](plan-panel-tab-menu.md)。
- [关闭确认、⌘T 与 cwd](plan-tabs-close-and-newtab.md)。
- [工作区交接](workspace-handoff.md)。
- [标签与窗格界面](plan-tab-chrome-refactor.md)。
- [可选 tmux 重构](plan-optional-tmux.md)及[实施清单](optional-tmux-implementation.md)。

旧脚本退役、测试数变化、历史 UI 被替代不能拿来删除仍有效的业务断言；每个新增闸门都需目标红、同变异独立对照绿、回滚复绿及 SHA256 一致。
