# 能力矩阵

2026-10-28 已完成 [可选 tmux 重构](plan-optional-tmux.md)。默认 PTY/SSH 已恢复，
旧 sessiond、私有恢复与随包 runtime 已退役。可选 tmux 为 🟡：支持证据限于 macOS 本机与 Ubuntu 22.04 SSH 的 tmux 3.6a；真实命令/PTY、原生列表、任务 PID 保留、断网与受限账户已验证，最终打包证据见实施清单。仅支持 POSIX 兼容远端登录 shell；不自动安装、恢复或管理旧 daemon。
进度见 [实施清单](optional-tmux-implementation.md)。

> 口径（铁律 #8）：**✅** 已实现且有闸门/实测证据 · **🟡** 已实现但只在某一档证据下成立
> （必须写明哪档）· **⏳** 契约/占位在，无实现 · **❌** 未开始 · **⛔ WONTFIX** 不做并注明原因。
>
> 核实日期：**2026-10-10**（M2/M3/M4 收口：对话框 / Keychain / 编辑器 / SFTP）。

## 核心（C）

| 能力 | 状态 | 证据 |
|---|---|---|
| 稳定 C ABI（`ghosterm.h` v0） | 🟡 | 64 符号双向对拍（`scripts/check-abi.sh`）；**接口未冻结，破坏性变更允许** |
| `ssh_config` 解析（AST + Host 块） | ✅ | `ctest sshconfig_golden`：4 个 Host 块 / 6 个模式 |
| `ssh_config` 无损往返（**未编辑**） | ✅ | 与原文**逐字节相同**（`test_config` 头号断言） |
| `ssh_config` 求值（typed） | ✅ | `gt_config_resolve`：user/hostname/port/forwardagent/compression/serveraliveinterval/identityfile/proxyjump/setenv/localforward |
| `ssh_config` 求值（通用视图 = `ssh -G`） | ✅ | `gt_config_effective`：**42 键 × 6 host = 252 项**全表语义对拍（布尔归一化）；支持集双向校验 |
| 默认 IdentityFile 候选链 | ✅ | `gt_identity_policy_t` 五档（9_6=7 / 10_3=5 / MASTER=6 / NONE / CUSTOM），每条链注明出处；回落机制有断言 |
| `ssh_config` **编辑** API（改值后无损写回） | ✅ | `gt_config_edit`：`sshconfig_writeback` 9 用例逐字节对拍（`fixtures/writeback.json`）+ `=` 分隔符保留补充断言；B1~B6 变异有牙 |
| `Include` 展开 / glob | ✅ | `process_include`：相对路径基准 = **~/.ssh**（OpenSSH_10.3p1 实测 + 真 ssh `-G` 同口径对拍）、`~`/`~user`、glob 无匹配静默 / 具名缺失显式失败 / 嵌套 + 深度上限 16；被含内容不进写回（铁律 #4）；`test_config` 6 组断言 + 变异 I1~I4 有牙 |
| 事件队列（低频、批量、非阻塞） | ✅ | `src/core.c` 环形缓冲 + 锁；`test_api`；SFTP job（PROGRESS ~16ms 合帧 / JOB_DONE 三态）经此上报 |
| Transport 接缝（本地 PTY） | ✅ | `gt_transport_open_pty`（POSIX forkpty）；`pty_transport` 行为断言（真 forkpty，离线）：EOF 前不丢最后一批 / 退出码 / 字节往返 / 超时不折叠 EOF / winsize / resize / TERM / close(NULL) 安全 / 前台作业四态 / 内核 cwd / **写绝不无限等**（第 13 节：pty 输入队列满时写要有界返回且积压字节随后补送——旧实现的 `poll(-1)` 会钉死会话线程 → 关标签页时 `queue.sync` 把主线程也钉死＝整个 app 无响应）；变异 `mutation-check-transport` P1~P4 有牙。**Swift 端** `StopBoundedTests` 直接断言 `SessionController.stop()` 经 `queue.sync` 屏障有界返回（走生产链路 `sendText → onInput → 会话线程 write`，子进程 `stty -icanon -echo` 才会真触发 EAGAIN 而非 canonical 丢弃），对 P4 验过牙（变异体挂起超时 / 干净 0.7s 绿） |
| Transport 接缝（SSH shell） | ✅ | `gt_transport_open_ssh_shell`（libssh2 pty-req+shell）；`ssh_e2e` 真 sshd 回显/resize |
| Transport：前台作业判定（关闭确认用） | ✅ | `gt_transport_child_state` **四态**（UNKNOWN/EXITED/IDLE/JOB）：POSIX 读内核 `tcgetpgrp(master) == shell 自己的进程组` 判「空闲提示符 vs 有作业」，SSH / ConPTY 报 `UNKNOWN`（**不猜**，铁律 #6）。`test_transport` 第 11 节（IDLE/JOB/EXITED/NULL→UNKNOWN）+ 变异 `mutation-check-transport` P1/P2 有牙。**前置修复**：本地 PTY 子进程曾拿不到控制终端（父进程抢跑 `setpgid` → 子进程 `setsid` 报 EPERM），已修 |
| Transport：会话 cwd | 🟡 | **本地 PTY ✅**：`gt_transport_child_cwd` 内核查询（macOS `proc_pidinfo` / Linux `/proc/<pid>/cwd`），`test_transport` 第 12 节（继承 cwd + `cd` 后随之变）。**远端 cwd 未交付**：OSC 7 在我们钉的 embedded host-IO 下到不了宿主（无 `GHOSTTY_ACTION_PWD`），且 libghostty 要求 host `isLocal`——接线留为 seam，依赖上游支持 / roadmap R2 |
| SSH（libssh2） | ✅ | connect / pubkey·password 认证 / 主机密钥三态 / TOFU；`ssh_e2e` 14 项断言（真 sshd，环回离线）。**写绝不无限等**（§5b：远端不读 stdin 时 `gt__ssh_shell_write` 在 `GT_SSH_WRITE_GRACE_MS` 内有界返回、积压字节走 `wtail` 队列随后补送——与本地 PTY §13 同结构。`TIMEOUT`/`EAGAIN` 不再当硬错丢字节（旧实现把两者都算 `GT_ERR_IO` → 键入的字节就这么没了）。写路径自己设/还 session timeout，不打扰同 session 的 SFTP。e2e 限制：sshd 排空速率 ≈ 40 MiB/s，3 MiB payload 写完时队列**根本没形成**——flush 机制无法被真 sshd e2e 触发，由代码审阅 + PTY §13/P4 的变异覆盖保证同构正确性） |
| SFTP | ✅ | `gt_sftp_*`（libssh2_sftp 封装）：list / stat / lstat / readlink / realpath / mkdir / rmdir / remove / remove_all（递归）/ rename / chmod / walk（递归枚举，visitor 可取消）/ upload / download（job + 事件三态）/ cancel / pause；**每会话一把 I/O 锁 + 单条 I/O worker 线程**（修掉「每 job 一线程共用同一 libssh2 会话」竞态）。e2e 真 sshd 75 项断言 + 变异 `mutation-check-sftp` 10 项 / `mutation-check-exec` 4 项 |
| SSH exec 通道 | ✅ | `gt_ssh_exec*`：远端单命令（stdout/stderr 分离 + 退出码）；把递归搜索/大树遍历放到远端本地算（SFTP readdir 是每条目一次往返）。服务器禁 exec → `GT_ERR_UNSUPPORTED`（不静默降级）。e2e 含**性能对比**（300 文件：exec-find vs sftp-walk，两条路都找全；**环回上 exec 反而慢 ~14×**——起远端进程的开销，优势只在真实 RTT 链路显现）。变异 4 项 |
| 端口转发 -L/-R/-D | ❌ | — |
| known_hosts / TOFU | 🟡 | 明文主机名匹配 + TOFU 落盘 ✅；**hashed 条目 / @revoked 未支持**（命中不了→UNKNOWN，不误判） |
| 本地 PTY | 🟡 | POSIX（macOS/Linux）forkpty ✅；Windows ConPTY ⏳（能力缺失显式报错） |
| 可取消的 SSH 认证 | 🟡 | 通用公开 `ssh_auth.h` 与 Apple `SSHAuthenticator`/DNS 解析已从旧安装链拆出；C 连接器与公开翻译测试、8 组认证 ABI / 6 组凭据选择变异通过。产品入口统一接线仍在实施。 |
| POSIX PTY 尺寸能力 | 🟡 | macOS 共享后端 `pty_dimensions` 三类基线随核心闸门通过；保留像素尺寸与默认值校验，非全平台证据。 |
| 可选 tmux 后台会话 | 🟡 | macOS 本机与 Ubuntu 22.04 SSH 的 tmux 3.6a 真 PTY attach、列表/管理、原生键鼠与任务保留；TCP reset/静默断网、背压、未知回复及受限账户验证。仅 POSIX 兼容远端 shell；其他版本/平台未验，不自动恢复，受用户 tmux 与系统清理策略影响。 |
| 配色集随包 + 主题选择 | ✅ | `build-ghostty.sh` 从 `iterm2_themes` 依赖派生 607 套配色 → `Contents/Resources/ghostty/themes`；CGhostty 推导 `GHOSTTY_RESOURCES_DIR`（上游入口，未设置时兜底）；出包脚本的**包内**配置检查验随包主题被引擎接受（变异 A6 有牙） |
| 终端配置下发（GUI → 引擎） | ✅ | `gt_ghostty_app_new_with_config` / `_app_reload_config` / `_config_get_{float,double,bool}`：设置页写 Ghostty 语法的配置文件，libghostty 自己解析；**判有效看诊断条数**（未知键只记诊断不丢配置）；`font-size` 改完网格密度真的变（`TerminalConfigTests` 11 项，含读回入口的键长度/类型）；数值键的 C 类型分 f32/f64（用错只得到 0）；变异 S1~S4 有牙 |
| 嵌入者自有字节流（host IO 后端） | ✅ | `patches/libghostty/8159838`：`ghostty_surface_process_output` + host 回调；`gate-ghostty-patch.sh` 对拍 4 个导出符号 + `mutation-check-ghostty-patch.sh` 证明有牙 |
| OSC 7501 程序状态（Program Status Protocol） | 🟡 | pin `8159838` 带解析层；**app 路径由补丁 0002 接**（`host_program_status` 回调 + 支持查询回包），端侧 `ProgramStatusReport`/`ProgramStatusStore`/标签条+侧栏角标。**在哪档证据下成立**：无头测到「报告→回调→记录」（`TerminalSurfaceTests` 4 项 + `ProgramStatusStoreTests`/`ProgramStatusBadgeTests`/`ProgramStatusPaneTests`），**UI 角标已取证到像素**：`ProgramStatusBadgeRenderTests` 把侧栏装进 NSHostingView、进程内 `cacheDisplay` 抓帧，断言 blocked 画出橙色像素、error 画出红色像素，并各配**对照**（同布局无程序状态 → 这些颜色为 0；判据靠 `g<180` 把「连接中」的黄色点排除掉，否则对照会假橙）。变异 `program-status` M1~M4 有牙（M4 直接打像素断言）。记录生命周期**两半齐了**：提示符开始（OSC 133）清 working/blocked，程序退出（会话状态）同样清 |
| OSC 133 shell 集成（semantic prompt） | 🟡 | **app 路径由补丁 0003 透出**（上游只转 `C`/`D` 两个事件，`prompt_start` 等没透）：`host_semantic_prompt` 回调带 kind/prompt_kind/exit_code/命令行，序数与上游 vt `GhosttySemanticPromptKind` 一致。**在哪档证据下成立**：`TerminalSurfaceTests` 2 项（四步循环 + 非 0 退出码）走真解析器；**已消费**：`prompt_start`（清 working/blocked）、`output_start`/`command_end`（侧栏显示**上一条命令**的命令行/耗时/退出码，标签胶囊对非 0 退出码画红点）；`input_start` 仍未消费。三态不折叠：没报退出码时不画失败记号、不编耗时。变异 `program-status` M3/M5 证明 kind 判别与失败记号有牙 |

## 终端引擎

| 平台 | 引擎 | 状态 | 证据 |
|---|---|---|---|
| macOS | full libghostty（Metal） | ✅ | **焦点下发**（分屏只有当前窗格的光标才闪）：宿主把焦点报给 libghostty（`ghostty_surface_set_focus` / `ghostty_app_set_focus`），口径 = SwiftUI 当前窗格 **且** 窗口 isKeyWindow；非聚焦面由引擎画成空心块并停闪。`WindowFocusTests`（DEC 1004 焦点上报的 `CSI I`/`CSI O` 字节，带可驱动的 isKeyWindow）+ 引擎侧 `TerminalSurfaceTests.testFocusIsReportedToProgramThroughHostWrite`；变异 `focus` F1~F3 有牙。**非聚焦窗格变暗**：盖 `unfocused-split-fill`（未设回落 `background`）层，alpha = 1 - `unfocused-split-opacity`；`SplitDimTests`（规则穷举 + 端到端抓帧），变异 `split-dim` D1~D3 有牙。**后台标签页停画**：surface 保持挂载但不可见时报 occlusion，引擎停掉渲染线程（窗口遮挡状态变化会重新同步）；`WindowFocusTests` 可见性两条 + 引擎侧 `TerminalSurfaceTests`（DEC 2033 的 `CSI ?999;1n / 2n`），变异 `occlusion` O1~O3 有牙。三层证据：字节→网格（`terminal-gate` 6 项，读 `ghostty_surface_read_text`）；渲染层已挂（`RenderProbeTests` 断言 layer-hosting + bounds/scale）；**像素级**（`PixelRenderTests` 抓自进程内视图帧：「红底真出现在屏幕像素上」+ 对照「换绿底红像素消失」+ 主题背景主色非纯黑 + 白字前景被绘制；变异 P1~P3 有牙）。仍不声称字形形状/抗锯齿细节。**键盘输入（IME）**：`TerminalHostView` 实现 `NSTextInputClient` + `keyDown` 走 `interpretKeyEvents`——中文/日文/韩文输入法可唤起（候选窗 + 组合内联）；组合串经 `ghostty_surface_preedit` 内联显示、**提交**走**键入**语义（`ghostty_surface_committed_text`，非粘贴）、候选窗锚点用 `ghostty_surface_ime_point`。`TerminalIMETests` 8 条（`is NSTextInputClient`+`inputContext`/`ime_point` 宽度/提交字节不带 bracketed paste/keyDown 经输入链路/特殊键编码不变（含 Ctrl+B）/**输入法提交的控制字符原样到达**（`\u{0002}` → 裸 `0x02`，tmux 前缀不被吞；对照上游 ghostty/ghostty discussion #14451——该缺陷在上游 macOS app 层的「keyDown 期间键盘布局 ID 变了 ⇒ 整键丢弃」guard 里，Ghosterm 不编译那一层、自研 keyDown 无此判据，故**不需要 vendor 补丁**）+ 两条对照），变异 `terminal` T5~T8 有牙 |
| iOS | full libghostty（Metal） | ❌ |
| Linux | full libghostty（OpenGL 4.3） | ❌ |
| Web | libghostty-vt + 自绘（wasm） | ❌ |
| Android | libghostty-vt + 自绘 | ❌ |
| HarmonyOS | libghostty-vt + ArkUI Canvas | ❌ |
| Windows | 待定 | ❌ |

## 环境包（runtime-payload）

| 端 | payload | 状态 |
|---|---|---|
| Android | Termux bootstrap + proot（按需下载） | ❌ |
| iOS | iSH（随 IPA 打包，延后） | ❌ |

## 构建与交付（CI）

| 交付物 | 状态 | 证据 / 口径 |
|---|---|---|
| macOS **dev** 包 | 🟡 | 每次 push `main` 出包：`.github/workflows/dev-build.yml` → ad-hoc 签名的 `.dmg`/`.zip` + SHA256SUMS + BUILD-INFO，挂到滚动 tag `dev` 的 prerelease。**仅 arm64**（`GhosttyKit.xcframework` 只有 arm64 切片）、**未公证**（首次打开是否被拦取决于取包工具贴不贴 `com.apple.quarantine`：浏览器与 `brew --cask` 都贴 → 要放行一次；`curl` + `ditto` 不贴标记 → 不触发 Gatekeeper 评估。实测口径与证据档（哪些量到、哪些是推论）见 [dev 包怎么装](../how-to/dev-build.md)）。build 号 = `GITHUB_RUN_NUMBER`，dev/release 两轨不同源单调。不能当自动更新通道（每轮 cdhash 不同）。 |
| macOS dev 包 **Homebrew cask** | 🟡 | tap 在独立公开仓库 [`hanxi/homebrew-ghosterm`](https://github.com/hanxi/homebrew-ghosterm)（`Casks/ghosterm-dev.rb`），指向 `dev` 滚动直链。**🟡 成立的证据档**：`brew tap` + `brew trust` + `brew install --cask ghosterm-dev` 在本机装成、`brew style`/`brew audit --cask --strict` 绿、`brew info --cask` 读出 `depends_on arch: :arm64` 与 `depends_on macos: :tahoe`（26.0 下限由内嵌 Homebrew `libcrypto` 抬上去）。**不解决 Gatekeeper**（Homebrew 自己贴 quarantine，见上一行），**`sha256 :no_check`＝故意不校验字节**（滚动资产钉不住，验字节请对 `SHA256SUMS`）；`:latest` cask 默认不进 `brew outdated`，升级要 `--greedy`。**未实测**：Intel 机器与 macOS < 26 的报错路径没有实机可验，只验到声明被正确读出。cask 的 URL 与本轮资产名由 `scripts/check-dev-cask.sh` 在 dev 轨对拍，牙见 `scripts/mutation-check-package-mac.sh` 的 A10 |
| macOS **release** 包 | 🟡 | tag `v*` / PR / 手动 → `.github/workflows/macos-release.yml`，与 dev 共用同一份可复用工作流 `macos-package.yml`（`channel=release、gates=full`）。**🟡 无 Developer ID 证书 secret ⇒ 目前同样落 ad-hoc**；`--sign` / `--notarize` 分支已就绪，拿到 secret 即用 |
| dev 版**更新检测**（app 内入口） | ✅ | 关于窗口（`Ghosterm → 关于 Ghosterm`）版本号下面那一行：手动点「检查更新」→ 取固定 URL 的 `version.json`（不走 API、不占 60 次/小时额度）→ 以 `git_commit` 为主锚（本机短 sha 前缀比清单的 40 位）。**三态不折叠**：无 commit 记录 / `+dirty` / 渠道不对 / 取不到 / 读不懂，一律报「查不出来 + 原因」，不报「已是最新」。**自动安装 ❌（有意不做）**：`signature=adhoc` + `notarized=false` ⇒ 只给下载直链，不画「安装」按钮。判据与文案由 `UpdateCheckTests`（12 项）钉，入口几何由 `AboutWindowTests` 钉，牙见 `scripts/mutation-check-app-update.sh`（7 组变异）；口径见 [dev 包怎么装](../how-to/dev-build.md) |
| Android / iOS 出包入口 | ❌ | 没有构建脚本、没有 workflow——是**未开工**，不是 CI 漏项 |
| 闸门分档 | ✅ | 轻档（dev）：`scripts/gate.sh`（core + ctest + ABI + api_smoke）+ 图标一致性 + `gate-ghostty-patch.sh`；全档（release）：再加 Apple 原生三包闸门与 18 份变异（终端/传输/标签/界面/设置/像素/焦点/变暗/遮挡/更新、普通默认入口、认证 ABI、凭据、真实认证、tmux 管理与 UI、打包）。dev/full 都另以本轮 ZIP 验一键安装脚本（签名/SHA/递归 quarantine/回滚），不触碰真实应用目录。真实 Linux SSH/断网仍须手工验证。缺工具链时显式 `SKIP` 并让 CI 断言不许 SKIP，不静默跳过（铁律 #6） |

## 各端 UI

| 端 | UI | SSH | SFTP | 本地 shell | ssh_config GUI |
|---|---|---|---|---|---|
| macOS | 🟡 | ✅ | ✅ | ✅ | ✅ |

> macOS 打包口径：`.app`/`.dmg` 由 `scripts/package-mac.sh` 产出，**自包含**已实测
> （递归内嵌依赖闭包 + 运行时断言不从 Homebrew 加载）；默认 ad-hoc 签名。
> **🟡 Developer ID 签名 + 公证未做**（无证书；脚本/CI 已就绪）。
> **🟡 最低系统版本**受内嵌 Homebrew OpenSSL 抬高（minos 26.0），需要自建依赖才能降。
> 签名档位按**证书真实的类**记账（`adhoc` / `dev` / `id` / `none`），身份串不在这两类
> 当场拒——**免费个人账号的 Apple Development 能签但不能公证**，与 ad-hoc 一样被
> Gatekeeper 拦（只多买到「签名身份稳定」），故它不构成第三条分发路；口径见
> [dev 包怎么装](../how-to/dev-build.md#签名档位adhoc--dev--id--none)。

> macOS 一行的口径：
> - SSH ✅ —— `apple-app-gate.sh` 起**真 sshd** 跑通「认证 → shell 通道 → 回显进
>   libghostty 网格 → resize 后远端 `stty size` 与网格一致」；主机密钥走真三态
>   对话框（M2），私钥口令走 Keychain + 超时闸（无签名环境不卡连接线程）；
> - 本地 shell ✅ —— 同上（`GT_TRANSPORT_LOCAL_PTY` 分支），且宿主可执行
>   `GhostermApp --local` / `--ssh-smoke` 直接冒烟（后者覆盖 AppModel 全链路）；
> - SFTP ✅ —— core `gt_sftp_*`（list / stat / lstat / readlink / realpath /
>   mkdir / rmdir / remove / remove_all / rename / chmod / walk / upload /
>   download / cancel / pause）+ SFTPBrowser UI（旧单栏浏览）**与新的内嵌文件
>   面板**（`FilesPanelView`：终端标签页右侧、⌘⇧E、前进后退/上级/刷新/过滤/
>   隐藏文件/⌘F 搜索/右键菜单）；e2e 走真 sshd 的 sftp-server，含失败 job
>   三态断言；变异验证 `sftp` 10 项 + `exec` 4 项 + `files` 4 项。
>   **🟡 文件面板口径**：本地 shell 标签页后端=LocalFileBackend（初始目录跟随
>   终端 cwd，且默认**持续跟随**——终端 `cd` 面板就跟着走）；SSH 标签页懒开
>   第二条会话。**独立 SFTP 标签页=左右双栏**
>   （`DualPaneFilesView`，栏间拖放即传）；**全局传输队列** `TransferManager`
>   （进度/速度/ETA/暂停/继续/取消/重试 + 冲突询问；`transfers` 变异 4 项钉住）。
>   **拖放 + Quick Look ✅**：栏间拖放即传（`DropRouter`）、本地条目拖出到
>   Finder、Finder 拖入远程栏=上传；空格/右键 Quick Look（远端下临时副本）。
>   **树/列表可切 ✅**（懒展开的目录树）；**面板键盘** ✅（⌘F/⇧⌘N/⌘R/⌘I/⌫/空格，
>   只挂在文件列表上，macOS 14+；13 退化到右键菜单）。
>   **顶栏折行 + 关闭按钮 ✅**（2026-10-15，用户实测带出）：工具条原是一行
>   `HStack`，面板窄到 260pt 时右边按钮被画到框外（「顶部按钮超框了」）——改走
>   `FlowLayout` 折行（分行规则是纯函数 `FlowLayout.rows`，可离线断言；单项比
>   整行宽时独占一行，不裁不缩）；顶栏第一行**最右**补 ✕ 关闭按钮（此前只能
>   ⌘⇧E / 「显示」菜单开合），双栏 SFTP 标签页的每栏不传 `onClose`、故无此钮。
>   断言 `PanelChromeTests` 6 条（折行规则 3 + 像素对照 3：窄面板顶栏必须更高、
>   右沿不许有控件被裁、给了 `onClose` 才画得出 ✕），变异 `files` F10~F12 有牙。
>   **仍未做**：iOS 抽屉（无 iOS target）、Finder 拖出**远端**条目的懒下载
>   文件承诺（v1 只支持本地拖出）；
> - UI ✅ —— 窗口 + 侧边栏（标签页/连接两栏）+ 顶部标签条（胶囊）
>   + 顶带按钮组（红绿灯 → 侧栏开关 → `+` → 配置无效时的 ⚠，侧栏开合两态
>   同一个 x，`BandControls` 一份两处用；设置与窗口级动作只在 macOS 全局
>   菜单条里，由 `AppMenuCoverageTests` + 变异 `band-controls` 钉住）
>   + 多标签页（后台标签页 surface 保持挂载）+ 递归分屏（⌘D / ⌘⇧D；窗格
>   退出即收掉、只剩一个则关标签页 + 结构改动后分隔等分，见 `SplitExitTests` /
>   `tabs` 变异 T15/T16）
>   + **标签右键菜单**（`TabContextMenu`，侧栏标签行 / 顶栏当前标签胶囊 /
>   标签条 chip **共用一份**：文件面板开关 + 向右/向下分屏 + 关当前分屏 +
>   锁定 + 关闭；顶栏原本可见的两个分屏按钮移入菜单，右端那个 ⋯ 溢出菜单已删——
>   里面每条都是副本。菜单作用在**被右键的
>   那条**标签上并把它带到前台——右键的未必是当前标签页，写错就「右键 A 却
>   动了 B」，不报错、只在屏幕上不对；`TabContextMenuTests` 4 条 +
>   变异 `tabs` T17/T18 钉住）
>   + 标签页锁定 + 状态覆盖层（连接中/失败/已退出四档显式）+ 设置页（通用/
>   会话/外观/高级）+ 编辑器 + SFTP 浏览器 + 三态对话框 + 传输面板；
>   **遗留（显式）**：标签页恢复/持久化、拖拽重排手势；
> - ssh_config GUI ✅ —— HostEditorSheet（含 `Host *` 通配块编辑，M1 遗留
>   落点）+ 写侧三件套（备份 / 冲突检测 / 原子写，ConfigStoreTests 6 项）。
| Android | ❌ | ❌ | ❌ | ❌ | ❌ |
| iOS | ❌ | ❌ | ❌ | ❌ | ❌ |
| HarmonyOS / Windows / Linux / Web | ❌ | ❌ | ❌ | ❌ | ❌ |
