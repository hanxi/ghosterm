# AGENTS.md — Ghosterm 开发铁律

> 本文件是**验收闸门的唯一事实来源**。与代码冲突时以本文件为准，改规则须走 PR 并说明理由。

## 项目一句话

Ghosterm = **libghostty 驱动的多端终端** × **VS Code 式 `~/.ssh/config` 图形管理** × **WinSCP 式 SFTP 文件管理器**。

技术栈：**各端原生 UI + 共享 C 核心（libghostty / libssh2）+ Transport 接缝**。
**不用跨端 UI 框架，不用 Rust。**

## 目录边界（禁止越界）

| 目录 | 内容 | 依赖规则 |
|---|---|---|
| `core/` | C 共享库：ssh / sftp / ssh_config / known_hosts / forward / pty | **纯 C11 + POSIX**，禁止依赖任何端代码、禁止依赖 UI |
| `terminal/` | 各端 TerminalSurface（full libghostty 或 libghostty-vt+自绘） | 只依赖 `core/include` 与 libghostty |
| `apple/ android/`（现有）· `windows/ linux/ harmony/ web/`（规划） | 各端宿主 + UI | 通过 `core/include/ghosterm.h` 消费核心；**禁止**直接引用其他端 |
| `vendor/` | 上游 submodule（ghostty、ish-arm64），**锁 pin** | 不允许就地改；改动一律走 `patches/` |
| `runtime-payload/` | 环境包（Android Termux/proot、iOS iSH） | 只放清单与下载/校验逻辑，二进制不入库 |
| `patches/` | 上游补丁 | 每个补丁必须带 `WHY.md` |
| `scripts/` | 构建/闸门脚本 | — |
| `docs/` | Diátaxis 文档 | — |
| `assets/` | 品牌资源：应用图标**母版 SVG** + 各端派生产物 | 无代码依赖；产物一律由 `scripts/build-icons.sh` 从母版派生，禁手改产物 |

## 铁律

1. **三层接缝**：`UI → TerminalSurface → Transport → Core`。UI **禁止**直接调 Core；TerminalSurface **禁止**知道背后是 SSH 还是本地 shell。违反＝架构错误，不是风格问题。
2. **字节流不过桥**：终端输出走 `transport → surface` 直连，**不进事件队列、不过 UI 线程**。事件队列只承载低频事件（状态/进度/退出）。高频过桥是性能红线。
3. **C ABI 是唯一跨语言边界**：所有平台绑定只经 `core/include/ghosterm.h`。句柄一律 opaque；跨语言只传 POD 与 NUL 结尾字符串。**改 `ghosterm.h` 必须同步各端绑定并跑 `scripts/gate.sh`。**
4. **`ssh_config` 文件即真相**：解析→编辑→写回必须**逐字节无损往返**（保留注释/未知指令/块顺序/缩进）。写回前强制备份。**「无损」分两档验收**：未编辑往返＝逐字节相同；编辑后往返＝「除目标行外必须逐字保持不变」（M1 补闸门）。语义以真 `ssh -G` 对拍为准，锚点是 `core/tests/fixtures/golden.{conf,effective.tsv,json}`（由 `core/tests/scripts/regen-golden.sh` 同源再生，**勿手改**）。**再生必须在钉版本环境执行、且环境干净**（`ssh -G` 也读系统级 `/etc/ssh/ssh_config`）：当前锚 `OpenSSH_10.3p1`；换环境再生＝换基线，须标 `BREAKING`。**默认 IdentityFile 候选链是版本相关行为，不是常量**（9.6p1=7 条含 xmss/dsa、10.3p1=5 条、master=6 条含抗量子混合），故实现为 `gt_identity_policy_t` 策略，内核提供 9_6/10_3/MASTER/NONE/CUSTOM 五档，见 core/tests/README.md。**锚点不得带再生机器的身份**：环境相关的默认值（回落登录名、`~` 展开出的路径）由 `regen-golden.sh` 映射成中性 token，脚本落盘后跑泄漏守卫，搜到真实家目录/登录名即失败。
5. **密钥红线**：私钥只进平台安全存储（Keychain/Keystore/HUKS）。核心只接受**已解锁**的材料，**永不**落盘明文、不进日志、不进测试快照。
6. **三态语义不得折叠**：`未知 ≠ 拒绝`（hostkey / job 结果 / 能力探测）。**能力缺失必须显式报错**（`GT_ERR_UNSUPPORTED`），禁止静默降级、禁止假装成功。
7. **终端引擎按平台分档，不得混用**（见下表）。full libghostty 在 Android/HarmonyOS **没有渲染后端**；Web 端上游 wasm 产物只有 libghostty-vt（`renderer/WebGL.zig` 在 pin 版本是空 stub）——这三端**当前**必须走 libghostty-vt + 自绘。Web 的长期目标仍是 full libghostty（WebGL, wasm），等上游后端落地再评估切换。
8. **平台差异显式化**：能力矩阵维护在 `docs/reference/capability-matrix.md`，状态口径 ✅/🟡/⏳/❌ 必须诚实，🟡 必须写明「在哪档证据下成立」。
9. **上游补丁入库**：对 libghostty / libssh2 / iSH 的任何本地修改放 `patches/<upstream>/` 并附 `WHY.md`，随代码提交。
10. **环境包按需下载**：运行时环境（Termux bootstrap / iSH rootfs）**不进 App 包**；下载须校验 sha256、原子激活、可回滚、可删除。
11. **许可**：仓库整体 **MIT License**（`LICENSE`）。随包组件按各自许可声明；iSH / Termux 这类 GPL 组件若日后随二进制分发，仍须履行**其自身的** source offer；安排见 `docs/explanation/licensing.md`。

## 终端引擎分档（铁律 #7 的执行体）

| 平台 | 引擎 | 渲染 | 依据 |
|---|---|---|---|
| macOS | **full libghostty** | Metal | Ghostty 本体 / [Termini](https://github.com/hudsonkit/Termini) / [iGhostVT](https://github.com/owngoal-dev/iGhostVT) |
| iOS | **full libghostty** | Metal | [Termini](https://github.com/hudsonkit/Termini) / [iGhostVT](https://github.com/owngoal-dev/iGhostVT) |
| Linux | **full libghostty** | OpenGL 4.3 core | Ghostty GTK |
| Web | **libghostty-vt + 自绘**（目标 full libghostty，待上游 WebGL 后端） | Canvas/WebGL（wasm） | [ghostty-web](https://github.com/coder/ghostty-web) 即此路线；上游 wasm 只产 libghostty-vt，`renderer/WebGL.zig` 为空 stub |
| Android | **libghostty-vt + 自绘** | GLES/Canvas | 无 GL4.3 core 后端 |
| HarmonyOS | **libghostty-vt + 自绘** | ArkUI Canvas | [libghostty-ohos](https://github.com/wiedymi/libghostty-ohos) |
| Windows | **待定** | — | OpenGL on Windows 需 WGL/ANGLE，先验证 |

> 两档挂在**同一个 transport 接缝**后面，故 UI 与业务代码零分叉。

## 临时文件与执行（环境硬约束）

- **临时文件一律放工程根目录下的 `tmp/`**（已 git 忽略）；**不写、不依赖系统 `/tmp`**。
- **禁止在系统 `/tmp` 下执行**任何二进制 / 脚本 / 探针——本环境不允许，从 `/tmp` 执行的进程会被判死（SIGKILL，表现为 `exit 137`）。临时探针、一次性脚本、二进制都在 `tmp/` 里生成并在 `tmp/` 下执行。
- `scripts/` 与闸门产生的临时文件同样落在 `tmp/`（构建产物 `core/build/` 已忽略，不在此列）。
- `tmp/` 内容不入库；用完自行清理：`rm -rf tmp/*`（保留目录本身）。

## Shell 写法（跨 runner 的硬约束）

- **`$var` 紧跟非 ASCII 字符（中文标点、全角括号、破折号）时必须写 `${var}`**：某些
  bash×locale 组合会把多字节的首字节**吃进变量名**，`set -u` 下当场报「unbound
  variable」——于是**该打日志说明失败原因的那一行，反而把脚本打死**。CI 实测两例：
  打包变异 A7 的 `源 $A7_ROOT）` 报 `A7_ROOT?: unbound variable`（其余 15 项全绿却整步
  判红），本地 `LC_ALL=en_US.UTF-8 /bin/bash` 可稳定复现同样的旧写法红、`${A7_ROOT}）`
  绿。仓库文案与注释里中文很多，这条不是风格而是验收口径；自查用正则
  `\$\{?[A-Za-z_][A-Za-z0-9_]*\}?[^\x00-\x7F]`（对 `git ls-files '*.sh'` 全量跑，
  命中且未带花括号的就是雷）。

## 验收闸门（合并前必须全绿）

```bash
bash scripts/gate.sh        # 离线闸门：C 核心构建 + ssh_config 对拍 + ABI 冒烟
bash scripts/build-core.sh  # 只构建核心（快速）
```

闸门内容（`scripts/gate.sh`）：

1. `core` 以 `-Wall -Wextra -Werror` 构建通过；
2. `ctest` 全绿，其中 **`sshconfig_golden` 必须做全表语义对拍（42 键 × 6 host）**，并**双向校验支持集**（C 表 ↔ 夹具元数据，防漂移）；
3. `ghosterm.h` 导出符号集合无漂移（符号表对拍，见下）；
4. 未实现入口**必须返回错误**（`api_smoke` 断言，防「静默成功」）。

`public_headers` 随 `ctest` 验证全部公开头可独立以 C11 编译、依赖无环、
子头不反向包含聚合头 `ghosterm.h`。平台绑定仍只经聚合头进入；
共享错误类型位于 `ghosterm/types.h`，不在各绑定复制。

**改了 `apple/ android/ windows/ linux/ harmony/ web/` 必须另跑对应平台构建**；JS/宿主闸门不覆盖原生编译。

**CI 双轨（每次提交自动出包）**：push `main` → `dev-build.yml`（轻闸门 + ad-hoc macOS 包 +
滚动 `dev` prerelease）；tag `v*` / PR / 手动 → `macos-release.yml`（全量闸门 + 签名/公证分支）。
两条轨共用 `.github/workflows/macos-package.yml`；包内必须内嵌的 Linux 会话安装包由
`scripts/build-session-vt-linux.py` 在 `ubuntu:22.04` 容器里双架构现造。装机口径见 `docs/how-to/dev-build.md`。
dev Release 另挂一份机读清单 `version.json`（固定 URL，不走 API）：**新旧以 `git_commit` 为主锚**
——`build` 号按 workflow 文件计数，跨轨不可比；清单由随包 `BUILD-INFO.txt`/`SHA256SUMS` 解析而来
（不是第二份真相），解析不到即当场红，「复验」step 再从固定 URL 取回对答案。它只解决**检测**，
`signature=adhoc` + `notarized=false` 就是「不许自动安装」的机器可读声明。

> **CI 覆盖范围（诚实标注）**：`gates=full` 只自动跑这 **11 份**变异验证——
> Apple 端 `terminal / transport / tabs / chrome / settings / pixel / focus / split-dim / occlusion`、
> Linux builder 的 `sessiond-linux`、以及 `package-mac`（在**干净检出**上验打包链，输出里
> 出现 `SKIP` 一律判红，缺随包主题不许蒙过去）。**其余闸门仍是「手工跑」**：`core/` 侧的
> sshconfig / include / ssh / sftp / exec / files / transfers / dragdrop / pty-inheritance /
> attachment-peer / auth / login-shell / known-hosts / public-headers 等 CI 看不见。
> 所以「CI 绿」≠「闸门全绿」——改到上面那些面，合并前仍要本地 `bash scripts/gate.sh` +
> 对应变异脚本各自跑绿。

### C ABI 符号表（改 `core/include/ghosterm.h` 或 `core/src/*` 时另加）

`ghosterm.h` 是**单一事实来源**。新增/删除/改签名后必须：

1. 同步实现与各端绑定；
2. 跑 `scripts/check-abi.sh`：`nm` 静态库实际导出名 ↔ 头文件声明，**双向对拍**（漏导出与多泄漏都拦）。

> **当前覆盖范围（诚实标注）**：只对拍「`core/` 内部实现 ↔ 头文件」。**各端绑定（Swift 模块表 / JNI / NAPI）尚未存在，故未纳入**。
> M1 起每加一端绑定，就要把该端的「绑定引用名集合」并入同一份对拍，否则 `ghosterm.h` 改了而某端没改，闸门看不见。

### 变异验证（新增闸门必须附）

新增任何断言/闸门，都要证明它**有牙**：故意改坏 → 断言变红 → 回滚复绿。
样板见 `scripts/mutation-check-sshconfig.sh`（当前 14 项：基线 + A1~A6 + B1~B6 + 回滚）。
**现有 33 份**（每份都必须能独立跑绿）：sshconfig / include / settings /
settings-ui / tabs / chrome / sidebar-chrome / band-controls / pane-chrome /
auth / login-shell / transport / ssh / sftp / exec / files / transfers /
dragdrop / terminal / pixel / focus / split-dim / occlusion / known-hosts /
package-mac / icons / ghostty-patch / program-status / split-divider / public-headers / pty-inheritance / attachment-peer / sessiond-linux。
（`sessiond-linux` 用 `python3 scripts/mutation-check-sessiond-linux.py` 执行，
护「mac 与 Linux 两侧 session contract 逐位一致」这条链条：少打一片**真改到头树**的
补丁 / vendor pin 被改 / 把 `.a` 冒充 `--engine` / 改一个公开头的一字节——四组变异都要
让对应断言变红，且每组带**指向性对照**（静态版的 `DT_NEEDED` 仍是基线的子集，证明红的
是新加的下界断言而不是旧的上界白名单；没动 `core/` 时 `public-headers` 仍绿）。
脚本只 import `scripts/build-session-vt-linux.py` 复用同一份 staging/打包实现，不复制逻辑；
变异落在 `tmp/` 的副本上，回滚逐字节比 sha。**需要 Linux builder**：非 Linux 主机显式
打印 `SKIP` 并 `exit 0`，CI 的 `linux-bundle` job（`gates=full`）断言输出**不含** `SKIP`。
注意契约的真实边界：契约只哈希「头树 + 补丁文件本身 + schema 源文件」，所以「少打一片
只改 `.zig` 的补丁」契约**看不见**——别拿那种变异冒充有牙。）
（`public-headers` 用 `python3 scripts/mutation-check-public-headers.py` 执行，
护子头→聚合头、隐藏聚合类型依赖与子头环路；变异目标红、独立 config 头对照绿。
会话状态 P0 的 `scripts/probe-session-state.py --mutations` 是另一个原型证据，
尚不作为完整恢复闸门，资源探针返回 3 表示 P0 未完成。）
（`attachment-peer` 用 `python3 scripts/mutation-check-attachment-peer.py` 执行，
护 Unix stream 连接死亡释放本代、旧 owner 不碰新代、未接受字节在 HUP 后丢弃、
短写/背压不谎报、信号不重置超时与 socket 的 CLOEXEC/NONBLOCK。
十三类基线、十五组隔离变异及 SHA256 回滚；SIGKILL 用独立空闲客户端进程，
真实 PTY 在断连后继续输入/输出，尚不替代 full surface 的跨进程 GUI 恢复。）
（`pty-inheritance` 用 `python3 scripts/mutation-check-pty-inheritance.py` 执行，
护 master/唤醒管道的 CLOEXEC 以及 exec 前隔离调用方描述符：后开的 shell 若继承
前一条会话的 master，父进程关 master 也无法触发 hangup，close 的 waiter join
会卡死。回归用真 exec 子进程检查描述符，且第二个 PTY 活着时第一个必须秒关；
两个变异在独立 core 副本中分别破坏 CLOEXEC 与子进程 close，API 对照仍绿。）
（`sftp` 护文件操作原语（stat 尺寸 / 列目录名 / 下载内容 / job 三态 /
`remove_all` 递归 / `chmod` 模式位 / `walk` 深度 / `pause`）；`exec` 护远端
命令通道（stdout-stderr 分离 / 退出码）——两者都跑真 sshd 的 `ssh_e2e`。
`files` 护文件浏览器后端与面板模型（非递归删非空必须失败 / walk 深度 /
list 不过滤隐藏 / 目录优先排序 / 双栏接线）**与面板顶栏那两条用户实测
反馈**（F10~F12：顶栏工具条排不下必须折行——退回一行 `HStack` 就是用户报的
「顶部按钮超框了」，按钮被画到框外、不报错；给了 `onClose` 就得画得出 ✕
关闭按钮——没有它面板只能靠 ⌘⇧E 开合。后两条的判据是**像素**；F13 护**条目
右键菜单上的快捷键**：菜单标的键必须倒推回 `PanelKeymap`——标错就等于菜单
承诺了一个按不动的键，比不标更糟；只在 macOS 14+ 标，13 上没人接键），
`transfers` 护全局传输队列
（进度总量 / 失败不折叠 / 冲突 skip / 暂停路由），`dragdrop` 护拖放路由
（远端→本地=下载的方向 / 转义往返 / 完成回调），三者跑 `swift test`。）
（`band-controls` 护「顶带那一组按钮 + 菜单栏是唯一退路」——侧栏底部的 ⚙︎ 与
标签条右端的 ⋯ 都删了（macOS 顶上有全局菜单条，窗口里再挂一份是重复），于是
三件事会**悄悄**坏掉：按钮顺序换了（B1，像素断言看不出组内顺序，必须靠几何
断言）、终端配置坏了没人吭声（B2/B3，铁律 #6）、菜单栏少一项就等于功能消失
（B4~B6）。B5 的指向性特意证明「传输」没有快捷键、旧的 ShortcutCatalog 对拍
看不见它——新 gate 堵的是真缺口。）
（`focus` 护「分屏只有当前窗格的光标才闪」——焦点从 SwiftUI 下发到 libghostty
的 surface；漏下发时**屏幕上不报错**，每个格子都停在引擎「默认已聚焦」上
一起闪。断言用 DEC 1004 焦点上报的 `CSI I`/`CSI O` 字节做观测，变异 F1~F3 有牙。
F4/F5 另护**键盘落点**（AppKit firstResponder）：它与「报给引擎的焦点位」是
两件事，只做后者就是用户实测的「开终端 / 分屏后光标在新格子闪、敲字却进了上
一个终端」——后台标签页的 surface 保持挂载，没人把 firstResponder 撤下来。
新窗格是「先置当前、后进视图树」，故 `paneFocused` 翻转与进树时都要要一次键盘。）
（`split-dim` 护「非聚焦窗格变暗」——盖的是 `unfocused-split-fill`（未设回落
`background`）、alpha = 1 - `unfocused-split-opacity`（上游口径，≥1 即关掉）；
纯规则穷举 + 端到端抓像素，变异 D1~D3 有牙。）
（`occlusion` 护「后台标签页停画」——surface 保持挂载但不显示时，把可见性报给
引擎让它停掉渲染线程（窗口 isKeyWindow/遮挡状态一变就重新同步）；断言用 DEC 2033
可见性上报的 `CSI ?999;1n / ;2n` 做观测，变异 O1~O3 有牙。）
（`transport` 护「本地 PTY 的子进程真的拿到控制终端」与「前台作业判定
（空闲=IDLE / 有作业=JOB / 查不出=UNKNOWN 不折叠）」——它连着那个潜伏 bug：
父进程抢跑 setpgid 会让子进程 setsid 憋成 EPERM、整个会话拿不到控制终端。）
（`transport` 还护「**写绝不无限等**」：pty 输入队列满（前台程序不读 stdin）
时写要在有界时间内返回、积压字节随后补送——旧实现的 `poll(...,-1)` 会把会话
线程钉死，读循环出不来（终端冻住）+ `stop()` 的 `queue.sync` 把主线程也钉死
＝整个 app 无响应。变异 P4 有牙：回到 `poll(-1)` 即整条套件卡到 timeout 变红。）
（`ssh` 护「**SSH shell 写也不无限等 + 不丢字节**」：与本地 PTY 同结构——
`gt__ssh_shell_write` 在 `GT_SSH_WRITE_GRACE_MS` 内尽量送，剩下的进 `wtail`
队列由后续 read/write 补送；timeout 由写路径自己设、自己还（`get_timeout`
保存、`set_timeout` 还原），不打扰共用同一 session 的 SFTP。读路径每轮也
flush 一次 wtail，当尾队列的泵。契约见 `transport.h`：「输入不丢，只是晚到」。
已知 e2e 限制：`sshd` 排空速率 ≈ 40 MiB/s（环回实测），3 MiB payload 写完时
队列**根本没形成**——flush 机制无法被真 sshd e2e 触发。test_ssh §5b 只断言
「写有界（elapsed<3s）+ 不丢字节（rc=GT_OK）」；队列机制本身由代码审阅 +
与 PTY `wtail` 的结构对称性保证（见 `core/src/io/pty_posix.c` 的 wtail + P4）。无独立变异
脚本——与 PTY P4 是同构的 wtail 实现，PTY 的 12/12 变异覆盖已验证同一套机制。）
（`terminal` 护「字节真的进了网格」（feed 被短路 → 网格断言红）、「浅/深色切换
下发到**已有** surface」——后者是用户实测「切深色终端没刷新、新开的才对」：
每个 surface 各自持有条件状态，app 级事件改不到它们，必须逐个下发 + 消费
`reload_config` 动作，断言用 DEC 2031 配色上报观测——以及「macOS 输入法真的接上
了终端」（用户实测「敲不出中文、输入法不弹」）：视图必须实现 `NSTextInputClient`
且 `keyDown` 走 `interpretKeyEvents`，组合串经 preedit 内联显示、提交走**键入**
语义（`TerminalIMETests`，观测口径是 libghostty 的 `ime_point` 宽度与 transport
字节）。变异 T1~T7 有牙。）
（`sidebar-chrome` 与 `auth` 是用户实测反馈带出来的两份：前者护「侧栏切换按钮
在它要收的那条边上、当前标签页高亮、顶栏标签条标签多了能横向滚（普通鼠标滚轮
也算）」，后者护「没口令的钥匙不被问口令」。
`tabs` 另护**标签右键菜单的落点**（T17/T18）：菜单里的分屏 / 文件面板开关
必须作用在**被右键的那条**标签上，不是「当前标签页」——右键的未必是当前的，
写错就「右键 A 却动了 B」，不报错、只在屏幕上不对。T20 再护**菜单上的快捷键
显示**：`.keyboardShortcut` 要真的落到 NSMenuItem 的 keyEquivalent 上（不然
菜单右侧什么都不画），且与 `ShortcutCatalog`（已与主菜单对拍）逐条一致。
`program-status` 护 OSC 7501/133 的五处**我们自己写的规则**：clear 的子树
删除、程序文本里不可见格式字符的剥离、OSC 133 的 kind 判别（只有
`prompt_start` 才清 working/blocked）、**角标的像素渲染**（M4 让摘要恒空 →
侧栏画不出橙色/红色）、以及**上一条命令的失败记号**（M5 把条件写死 →
标签胶囊画不出红点）——各自配一条同变异下仍绿的对照。
（`pane-chrome` 护「窗格外观 + 缩放 + 只有一个加号」这批纯外观接缝：P1 间距
偏好读了却没留白（padding 加在错的层，两格仍贴着）、P2 标题栏开关没接上、
P3 侧栏展开时顶部标签条不隐（同一份标签清单画两处）、P4 顶栏与侧栏同时画
`+`（用户实测：「现在出现 2 个加号了，我只要一个加号」）、P5 缩放时把被缩放
那格的位置塌掉——兄弟窗格跟着重排，终端那边就是一轮 SIGWINCH、回卷被揉烂
（所以实现是「盖一层」：原位留一张空卡片，`layout.viewID` 不变）、P6 关掉被
缩放那格后不清状态（覆盖层盖出一片空白）。P2 的判据是**像素**；P1 量的是真视图树里
两块终端面之间空出多少点——按颜色比会假绿（浅色外观下窗口底色与窗格底色都是
近白，变异验证当场抓到）；P4 数的是 SwiftUI `Menu` 落到 AppKit 的 popup 按钮
个数；P5 量的是兄弟 `TerminalHostView` 的宽度。）
`settings-ui` 护设置弹窗的数据面：配色两档的序列化（拼错整份配置就废）、
`/etc/shells` 的解析、**快捷键表与真菜单的双向对拍**、**下拉控件的定宽**
（U6：回到 `maxWidth` 就会随选中值伸缩，短值 93pt / 长值 190pt）、
**外观页下拉右边的配色色块真画得出来**（U7：色块塞回 `Menu` 的 label 就被
AppKit 收编吃掉——用户报的「主题预览没显示」；断言数的是色块背景色的像素）、
**改外观偏好要立刻落到 `NSApp.appearance`**（U8：删掉 `didSet` 里那半截，
界面就要重启才变——用户报的「切深/浅色界面不动」）、以及**设置窗口底色要
跟着外观走**（U9：底色回到 `viewDidLoad` 里 `.cgColor` 求死，动态色按
`NSAppearance.current`（系统外观）解，系统浅色 + 选深色会解成白底 + 浅字——
用户报的「设置界面看不清」）。）
手法要造出**目标失效状态**：

- 验「无损往返」必须改被插值的**期望值**，不能改两侧一起变的常量；
- 验「agent 不产生假绿」要用**双断言**（同一变异下闸门红 + 另一条仍绿，证明红有指向性）；
- 变异脚本必须：**起跑前扫变异标记**（命中即 `exit 2`）、`trap … INT TERM` 复原、回滚时比对**起跑前记录的 sha256**；
- 变异脚本要超时**只能走 `scripts/lib/gt-timeout.sh` 的 `gt_timeout`**，**禁止**裸用 `timeout`：
  macOS（含 CI 的 macos-15 runner）没有 GNU `timeout`，brew 装的是 `gtimeout`。
  「命令不存在」和「断言变红」都是非零退出码，于是**基线红、变异也红**，`expect red`
  整片假通过（CI 实测：`mutation-check-transport` 报「4 passed / 8 failed」，每项只花
  2–4 秒，而同一 job 里 ctest 是绿的）。`gt_timeout_require` 解析不到 `timeout`/`gtimeout`
  就 `exit 2` 拒绝起跑——**宁可红，不拿「命令不存在」冒充有牙**；也绝不降级成
  「不设超时」（P4 那类「无限等」的变异靠超时才有界，拿掉超时就是拿掉判据）。
- 变异脚本**取输入的那一步要和被测代码取同一个源**，并且先有一条「变异前就找得到」的
  前置断言：CI 的 runner 与开发机看到的文件系统不一样。实测两例——`vendor/ghostty/zig-pkg`
  是构建期产物、不入库，缓存命中时不跑 zig，于是写死 zig-pkg 的 A7「变异→红」是**因为目录不在**
  （红得没有指向性），「复原→应绿」也假红；缺 Pillow 的开发机上图标对照直接红。
  两条都靠「先断言源在，再谈变异」挡在前面。
- 判据反了（期望红实际绿 / 期望绿实际红）时必须**当场 dump 最后一次运行的输出**，
  不许 `>/dev/null 2>&1` 一丢了事——只剩一行结论的失败没有证据，归因只能靠猜（CI 上真猜过一轮）。
- **wall-clock 类判据要和「同一负载下的标定」比**，不能只钉一个绝对秒数：共享 runner 会把
  调度抖动整倍放大（`test_transport` 的 close-speed 实测连续三次 1.07/1.53/1.09 秒，同一
  commit 在空机器上 0.2 秒）。做法：被测那段等待是 `20×10ms` 轮询，就再量一遍一模一样的
  轮询当标定，预算取 `max(原来的绝对下限, 1.5×标定 + 余量)`——**放大对两侧同向**，真回归
  （把宽限写长）只有 close 一侧变慢照样红；绝对下限**不放宽**，否则历史那次 2.3 秒的回归
  就没人管了。
- **wall-clock 的下界判据，时间戳必须在被测动作「之前」取**，而且松弛量要显式钉住：
  `test_ssh_connector` 的 real-deadline 原先在 `open()` 返回**之后**才取 `started`，而被测的
  到期时刻记的是连接器**自己**那一刻——两者之间被 runner 挤掉 1ms，`elapsed` 就成 29，
  而 `>=30` 的下界零余量（CI 实测 0.04 秒内即红；本机空载与 8 路忙循环下都是 30/31，
  所以**不是忙循环问题，是取点问题**；注入 3ms 调度间隙即可复现 26/27）。取早了 `elapsed`
  只会变大，代价是下界白捡一段准备成本，于是那段成本要**实测出来并断言在预算一半以内**
  （`open_cost<budget/2`），否则「松弛自己长大到把回归整段吞掉」没人看得见。
  另一条同类坑：**标定必须在取点之前跑完**——把标定循环放在 `started` 之后，等于把标定
  自己的 43ms 算进 `elapsed`，上界从此不再量轮询。

## 提交纪律

- **直接提交到 `main` 分支**：不新建功能分支、不走 PR 流程（本仓库约定）。
- 每切片必提交；一个提交只做一件事；闸门绿才提交。
- 提交信息遵循 **Conventional Commits**：`type(scope): description`（feat/fix/docs/test/chore/refactor），description 与 body **尽量用中文**。
- 破坏性变更（`ghosterm.h` / fixture）在提交信息里显式标注 `BREAKING`。
- **禁止**在提交信息里添加任何署名类 trailer：`Co-Authored-By`、`Generated-with`、`Assisted-by` 一律不写（无论 AI 助手还是替他人代签），作者字段只填本人。发现历史里混入即清理（`git filter-branch --msg-filter` 剥掉该行）并 `--force-with-lease` 推回。
- 关联 GitHub issue 的提交信息**必须**带 issue 引用，引用规则：
  - 短写 `#123` 永远指向**当前 commit 所在仓库**的 issue；
  - 只要引用的不是当前仓库的 issue，就必须写完整 `owner/repo#123`。
