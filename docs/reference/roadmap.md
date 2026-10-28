# 路线图

2026-10-28 会话方向变更：已交付 [可选 tmux 重构](plan-optional-tmux.md)。默认连接回到普通
PTY/SSH，自有 sessiond 整体退役；旧私有恢复与随包 runtime 已退役，tmux 管理/UI 与本机、真实 SSH 验收已完成。
具体切片与验证进度见 [实施清单](optional-tmux-implementation.md)。

> 优先级：**macOS 做到功能完整 → Android 竖切 → 其余端**（7 端都要，只是排期）。
> 排期原则（2026-10-10 调整）：**所有核心能力（含 config 编辑 / SFTP）先在
> macOS 一端实现并验证到位**，Android 动工时核心已冻结、只做端侧。差异化功能
> （config GUI / SFTP）不再后置成独立里程碑，而是并入 mac 阶段。
> 每项完成时必须同步更新能力矩阵并附证据。
> 2026-10-28 会话专项调整：普通连接为默认，可选 tmux 保活已交付；共享 C 契约本轮有 BREAKING 调整，
> 不能把此前「核心已冻结、只做端侧」当作当前状态。

## M1 — macOS 竖切 ✅ 完成（2026-10-10）

目标：**在 macOS 上跑通「终端 + SSH + 主机列表 + 本地 shell」**，验证原生路线的手感。

- [x] `core`：POSIX `forkpty` transport（`gt_transport_open_pty`）
- [x] `core`：libssh2 后端（connect / auth / hostkey TOFU / ssh shell transport）
- [x] `core`：`ssh_config` 编辑 API（改值后仍无损写回）
- [x] `terminal/apple`：full libghostty surface（Metal）+ `patches/libghostty/` 的
      host IO 后端补丁（`ghostty_surface_process_output` / `host_write` / `host_resize`）
- [x] `apple/`：SwiftUI 宿主（`GhostermCore` / `GhostermTerminal` / `GhostermApp`）
      + `ghosterm.h` module map 绑定
- [x] 闸门：C ABI 对拍 + ssh_config 对拍 + 本机渲染烟雾
      （`gate.sh` / `terminal-gate.sh` / `apple-app-gate.sh` / `gate-ghostty-patch.sh`
      / `check-abi-apple.sh` + 四份变异验证）

### M1 遗留（不阻塞，已并入后续里程碑并在能力矩阵标注）

- **像素级渲染验证**：当时的结论是「抓像素在无屏幕录制权限时 SIGSEGV」，
  闸门只到「网格拿到字节 + Metal 层已挂」。（→ 2026-10-12 落地，
  见 M2 章节：那条结论**部分有误**，抓自进程内的视图不需要该权限）
- **主机密钥确认对话框**：M1 的 UI 还没有，故 `AppModel` 显式**按未知拒绝**
  （宁可连不上，也不静默 TOFU）。（→ M2）
- **私钥取自文件**：Keychain 是后续切片（铁律 #5）。（→ M2）
- **`Host *` 通配块的求值**：列表会跳过它（不是可点主机），但编辑器要能改它。（→ M3）

## M2 — macOS 功能完整（终端应用收尾） ✅ 大体完成（2026-10-10，遗留两项显式）

目标：**macOS 版可日常使用**——收掉 M1 遗留，补齐连接交互的工程硬伤。

- [x] M1 遗留收尾：
  - 主机密钥确认对话框：真三态 UI（未知 ≠ 拒绝），NSAlert 模态桥
        （工作线程 ↔ 主线程），已知一致直接放行不弹（core 侧
        配合：MATCH 短路 ACCEPT，decider 只审未知——见 commit 7a15089）
  - 私钥入 Keychain + passphrase 解锁（铁律 #5）：按路径缓存已解锁
        PEM、按 host:port 存口令；**无签名进程读 secret 会卡
        securityd**（实测 mach_msg 死等）——SecItem 全部带 2s 超时闸
  - 连接走工作线程（TCP+握手不钉死主线程）；TerminalSurface 创建
        回主线程（Metal 线程契约，工作线程建直接 SIGSEGV）
- [x] 修复的三个真缺陷（真 sshd e2e 暴露）：SSHSession UAF（transport
      借用 gt_ssh_t，谁建会话谁撑生命周期）；Keychain 死锁；known_hosts
      路径硬编码（e2e 污染用户真实文件——现可注入隔离）
- [x] 像素级渲染验证（2026-10-12）：`PixelRenderTests` 抓自己进程内的
      视图帧（`bitmapImageRepForCachingDisplay`，**不需要**屏幕录制权限），
      断言「红色背景真的出现在屏幕像素上」并配**对照**（同布局换绿底 →
      红像素基本消失）；另断言主题背景主色非纯黑、白字前景被绘制。
      变异 P1~P3 有牙（`mutation-check-pixel.sh`）。
      **修正一条旧结论**：早先记的「无权限抓像素必 SIGSEGV」不准——
      会崩的是 `CGWindowListCreateImage`（抓别人的窗口）；抓自己进程内的
      视图正常。那次「0 像素」不是权限拦下，是**抓到的帧里没内容**——
      所以现在的断言先做内容判别（全黑帧直接判红）再谈颜色。
      （抓帧取位图的顺序、以及 UI 层的坑另见
      `docs/reference/apple-ui-pitfalls.md`。）
- [x] 打包脚本 + CI：**双轨**。`scripts/package-mac.sh`（`.app` + `.dmg` + `.zip` +
      SHA256SUMS + BUILD-INFO；递归内嵌依赖闭包并断言不依赖 Homebrew；dmg 挂载复验 +
      包内二进制冒烟）是唯一出包逻辑，两个入口共用同一份可复用工作流
      `.github/workflows/macos-package.yml`：
      · `dev-build.yml` = 每次 push main → ad-hoc mac 包 → 滚动 `dev` prerelease；
      · `macos-release.yml` = tag `v*` / PR / 手动（`channel=release、gates=full`），
        多跑 Apple 原生、认证和打包变异；打包链从干净检出验证，缺主题不能被开发机资源掩盖。
      build 号取 `GITHUB_RUN_NUMBER`。**装机口径见 `docs/how-to/dev-build.md`**
- [ ] dev 包签名 + 公证：**遗留**——dev 轨永远 ad-hoc（无证书 secret 就不假装签名），
      且每轮 cdhash 不同、`dev` tag 会被强删重建，**不能当自动更新通道**
- [ ] Developer ID 签名 + 公证：**遗留**——本机无签名证书
      （`security find-identity` 0 项）；脚本与 CI 已支持（`--sign` /
      `--notarize`），拿到证书即用。**免费个人账号不是捷径**：Apple Development
      能签，但 Apple 不给这一档公证，用户下载来照样被 Gatekeeper 拦（记账档位
      `dev`，四档口径见 `docs/how-to/dev-build.md`）
- [x] macOS dev 包 **Homebrew cask**（tap `hanxi/homebrew-ghosterm`）：一条命令装/升级，
      `depends_on` 显式钉 arm64 + macOS 26 下限；cask 的 URL 与本轮资产名由
      `scripts/check-dev-cask.sh` 在 dev 轨对拍（牙见 `mutation-check-package-mac.sh` A10）。
      **它不解决 Gatekeeper**——实测 Homebrew 自己贴 `com.apple.quarantine`
      （agent 写 `Homebrew Cask`、flag `0381`），首次打开与浏览器下载同样要放行一次；
      实测唯一不贴标记的取包法是 `curl` + `ditto`。口径见 `docs/how-to/dev-build.md`
- [ ] cask 切回**真实 `sha256` 校验**：**遗留**——现在写 `sha256 :no_check` 是因为 `dev`
      资产每轮强删重建、字节钉不住。拿到 Developer ID 且出**不可变版本资产**之后，
      把 cask 换成钉版本 + 真 sha256，并删掉 `:no_check` 与滚动 URL；届时
      `scripts/check-dev-cask.sh` 里「必须是 `:no_check`」「必须是 dev 直链」两条断言
      要跟着反过来，否则对拍会替已经作废的假口径背书
- [ ] 降低最低系统版本：**遗留**——内嵌 OpenSSL 来自 Homebrew（minos 26.0），
      把 `LSMinimumSystemVersion` 抬到真实值；需自建 libssh2/OpenSSL 并设
      `MACOSX_DEPLOYMENT_TARGET`
- [x] 终端 UX（标签页 / 分屏 / 设置页）：见下方 **M5** 章节（含显式遗留）

## M3 — macOS ssh_config 图形管理（差异化 ①） ✅ 完成（2026-10-10）

- [x] config 编辑器 UI（按平台习惯）：HostEditorSheet（HostName / User /
      Port / IdentityFile 专控 + 其余指令通用表格，空值 = 删除）；
      **含 `Host *` / 通配块的列表与编辑**（M1 遗留项落点：通配块单独
      分组展示，不可点连接、右键可编辑）
- [x] 无损写回 + 写回前备份 + 冲突检测：ConfigStore（写侧三件套）——
      备份（.bak）→ 冲突检测（mtime/size，外部改过拒写不覆盖）→
      原子写（同目录临时文件 + rename，半份配置永不落盘）
- [x] core：`Include` 展开 / glob 求值（2026-10-12）：OpenSSH 同口径——
      相对路径相对 **~/.ssh**（实测 OpenSSH_10.3p1：同目录 inner.conf 不
      生效、`~/.ssh/inner.conf` 生效；无 -F 时 macOS ssh 走 getpwuid 忽略
      `$HOME`，带 -F 时 `$HOME` 参与解析）、`~`/`~user` 展开、glob
      无匹配静默、具名缺失显式失败、嵌套允许 + 深度上限 16；被含内容
      `included=1` 不进无损写回（铁律 #4）。断言 6 组 + 变异 I1~I4 有牙
      （`scripts/mutation-check-include.sh`）
- [x] 闸门：ConfigStoreTests 6 项 + ConfigEditTests 6 项（含通配块编辑、
      冲突拒绝、备份内容、directives 来自块内真相非求值结果）

## M4 — macOS SFTP 文件管理器（差异化 ②） ✅ 完成（2026-10-10）

- [x] core：SFTP 实现（libssh2_sftp 薄封装，边界拦截 + 条目上限）：
      list / stat / upload / download（job 后台线程 + 事件队列，
      PROGRESS ~16ms 合帧）；取消协作式（已写部分保留）；失败 job
      三态上报（OK / FAILED / CANCELLED 不折叠）
- [x] UI：浏览（Table：名称/大小/时间，目录导航）/ 下载（默认落
      ~/Downloads）/ 上传（NSOpenPanel）/ 传输面板（四态显式 + 取消）
- [x] 与终端解耦：SFTP 走独立第二条 SSH 会话（同 core 不同 UI，互不抢
      通道，不经终端面）；释放顺序铁律（先 sftp.close 再 session.close，
      libssh2_sftp_shutdown 依赖 session 存活）
- [x] 闸门：真 sshd 的 SFTP e2e（C 侧 18 断言 + Swift 绑定全链路 +
      SFTPBrowser 浏览/下载/失败三态）+ 变异验证 6 项有牙
      （`scripts/mutation-check-sftp.sh`：stat 尺寸 / list 丢名 /
      download 写空 / job 失败折叠成 OK）

## M4.5 — macOS 终端 UX（标签页 / 分屏 / 设置页）✅ 完成（2026-10-12）

目标：把 M1~M4 的「一窗一面」解成日常可用的终端宿主，并把设置做成**真的
生效**（不是写个文件）。布局与视觉参照
[iGhostVT](https://github.com/owngoal-dev/iGhostVT)（同一个 libghostty 引擎的
macOS/iPhone 终端）。

- [x] 标签页：`TabManager` 是结构的唯一写入者（增/删/激活/重排/关闭请求）；
      后台标签页的 surface **保持挂载**（只切可见性与命中测试——surface
      一释放，那条会话的网格与连接就没了）；关闭两步（请求 → 确认 → 关闭，
      不静默杀会话）
- [x] 分屏：递归 `PaneLayout`（叶子 + 轴分裂，三窗格以上的嵌套是常规用法，
      扁平结构早晚要重写）；关窗格后布局**塌陷**不留空壳；最后一个窗格
      不允许用「关窗格」的方式关（那是关标签页）
- [x] 分屏收尾（2026-10-14，用户实测带出）：
      **窗格自己退出就立刻收掉它**——本地 / SSH shell `exit`（读到 EOF）→
      分屏里布局塌陷、只剩一个窗格则**关标签页**（`pane.onExit` →
      `AppModel.handlePaneExit` → `closePane` / `requestClose`；`.failed`
      **不**自动收，保留错误卡片）。另修**结构改动后的分隔尺寸**：
      `HSplitView`/`VSplitView` 背后的 `NSSplitView` 在子视图集合变化后沿用旧
      比例（实测三窗格关中间 → 163.5 vs 735.5，而不是各半），按结构 id 强制
      重建（`PaneLayout.viewID`）后等分。断言 `SplitExitTests`（结构决策 +
      真 PTY 敲 `exit` 端到端 + 抓窗格几何），变异 `tabs` T7/T15/T16 有牙。
- [x] 三态不折叠（铁律 #6）：`PaneStatus` 四档显式（connecting / ready /
      failed / exited）；标签页状态取**最差**的窗格——一个失败不被别的
      ready 盖住
- [x] 设置页：通用（启动行为 / 默认 shell）+ 会话（当前标签页状态与关闭）
      + 外观（字号 / 字体 / 背景不透明度 / 光标 / 选中复制…）+ 高级
      （三个配置文件路径 + 终端配置原文）
- [x] **设置真的生效**：外观走 **Ghostty 语法**的配置文件交给 libghostty
      解析（不自己维护一份配置子集——那必然随上游漂移）；`font-size` 改完
      网格密度真的变（断言在 `TerminalConfigTests`）
- [x] 闸门：16 项结构断言（`TabManagerTests` / `SplitLayoutTests`）+ 11 项
      设置断言（`TerminalConfigTests`）+ 设置断言变异 S1~S4 有牙
      （`mutation-check-settings.sh`）

### M4.5 遗留（**显式**：未做的说没做，做掉的不留在「遗留」里充数）

- ~~terminal 里卡住的程序拖垮整个 app~~ **已修（2026-10-15，用户实测带出）**：
  后台/前台一个**不读 stdin** 的程序（卡住的进程、只输出的程序）会把 pty 输入
  队列撑满（macOS 实测约 1 KiB），`gt_transport_write` 在队满时 `poll(...,-1)`
  **无限等**——于是会话线程被永久钉死：① 读循环再也出不来（终端画面冻住）；
  ② 关标签页时 `SessionController.stop()` 的 `queue.sync` 恰好在等这条线程，
  **主线程一起卡死＝整个 app 无响应**。修法：写**有界**（队列满最多等一小段就
  把余量攒进 transport 内部；每次 read 顺带补送，与真实终端流控同语义，输入
  不丢只是晚一点送到）。断言 `test_transport` 第 13 节（写有界 + 积压头尾都
  送达 + 回显无截断），变异 `mutation-check-transport` P4 有牙（回到 `poll(-1)`
  即整条套件卡到 timeout 变红）。
  SSH 写路径的**同构修复**（2026-10-15 同日收尾）：`gt__ssh_shell_write`
  曾经 `EAGAIN → continue` 自旋 + 把 `LIBSSH2_ERROR_TIMEOUT` 当硬错丢字节
  （`GT_ERR_IO`，键入的字节就这么没了）。修法与 PTY 同结构：写路径在
  `GT_SSH_WRITE_GRACE_MS` 内有界返回、剩下的进 `wtail` 队列、由后续 read/
  write 补送；timeout 由写路径自己 `get_timeout` 保存 / `set_timeout` 还原，
  不打扰同 session 的 SFTP。契约见 `transport.h`（输入不丢，只是晚到）。
  `test_ssh` §5b 断言（写有界 + rc=GT_OK）；e2e 已知限制：sshd 排空速率
  ≈ 40 MiB/s，3 MiB payload 写完时队列**根本没形成**——flush 机制无法被真
  sshd e2e 触发，由代码审阅 + PTY §13/P4 的变异覆盖保证同构正确性。

- **标签页恢复 / 持久化**：退出后重开还原上次的标签页。iGhostVT 靠常驻
  后台守护进程（launchd）让会话活过退出，我们**不做常驻 helper**（那是另一
  套安全模型：XPC + audit token 鉴权）。我们能做的是「记下来下次重建」，
  不是「会话不中断」——两者别混为一谈。
- **拖拽重排的 UI 手势**：`TabManager.move(_:to:)` 已实现并有断言，
  但标签条上还没有拖拽触发面。
- **分屏焦点下发**（用户实测反馈带出）：光标闪烁从没下发到 libghostty——每个
  窗格的 surface 都停在引擎「默认已聚焦」的默认值上，于是**每个格子都在闪**。
  已补 `gt_ghostty_surface_set_focus` / `gt_ghostty_app_set_focus`（CGhostty
  shim）+ 端侧 `TerminalSurface.setFocused` / `TerminalApp.setFocused`，并由
  `TerminalSurfaceView·TerminalHostView` 在「当前窗格 **且** 窗口 isKeyWindow」
  时下发（窗口 key 变化、App 前后台都观察）。非聚焦面 = 引擎画的空心块 + 停闪
  （对齐上游 Ghostty / kitty / iTerm2 的口径）。断言 `WindowFocusTests` +
  `TerminalSurfaceTests`（DEC 1004 焦点上报字节），变异 `focus` F1~F3 有牙。
- **非聚焦窗格变暗**（与上一条同批，对齐上游 ghostty）：盖一层
  `unfocused-split-fill`（未设回落 `background`）的遮罩，alpha =
  1 - `unfocused-split-opacity`（引擎把它夹在 [0.15, 1]，≥1 即关掉）。为此新加
  `gt_ghostty_config_get_color`（读引擎里**生效**的配色，主题间接来源也对）。
  规则是纯函数 `SplitDim.resolve`；断言 `SplitDimTests`（规则穷举 + 端到端抓帧：
  非聚焦那半偏红、聚焦那半不偏，且 opacity=1 时两半都不偏），变异 `split-dim`
  D1~D3 有牙。
- **后台标签页停画**（同批，省 CPU/GPU）：后台标签页的 surface 我们一直是
  **保持挂载**的（一释放就没网格、没连接），但 libghostty 不知道它没显示——
  渲染线程照常跑。现在把可见性报给引擎：`gt_ghostty_surface_set_occlusion`
  + `TerminalSurface.setVisible`，判据 = 「当前显示的标签页」且窗口 occlusionState
  含 `.visible`、未最小化（上游 macOS 同口径；窗口遮挡状态一变就重新同步，否则
  误判一次画面就停死）。断言 `WindowFocusTests` 可见性两条 + 引擎侧
  `TerminalSurfaceTests`（DEC 2033 的 `CSI ?999;1n / 2n`），变异 `occlusion`
  O1~O3 有牙。
- ~~主题选择器~~ **已做（2026-10-12）**：`build-ghostty.sh` 从 zig-pkg 的
  `iterm2_themes` 依赖派生 607 套配色到 `tmp/ghostty-resources`，
  `package-mac.sh` 拷进 `Contents/Resources/ghostty/themes`；CGhostty 在
  `ghostty_init` 前按可执行文件路径推导 `GHOSTTY_RESOURCES_DIR`（上游支持
  的入口；只在自己没被设置时兜底）。设置页「外观」多了配色下拉，
  出包脚本加了 **包内配置检查**（`--config-check`，在真装配好的 .app 里验
  随包主题被引擎接受），变异 A6 证明它缺了主题会红。
  **补（2026-10-14）**：上线后第一个包**其实没带主题**——定位写成
  `find -type d` 而 zig-pkg 里的 `iTerm2 Default` 是**文件**，永远找不到、
  静默 SKIP；又因 xcframework 命中缓存时 `package-mac.sh` 不跑
  `build-ghostty.sh`，连那次 SKIP 都到不了。定位已抽到
  `scripts/lib/ghostty-themes.sh`（两处共用，按**文件**找 + 条数下限），
  `package-mac.sh` 改成**自给自足**（缺配色集直接 fail，不再 SKIP），
  变异 A7 双向证明它有牙（见下）。
  **更正（同日）**：上面这条曾写「`--config-check` 抓不到主题名找不到」，
  那是**错的**——我把探针跑在了一个当时已被自己搞乱的 `.app` 上。
  在**装配完好**的包上重验：把 `Resources/ghostty/themes` 移走（或整个
  `ghostty` 目录移走）后，`theme = "Rose Pine"` 立即
  `配置加载失败：theme "Rose Pine" not found`；移回即 `CONFIG_OK`。
  所以 A6 那条断言对「包没带主题」是**有牙**的——它没瞎。
  这次漏过去的真正原因是 `package-mac.sh` 在**没主题时压根没跑过这条检查**
  （走到 `else` 分支 note 一声就过去了），不是检查本身失效。

## M4.6 — macOS 文件管理器重构（SFTP → 统一文件浏览器）🚧 进行中（2026-10-14）

目标：把「单栏只读 SFTP 浏览」重构成统一文件浏览器，与 README 的
「**WinSCP 式** SFTP 文件管理器」定位对齐。计划与取舍见
`docs/reference/plan-files-manager.md`。分阶段切片，每片独立提交 + 闸门绿。

- [x] **S1 core** —— 完整文件操作原语（mkdir/rmdir/remove/remove_all/rename/
      chmod/realpath/lstat/readlink/walk/pause）+ **SSH exec 通道**（gt_ssh_exec，
      远端加速：递归搜索/大树遍历放远端本地算）+ **每会话 I/O 锁 + 单 I/O
      worker 线程**（修掉「每 job 一线程共用同一 libssh2 会话」竞态）。
      闸门：`gate.sh`（57 公共符号）+ 真 sshd e2e 75 断言 + `mutation-check-sftp`
      （F1~F8）+ `mutation-check-exec`（E1~E2）。
- [x] **S2 bindings + 接缝 + 面板** —— `SFTPSession`/`SSHExec` 绑定；`FileBackend`
      接缝（`LocalFileBackend` / `SFTPFileBackend`）+ 统一 `FileEntry`；
      `FilesPanelModel`（导航/排序/过滤/隐藏/多选/搜索）+ **内嵌文件面板**
      （终端标签页右侧，⌘⇧E；本地 shell 后端=本地、初始目录跟随终端 cwd；
      SSH 后端懒开第二条会话）；`RemoteSearch`（exec 优先、禁 exec 显式回退
      walk）。闸门：`FilesPanelTests` 7 项 + `mutation-check-files`（F1~F4）。
- [x] **S3** —— 文件操作与右键菜单（新建/重命名/权限/信息/拷贝路径/打开/
      删除**带三态确认**；符号链接图标区分、删链接不删目标）；权限「应用到
      子项（递归）」+ 多选批量删除/下载；面板**树/列表可切**（懒展开目录树）。
- [x] **S4 全局传输队列** —— 前置已做：`CoreEventPump`（唯一事件泵，扇出给
      全部订阅者；`gt_core_poll_events` 一次性消费，两个轮询者会互相抢）。
      `TransferManager`：跨标签聚合、进度/速度/ETA、暂停/继续/取消/重试、
      目录展开成文件级 job、冲突「覆盖/跳过/保留两份 + 应用到全部」；传输窗口
      （窗口菜单「传输」）。`SFTPFileBackend` 自持有串行队列（供全局队列安全
      跨线程调用）。闸门：`TransferManagerTests` 9 项 + `CoreEventPumpTests` 2 项
      + `mutation-check-transfers`（T1~T4）。
- [x] **S5 独立标签页双栏** —— `.sftp(target)` 改 `DualPaneFilesView`（左本地
      右远程；`DualPaneSession` 持两栏 `FilesPanelModel`）。栏间拖放即传
      （把远端条目拖到左栏=下载、本地条目拖到右栏=上传）；下载/上传经全局
      传输队列。旧 `SFTPBrowser`/`SFTPBrowserView` **已删除**（被统一文件
      浏览器取代）。闸门：`SFTPBrowserTests`（真 sshd 双栏 e2e：导航/经队列
      下载/失败三态）+ `mutation-check-files` F5（DualPaneSession 接线）。
- [x] **S6 系统集成（macOS）** —— 拖放路由 `DropRouter`（栏间拖：远端→本地=
      下载、本地→远程=上传；Finder 拖入远程栏=上传）；本地条目拖出到 Finder
      （真 fileURL NSItemProvider）；Quick Look：空格（macOS 14+，挂着文件
      列表上）+ 右键「快速查看」，远端条目经队列下临时副本再预览。
      闸门：`FilesDragDropTests` 5 项 + `mutation-check-dragdrop`（D1~D3）。
- [ ] **S6-iOS** —— 底部抽屉 + 本地/远程分段。**未做（证据链）**：
      1) 随包 `vendor/ghostty/macos/GhosttyKit.xcframework` 只有 `macos-arm64`
         一个 slice（`Info.plist` 实测）；
      2) 钉住的 libghostty 构建系统**只提供 macOS**：`src/build/GhosttyLib.zig`
         只有 `initMacOSUniversal`（aarch64+x86_64），**没有全量 lib 的 iOS 路径**；
         iOS 只有 `libghostty-vt` 有 slice（`GhosttyLibVt.ApplePlatform{ios,
         ios_simulator}`）——而铁律 #7 规定 iOS 用 **full libghostty**，不是 vt；
      3) 实测 `swift build --triple arm64-apple-ios16.0-simulator` 在
         `GhostermTerminal/TerminalSurface.swift` 就报 `could not build
         Objective-C module 'AppKit'`（本环境**有** iOS 26.5 SDK 与模拟器，故
         不是工具链缺失，是引擎/宿主缺失）。
      => 前置是**独立里程碑**：给钉住的 libghostty 加 iOS 全量构建路径
      （走 `patches/libghostty/`，属上游 R&D）+ 把 `GhostermTerminal` 与 app shell
      的 AppKit 依赖抽成 macOS/iOS 双实现。写自欺的 `#if os(iOS)` 代码不符
      「诚实标注」铁律，故不做。
- [x] **S7 收尾** —— ⌘T 沿用当前类型、侧栏右键「用 SFTP 浏览文件…」与新形态
      对齐；文档收口。（SSH↔SFTP「同目录落地」按既定取舍不在此列：依赖远端
      cwd，OSC 7 未达宿主。）
- [x] **S8 面板顶栏 + 标签右键菜单**（2026-10-15，用户实测三条反馈带出；计划
      见 `docs/reference/plan-panel-tab-menu.md`）—— ① 顶栏工具条改 `FlowLayout`
      **折行**（原来一行 `HStack`，260pt 面板放不下、右边按钮被画到框外＝
      「顶部按钮超框了」）；② 顶栏第一行最右补 **✕ 关闭按钮**（此前面板只能
      ⌘⇧E / 「显示」菜单开合＝「SFTP 侧边栏如何关闭」）；③ **标签右键菜单**
      `TabContextMenu`（侧栏行 / 顶栏胶囊 / 标签条 chip 共用一份：文件面板
      开关 + 分屏 + 关分屏 + 锁定 + 关闭），顶栏那两个可见分屏按钮移入菜单。
      菜单作用在**被右键的那条**标签并把它带到前台。闸门：`PanelChromeTests`
      6 项 + `TabContextMenuTests` 4 项 + `apple-app-gate.sh`；变异沿用既有两份
      脚本扩展——`mutation-check-files` F10~F12、`mutation-check-tabs` T17/T18
      （**变异份数不变**，仍 26 份）。

> **未做即未做（诚实口径）**：S4/S5/S6 未动；`gt_sftp_pause` 原语已在 core 并
> 有 e2e，但**传输队列 UI 还没接**；「SSH↔SFTP 同目录落地」依赖远端 cwd
> （OSC 7 到不了宿主，见 M4.5 遗留 / R2），仍未做。

## 可选 tmux 后台会话重构

设计、依赖闭包和命令契约见 [重构计划](plan-optional-tmux.md) 与 [实施清单](optional-tmux-implementation.md)。

- [x] 默认本地/SSH 入口切回普通 PTY，移除旧启动恢复与安装 UI；真实普通终端、分屏、SFTP 及变异验证通过。
- [x] 退役旧 daemon、IPC、快照、replica、专用补丁、ABI、构建与随包 runtime；通用认证独立保留。
- [x] 共享 tmux 管理与 SSH PTY exec、本地/远端交互 attach。
- [x] 后台会话列表、新格子连接、断开提示与刷新后重新选择。
- [x] 完整本地/远端、网络中断、竞态、原生 UI、变异与干净打包验收；支持范围和证据档位见实施清单。

后台能力由用户显式启用；普通终端无后台保活承诺。tmux 内窗口与分屏交给普通客户端，
不使用 control mode，不恢复私有快照协议，不保留旧记录或 ABI 兼容层。

## M5 — Android 竖切

> config 编辑 / SFTP 沿用 mac 阶段已验核心；远端会话的共享 C 契约仍在开发，
> Android 通过同一 C ABI/attachment 契约接入，所需核心改动归会话专项同步各端绑定。
> 远端保活由服务器承担，不以 Android 本机常驻 daemon 为前置条件。

- [ ] `terminal/android`：libghostty-vt 交叉编译 + GLES/Canvas 自绘
      （脏行更新、IME / 组合键 / 软键盘辅助条，对照 [Chuchu](https://github.com/jossephus/chuchu)）
- [ ] `android/`：Compose 宿主 + JNI 绑定（`ghosterm.h` 对拍并入 ABI 闸门）
- [ ] W^X PoC：proot 在 app 数据目录内可执行（M5 首个 PoC；失败则本地
      shell 只走 `android sh` 档，不静默降级）
- [ ] `runtime-payload`：Termux bootstrap + proot（按需下载、sha256、原子激活）
- [ ] 本地 shell 两档：`android sh`（自带）/ Termux（payload）

## M6+ — 其余端

iOS（复用 Apple 那套 + iSH 打包）→ HarmonyOS / Windows / Linux / Web

## 显式延后（不在当前排期，能力矩阵保持诚实口径）

- 端口转发 -L/-R/-D（core `forward`）：待真实需求排期
- known_hosts 的 hashed 条目 / @revoked
- **SSH ↔ SFTP 跳转**（从 SSH 终端跳到同主机的 SFTP）：按 `RemoteTarget` 匹配同一台
  主机（`pattern`/`user`/`host`/`port`），已有该主机的 SFTP 标签页则切过去、没有则新开。
  入口形态（右键菜单 + 快捷键，二者都要能加）待做时敲定。**同目录落点依赖远端 cwd**，
  后者依赖下一条。接口已在 `TabKind.remoteTarget` / `TerminalTab.currentDirectory` 留好。
- **shell integration 注入（R2）**：给本地/远端 shell 注入 OSC 7 / OSC 133 集成脚本，
  让「cwd」与「语义提示符」**无条件**可用。等于 Ghostty 的 shell-integration 整块
  （bash/zsh/fish/elvish/nushell + 远端传输 + 安全审慎），是独立里程碑。
  **现状**：OSC 133 / OSC 7501 的**透出**已做（补丁 0002/0003 + 端侧回调）；
  缺的是**注入**。而且**实测**：即便有集成，OSC 7 在我们钉的 embedded host-IO
  下也**到不了宿主**（`apprt/embedded.zig` 没有 surface message 的 pwd 通路），
  且 libghostty 要求 OSC 7 的 host 必须 `isLocal`（只认 `localhost`/本机名）——
  故远端 cwd 需要**上游支持**（或 R2 注入时改写 host 为 `localhost`）。
  本地 cwd 不依赖集成，走内核 `proc_pidinfo` / `/proc/<pid>/cwd`（**已交付**）。
  计划与取舍见 `plan-tabs-close-and-newtab.md`。

## 已知风险

| 风险 | 应对 |
|---|---|
| full libghostty 嵌入需上游补丁，API 未稳定 | 锁 tag + `patches/` + WHY.md；官方明示会破坏性变更 |
| libssh2 阻塞 + SFTP 历史漏洞 | 一 session 一 worker 线程；经薄封装隔离，不让上层碰句柄 |
| mac 阶段战线拉长（config GUI / SFTP 前置） | M2/M3/M4 各自独立可交付、每里程碑出一版；SFTP 先核心后 UI 可再切片 |
| Termux proot 的性能/体积 | 按需下载；先验证 `android sh` 再上 payload；W^X PoC 前置 |
| iOS iSH 体积与许可 | 延后；GPL source offer 见 licensing.md |
| 无 daemon：关窗口 / 退出**真杀**会话 | 关窗口/退出按「有前台作业才弹一次」确认（`hasRunningProgram` 口径，同 iGhostVT）；可选 tmux 格子另按 attach 生命周期处理。见 `plan-tabs-close-and-newtab.md` |
| 远端 cwd 依赖 shell integration 注入（R2） | 现阶段只做 plumbing（OSC 7，有则用、无则 nil）；不注入、不猜。R2 列为独立里程碑 |
