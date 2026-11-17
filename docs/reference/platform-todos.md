# macOS / Android 非插件待办核对

2026-11-17 对照源码 `d53c987a`、[路线图](roadmap.md)、[能力矩阵](capability-matrix.md)、两端 README 与实施记录核对。范围为普通终端、SSH/tmux、配置、设置、文件管理和分发；插件由另一 session 处理，本轮不审计或修改其计划、实现及状态。本文是工作索引，验收规则仍以 [AGENTS.md](../../AGENTS.md) 为准。

本轮只做静态源码/文档核对和文档链接检查，没有运行原生构建、设备、变异、完整回归或线上发行查询。下列代码缺口是源码事实；所引通过/失败均属于原记录，不是本轮重验。

## 已实现但总览滞后的项目

| 项目 | 本次纠正 | 证据与剩余边界 |
|---|---|---|
| macOS 应用内更新 | 不再把 commit/摘要/下载来源校验或下载重启列为待实现 | [UpdateCheck.swift](../../apple/GhostermApp/Sources/GhostermAppModel/UpdateCheck.swift) 已校验字符集、资产及渠道基址；Sparkle/Ed25519 与本地安装专项见[更新说明](../how-to/macos-updates.md)。实际 GitHub 分发、首次手装迁移和权限/系统矩阵仍待验 |
| macOS 设置页像素失败 | 从“布局待修”改为“布局已修，当前完整宿主待跑” | [字体记录](macos-terminal-fonts.md)已有高度/正文布局修复和关联 56 通过/1 跳过；`4cbfcf4` 完整失败不抹去，也不能用定向结果记新完整绿 |
| macOS SFTP 打开 | 不再写成“仍调用无参 sftp” | [AppModel.swift](../../apple/GhostermApp/Sources/GhostermAppModel/AppModel.swift) 已用 `sftp(timeoutMilliseconds: 5000)`；缺原认证剩余预算/取消，浏览/传输仍未迁移，不因此关闭整个待办 |
| Android 会话/IME | 总览补到固定会话按钮、同窗口覆盖面板与切换保持 IME | [TerminalScreen.kt](../../android/app/src/main/java/com/hanxi/ghosterm/TerminalScreen.kt)、实施记录 §9.207–§9.208；自动化按模拟器档记账，OEM 输入法另列人工手测 |
| 统一连接入口与 ProxyJump | 不再泛称“高级 SSH 选项均未实现”，也不再把默认入口描述成旧后台菜单 | [统一入口记录](optional-tmux-implementation.md#远端统一连接入口2026-11-06)、[§9.214](android-implementation.md#9214-proxyjump-普通终端sftp-与-tmux2026-11-08)已有两端普通终端/SFTP/tmux；自动查询不授权创建/attach，ProxyCommand/转发等仍未实现 |
| Android 出包 | 删除“无 Android workflow / 无 16 KiB / 无发行证据”的旧概括 | [android-release.yml](../../.github/workflows/android-release.yml)已接签名优化包和对齐检查，历史实际包/设备证据见[发行说明](../how-to/android-release.md)。CI 不跑完整设备套件；当前源码与实际发行包仍需分别验收 |

## macOS 可以直接拆分实施的工作

顺序兼顾严重程度与依赖；小切片可先并行排期，但不依赖正在进行的插件实现。完整原始退出条件保留在[路线图](roadmap.md#macos-下一步与共享缺口)。

| 顺序 / 优先级 | 可做切片 | 源码落点与缺口 | 本片交付 / 验证边界 |
|---|---|---|---|
| 1 / P0 | SSH 配置保存事务与模型发布 | [ConfigStore.swift](../../apple/GhostermApp/Sources/GhostermAppModel/ConfigStore.swift) 忽略备份失败、先删后移；`AppModel.applyEdits/addHost` 先改内存后落盘 | 备份失败阻断，同目录原子替换，候选落盘成功后才发布；覆盖备份/替换/并发变化/提交后错误。可先做，不等表单重构 |
| 2 / P0 | 主机表单保真 | [HostListModel.swift](../../apple/GhostermApp/Sources/GhostermAppModel/HostListModel.swift) 简化扫描，[HostEditorSheet.swift](../../apple/GhostermApp/Sources/GhostermApp/HostEditorSheet.swift) 提交全部字段、多条身份只回显最后一条 | 共享语法/来源与脏字段接线，未改保存不变；Include/重复块保持归属保护。别名改名依赖此片与事务片 |
| 3 / P0 | 私钥安全存储生产接线 | [SSHCredentialSelector.swift](../../apple/GhostermApp/Sources/GhostermAppModel/SSHCredentialSelector.swift) 从受限磁盘文件读 PEM，Keychain 生产路径保存口令/密码 | 私钥导入→安全存储→解锁→认证，引用/取消/错误明确；不把 Android 容器引用当本端路径，不放宽铁律 #5 |
| 4 / P1 | 宿主闸门必跑项 | [apple-app-gate.sh](../../scripts/apple-app-gate.sh) 的 ABI/HTML 子检查失败传播已修；sshd 失败可 SKIP 后 PASS 的策略仍未收口 | 失败传播五项 shell 检查与两组撤回修复验证已通过，非原生业务证据。后续明确 full 档 SSH 必跑策略，独立验证缺环境会失败；不删测试或把缺环境改成通过 |
| 5 / P1 | 文件导航/错误/缓存 | [FilesPanelModel.swift](../../apple/GhostermApp/Sources/GhostermAppModel/FilesPanelModel.swift) 无导航代次，失败不回滚历史，刷新不失效树缓存，树错误折为空 | 可从导航代次与历史事务开始；覆盖快速 A/B、后退失败、树失败重试、刷新增删，随后独立收口搜索/递归权限错误 |
| 6 / P1 | 传输控制与批次结果 | [TransferManager.swift](../../apple/GhostermApp/Sources/GhostermAppModel/TransferManager.swift) 忽略控制错误、取消立即终态、晚到结果丢弃；全局“应用到全部”、目录摘要/重试范围不完整 | 按请求/确认终态记账，owner 释放后才结束；冲突限本批、目录汇总与完成顺序无关，成功项不盲目重传。建议拆控制、批次、目录三片 |
| 7 / P1 | 文件 owner 与受控读写/发布迁移 | [SFTPFileBackend.swift](../../apple/GhostermApp/Sources/GhostermAppModel/SFTPFileBackend.swift) 仍用旧 list/job，Bool 存在性吞错误、主线程同步等待；Quick Look 无预算/代次/回收 | 先建立独占 owner/取消和查询三态，再接 fd 暂存/完整性/非覆盖发布，最后预览生命周期；共享 API 已有，不能与同会话旧 job 并发混用 |
| 8 / P1 | 终端配置读写 | [SettingsStore.swift](../../apple/GhostermApp/Sources/GhostermAppModel/SettingsStore.swift) 读取失败当空，外部新建不冲突，重复键读改不一致；引擎校验/恢复顺序待修 | 不存在/失败分开，候选先验证，提交后才发布；实际已开 surface 字号/配色与磁盘一致。此存储与 SSH ConfigStore 分开实施 |
| 9 / P1 | SSH 多地址与 shell 打开取消 | [SSHAddressResolver.swift](../../apple/GhostermCore/Sources/GhostermCore/SSHAddressResolver.swift) 首个有效地址即结束；`AppModel.startSSH` 用旧 shell opener | 两片分别做：TCP 阶段候选推进；认证剩余 deadline/取消贯通 channel→PTY→shell。SSH/SFTP/tmux 各入口验证，ProxyJump 首跳也不能沿用新能力的假定 |
| 10 / P1–P2 | 剪贴板与设置文案收尾 | 菜单 paste 仍直接输入；[SettingsView.swift](../../apple/GhostermApp/Sources/GhostermApp/SettingsView.swift) 仍提示仅新标签生效/恢复会话/关闭结束 | 粘贴确认与全局菜单路由单独验证；文案可独立小修，但不能借文案变更记配置事务已修 |

共享事件队列终态保留/查询、旧 job 下载完整性、负深度 walk 硬护栏、递归权限链接处理仍有效，属于文件迁移的相关工作，不借新绑定或 Android 验收关闭。macOS 信任记录管理页也可接现有快照/事务 API；别名改名应排在配置事务/表单之后。

## Android 可做的实现与验证

**Android 验证分工（2026-11-17 用户确认）**：自动化测试只使用模拟器；真机仅安排人工手动测试，不列为自动化开发待办，也不因没有真机自动测试反复报告功能缺失。历史记录保留实际测试环境；人工手测结果单独记账。

| 类型 | 待办 | 起点与交付边界 |
|---|---|---|
| 可实施的新能力 | SSH 周期保活 | 当前 [config.c](../../core/src/config.c) 只允许 `ServerAliveInterval=0`；前台服务/wake lock 不提供 SSH 协议保活。先定义 interval/count、无回复/取消和 owner 调度，再接共享核心及两端消费者与准入，不能仅放宽配置允许表，也不承诺换网不断线 |
| 可立即组织的回归 | 非插件当前源码完整模拟器基线 | 最近同源完整仍为 `ae164f1` 双系统各 942 项；后续 §9.200–§9.214 的专项不替代全量。固定输入/APK/发现清单重新跑，按现有 runner 明确默认套件与关闭插件的范围，失败不算通过 |
| 模拟器自动化 | 后台/Doze/取消/进程死亡/网络故障 | 已有终端进程 owner、前台服务和任务记录；在指定模拟器与隔离服务上验证可控制的后台、断网/延迟、取消及 SFTP/tmux 角色认证组合。网络恢复不自动重放副作用；运营商/厂商网络行为另列人工手测 |
| 模拟器自动化 | 输入/字体/布局/性能 | 会话面板/IME、触屏、连发、Unicode/emoji 已实现；可补模拟器输入法、按键/布局、大字号、无障碍节点、挖孔/旋转和优化 Release 长负载。记录模拟器/API/页大小，不据此推定手机表现 |
| 模拟器自动化 | 文件与发行包回归 | HOME/SAF/SFTP、四方向复制、历史核对/重试/清理已有实现；补可控 provider/隔离 SSH 服务/端点死亡、最终签名包升级与 16 KiB 模拟器实际操作，精确记录组合，避免再建一套任务/恢复机制 |
| 需另定范围的扩展 | 设备存储直接粘贴、本地 tmux/环境包 | [FileDestinationAccess.kt](../../android/data/src/main/java/com/hanxi/ghosterm/data/FileDestinationAccess.kt) 对 DEVICE 目标明确拒绝，当前通过 SAF 授权写入；不是已有复制按钮漏接。A9 已决定延后，不能当修 bug 顺带开启。跨重启自动恢复执行同样不属于当前承诺 |

## 尚需外部条件或单独安排

Developer ID/公证需要实际证书与发行；macOS 15、真实双屏/全屏/Finder/Quick Look 和实际 GitHub 更新迁移需对应环境。Android 的手机/OEM 输入法、实体键盘、移动网络切换、SD/USB 与 TalkBack 实际体验统一列为人工手测；仅在安排人工手测或收到相关问题时展开，不阻塞可在模拟器完成的自动化工作。provider/WAN 测试使用明确授权的对象。

已入库的出包工作流、历史发行和本地实现分别保留；本轮没有把 2026-11-04 线上快照改称最新下载。无需证书也能做 ad-hoc 更新验收，不能把 Developer ID 作为该工作的前置条件。

## 分批处理记录

- 文档批 `06dee62a`：同步非插件现状与 Android 模拟器自动化/真机人工手测分工。
- 闸门失败传播批：仅改 `apple-app-gate.sh` 的子检查返回值处理，保留现有入口、过滤器和 SSH 跳过策略；独立 shell 验证不执行插件。依赖桩最初遗漏 PATH 工具及误选系统 Bash 3.2 的失败未计有效红；修正夹具后，原实现只有 ABI/HTML 两条正常断言失败，Core/Swift 错误与全成功对照通过。最终 PATH Bash 5.3.20 五项通过；两组撤回修复分别正常断言红/对照绿/恢复绿及 SHA256。未运行完整原生宿主或 CI。系统 Bash 3.2 的空数组兼容另行处理。
