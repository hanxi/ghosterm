# macOS / Android 非插件待办核对

2026-11-17 对照源码 `d53c987a`、[路线图](roadmap.md)、[能力矩阵](capability-matrix.md)、两端 README 与实施记录核对。范围为普通终端、SSH/tmux、配置、设置、文件管理和分发；插件由另一 session 处理，本轮不审计或修改其计划、实现及状态。本文是工作索引，验收规则仍以 [AGENTS.md](../../AGENTS.md) 为准。

最初核对轮只做静态源码/文档与链接检查；后续实际实现及原生验证按文末各批次分别记账。下列代码缺口是源码事实；所引通过/失败均属于原记录，不是本轮重验。

## 已实现但总览滞后的项目

| 项目 | 本次纠正 | 证据与剩余边界 |
|---|---|---|
| macOS 应用内更新 | 不再把 commit/摘要/下载来源校验或下载重启列为待实现 | [UpdateCheck.swift](../../apple/GhostermApp/Sources/GhostermAppModel/UpdateCheck.swift) 已校验字符集、资产及渠道基址；Sparkle/Ed25519 与本地安装专项见[更新说明](../how-to/macos-updates.md)。实际 GitHub 分发、首次手装迁移和权限/系统矩阵仍待验 |
| macOS 设置页像素失败 | 从“布局待修”改为“布局已修，当前完整宿主待跑” | [字体记录](macos-terminal-fonts.md)已有高度/正文布局修复和关联 56 通过/1 跳过；`4cbfcf4` 完整失败不抹去，也不能用定向结果记新完整绿 |
| macOS SFTP 打开 | 原认证余额与取消已贯通 | 普通文件工厂调用已有受控重载，取原余额与五秒上限较小值，取消直接置令牌；实际 SFTP INIT 等待有自有 sshd 证据，见[打开说明](macos-ssh-opening.md)。浏览/传输/关闭仍未迁移 |
| Android 会话/IME | 总览补到固定会话按钮、同窗口覆盖面板与切换保持 IME | [TerminalScreen.kt](../../android/app/src/main/java/com/hanxi/ghosterm/TerminalScreen.kt)、实施记录 §9.207–§9.208；自动化按模拟器档记账，OEM 输入法另列人工手测 |
| 统一连接入口与 ProxyJump | 不再泛称“高级 SSH 选项均未实现”，也不再把默认入口描述成旧后台菜单 | [统一入口记录](optional-tmux-implementation.md#远端统一连接入口2026-11-06)、[§9.214](android-implementation.md#9214-proxyjump-普通终端sftp-与-tmux2026-11-08)已有两端普通终端/SFTP/tmux；自动查询不授权创建/attach，ProxyCommand/转发等仍未实现 |
| Android 出包 | 删除“无 Android workflow / 无 16 KiB / 无发行证据”的旧概括 | [android-release.yml](../../.github/workflows/android-release.yml)已接签名优化包和对齐检查，历史实际包/设备证据见[发行说明](../how-to/android-release.md)。CI 不跑完整设备套件；当前源码与实际发行包仍需分别验收 |

## macOS 可以直接拆分实施的工作

顺序兼顾严重程度与依赖；小切片可先并行排期，但不依赖正在进行的插件实现。完整原始退出条件保留在[路线图](roadmap.md#macos-下一步与共享缺口)。

| 顺序 / 优先级 | 可做切片 | 源码落点与缺口 | 本片交付 / 验证边界 |
|---|---|---|---|
| 1 / 已修 | SSH 配置候选模型发布 | ConfigStore 存储事务与 AppModel 候选发布均已接；失败不污染已发布 AST/列表，已提交后错误重载磁盘并明确报告 | 50 项原生组合、六组实际编译变异和 arm64 Release 通过；主机表单/来源与改名另见批 3，不扩大为 M3 全完成 |
| 2 / 已交付专项 | 主机表单保真与改名 | Core 物理块快照替换 Swift 解析；脏字段、逐行身份、继承来源、多别名提示、只读/歧义保护；改名预览确认及旧表单冲突 | 结果逐项记入[剩余实施记录](non-plugin-remainder.md)；重复未知标量及 Include 仍显式转来源文件编辑，不声称全指令表单支持 |
| 3 / 已交付专项 | 私钥安全存储生产接线 | IdentityFile 首次导入后从 Keychain 回读认证，表单可主动重新导入；写/读失败明确终止，取消隔离，来源文件仍由用户管理 | 46/105 项组合分轮、十八项真实 SSH、十四组变异和 Release，见[材料说明](macos-private-keys.md)；Android 引用不自动迁移 |
| 4 / 已交付 | 宿主闸门必跑项 | full CI 两个 Apple 闸门显式 required；缺 Swift/SSH、启动失败、空测试、必跑项无 passed 收据均失败；optional 缺证据报 PARTIAL | Bash 3.2/5 各 18 项、六组 shell 故障断言及自有 sshd 原生专项通过；其它条件跳过仍单列，见[分批实施](non-plugin-remainder.md) |
| 5 / 部分完成 | 文件导航/错误/缓存 | 最新导航身份、历史成功提交、刷新 epoch、树失败可重试及 cwd 同目标去重已接 | 最终 34 项/十组变异/33 项真实 SSH 与 Release，见[导航说明](macos-files-navigation.md)；搜索取消/部分失败另见[搜索专项](macos-file-search.md)，递归权限仍待做 |
| 6 / P1 | 传输控制与批次结果 | [TransferManager.swift](../../apple/GhostermApp/Sources/GhostermAppModel/TransferManager.swift) 忽略控制错误、取消立即终态、晚到结果丢弃；全局“应用到全部”、目录摘要/重试范围不完整 | 按请求/确认终态记账，owner 释放后才结束；冲突限本批、目录汇总与完成顺序无关，成功项不盲目重传。建议拆控制、批次、目录三片 |
| 7 / 本批实现并定向验证 | 文件 owner 与安全传输 | `SFTPFileBackend` 串行持有 SFTP 调用；下载在同目录暂存后原子发布；上传写入远端随机同目录暂存文件，成功关闭后原子发布；覆盖使用 POSIX rename，无覆盖使用标准 rename，失败/取消明确报告暂存路径。目录展开记录本批创建目录，mkdir 回包不明保留 UNKNOWN | Core 构建和真实 sshd 的 `sftp_transfer_e2e` 通过；Swift 生产包构建通过；XCTest 当前环境不可用，完整宿主验收仍待跑。Android 的通用 staged-upload ABI 明确 deferred：Android A5 用自己的受控发布执行器 |
| 8 / 已交付专项 | 终端配置读写与应用 | 存储事务、内存候选预校验、提交后应用、条件状态回显及旧编辑器保护已接 | 证据/提交后应用错误与恢复边界见[设置说明](macos-terminal-settings.md)，不承诺多文件/全部 surface 原子事务 |
| 9 / 部分完成 | SSH 多地址与打开取消 | 普通 shell/SFTP 原认证余额、独立取消和晚到交接已接；DNS 仍首个有效地址即结束 | 打开切片见[说明](macos-ssh-opening.md)；TCP 候选推进与原 hostname 身份仍待做，SSH/SFTP/tmux 和 ProxyJump 首跳各自验收，不能借打开接线关闭整个网络矩阵 |
| 10 / 已交付专项 | 剪贴板粘贴检查与确认 | 用户菜单进入引擎检查，捕获文本、单次授权、窗格撤销和实际选区复制已验；真实进程菜单 nil-target 响应链有后台夹具 | 终端 47 项/新十六组/旧 OSC 52 六组，原生确认和窗口查询注入范围见[说明](macos-clipboard.md)；系统前台热键/跨应用激活另记，非完整剪贴板协议矩阵 |

共享事件队列现为 job 提交时预留终态节点，完成事件不再依赖满载环形队列分配，并在终态前清除该 job 的过期进度；旧 job 下载现校验尺寸、上限和关闭结果并以同目录暂存原子发布。Core 负深度 walk 已有 64 层硬护栏；递归权限先完整扫描再修改并跳过链接。以上新代码须以本批定向验收为准，不能借历史测试提前记绿。macOS 信任记录管理页仍可接现有快照/事务 API；别名改名应排在配置事务/表单之后。

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
- 闸门失败传播批：仅改 `apple-app-gate.sh` 的子检查返回值处理，保留现有入口、过滤器和 SSH 跳过策略；独立 shell 验证不执行插件。依赖桩最初遗漏 PATH 工具及误选系统 Bash 3.2 的失败未计有效红；修正夹具后，原实现只有 ABI/HTML 两条正常断言失败，Core/Swift 错误与全成功对照通过。最终 PATH Bash 5.3.20 五项通过；两组撤回修复分别正常断言红/对照绿/恢复绿及 SHA256。未运行完整原生宿主或 CI。系统 Bash 3.2 的空数组兼容由下一独立批处理。
- Bash 3.2 兼容批：修复环境/测试过滤器空数组在 nounset 下的展开，过滤器仍按一个参数传递。Bash 3.2 / 5.3.20 各六项 shell 检查通过，一组撤回修复单条正常断言红/独立对照绿/恢复绿/SHA256。未改插件代码或 SSH 跳过策略，未执行原生业务。
- ConfigStore 独立写盘批：强制备份、稳定非阻塞保存锁、字节/身份快照、原子替换、首次创建排他提交、提交后错误和重载保护已接。最终同源 43 项与 22 组完整 native 变异、arm64 Release、隔离 Core 589/185 与 C 格式 276 通过；不改 AppModel 或插件。完整范围/前置失败与剩余模型发布见[写盘事务](macos-config-store.md)。
- 普通设置文案批：六处启动、普通/tmux 关闭、字号、标题栏动作及配置重载提示已修，不改偏好/生命周期。与已提交 ConfigStore 组合的 arm64 Release 通过；97 项为 95 通过/1 开发态资源跳过/1 既有插件页像素失败，四个普通页原像素断言通过，未改原版复现同一插件页失败。完整数字不记全绿，也不改插件页，详见 Apple 说明。
- 上述两代码批收尾：格式工具以工程 TMPDIR/缓存重建后复验通过；仅移除本轮 cfg17/set17 两个隔离 worktree，主输入摘要不变。临时原件已清理，证据口径与工具前置修正见[ConfigStore 收尾](macos-config-store.md)。
- 剩余批 1：AppModel 配置候选发布完成。50 项组合与六组完整 native 变异通过（单条正常断言红/独立读取绿/恢复绿/SHA256），arm64 Release 通过；失败草稿/旧模型保留，提交后错误重载观察。仅改配置写侧方法，未纳入插件暂存内容。后续范围见[剩余实施记录](non-plugin-remainder.md)。

- 剩余批 2：Core 原始块指令快照、等号/长参数保真和失败解析保护已提交；两端绑定同步。590 CTest/186 ABI、Swift 四项、Android 专用 API 36 模拟器 35 项、Release/R8/四模块 lint 及八组编译变异通过，前置失败单列于[剩余实施记录](non-plugin-remainder.md)。主机表单另批接线。

- 剩余批 3：主机表单共享快照/脏字段/来源/逐行身份/改名预览确认、旧窗口保护、否定过滤与列表去重已接。73 项原生组合、16 个新保护＋发布旧六组分轮、arm64 Release 通过；Include 只读/歧义拒绝与前置失败保留，见[分批实施](non-plugin-remainder.md)。

- 剩余批 4：普通连接/SFTP/tmux/分屏与选择器统一别名字节身份，避免 Unicode 规范等价别名借用另一行的地址/凭据。50 项连接组合、76 项配置组合分轮、两组变异与 Release 通过，见[分批实施](non-plugin-remainder.md)。

- 剩余批 5：full CI 必跑 Swift/SSH 收据，optional 缺证据只报 PARTIAL；两版 Bash 各 18 项、六组 shell 断言故障与真实本机 SSH 原生专项通过，详见[分批实施](non-plugin-remainder.md)，没有把其它条件跳过记为完成。

- 剩余批 8：文件搜索取消/部分结果、明确 exec 回退、范围与输出预算已接；最终 68 项、21 组变异、26 项真实本机 SSH 组合与 Release 通过，范围及前置失败见[搜索说明](macos-file-search.md)。递归权限/传输 owner 仍分批推进。

- 剩余批 9：终端设置存储/读取/CRLF/重复声明及真实重新读取入口完成；80 项与真实本机 SSH 85 项分轮、十二个新保护/旧事务二十二组、Release 通过，见[设置说明](macos-terminal-settings.md)。引擎候选校验/应用下一片处理。

- 剩余批 10：终端设置内存候选校验/提交后应用、递归来源/条件回显、旧编辑器和打开页面不写盘完成。99 项定向/104 项本机 SSH 组合、17 组变异、Core 596/190、C 格式、四补丁闸门与 Release 通过，完整范围见[设置说明](macos-terminal-settings.md)。

- 剩余批 11：普通 SSH/SFTP 原预算/取消、旧完成隔离和取消状态已接；62 项真实本机 SSH 组合、真实协议等待与二十个新保护分轮，旧保护和 Release 收据见[打开说明](macos-ssh-opening.md)。多地址与后续文件 IO 仍分批推进。

- SFTP staged upload / 目录副作用批（2026-11-18）：新增 Core 远端随机暂存上传，成功 close 后发布；覆盖要求服务端 POSIX rename 扩展，不覆盖使用 SFTP 标准 rename，发布失败/取消报告保留的暂存路径。macOS `TransferManager` 按冲突选择传递覆盖语义。目录上传/下载展开在失败终态报告已创建目录；mkdir 回包不明为 UNKNOWN。`bash scripts/build-core.sh` 与自有 sshd `sftp_transfer_e2e` 通过；`swift build --package-path apple/GhostermApp` 通过；`swift test ... --filter TransferManagerTests` 因当前活动开发目录仅为 Command Line Tools、缺少 Xcode/XCTest 而失败，未计测试通过。Core ABI 206、Apple 203/3、JNI 167/39 对拍通过；clang-format 19.1.7 全量 297 文件检查通过；未执行 `gate.sh`。通用 staged-upload API 不被 Android 消费，已在 `android/bridge/deferred-abi.json` 登记：Android A5 采用独立受控发布执行器。
- Curve25519/SSH 握手分配复验（2026-11-18）：以本机 sshd 运行普通非 ASan `test_ssh`，291 项通过；macOS `leaks --atExit` 报告 0 bytes leaked，但系统限制使堆检查只能读取受限进程内容；`test_sftp_submit` 的 ASan 进程不能由 `leaks` 检查。该结果没有证明所有密码后端/系统组合无泄漏，也没有触碰或更改上游依赖。
- SSH 周期保活仍待实现：当前 libssh2 公开接口提供按调用发送 keepalive 的能力，但不公开未应答计数；目前连接读循环与 SFTP owner 也没有共用的空闲计时调度。不能因此放宽 `ServerAliveInterval` 准入或声称 `ServerAliveCountMax` 有效。后续需补可观察的回包/超时判据和两端 owner 调度后再接配置；Android 自动化仍只按模拟器验证，真机属于人工手测。
