# 计划：关闭确认降噪 + ⌘T 继承当前标签页 + cwd 地基

> 状态：**S1–S5 已交付**；S6 独立跳转与远端 cwd 仍延后。本文保留 2026-10-14 的设计/验收记录，当前单窗口导航与统一工作区见[标签界面计划](plan-tab-chrome-refactor.md)，可选后台保活见[tmux 计划](plan-optional-tmux.md)。
> 进度：**S1–S5 全部落地（收口）**（前台判定 + 关闭确认降噪 + ⌘T 继承 & SFTP 每次新开 &
> TabKind 统一 + cwd 地基的**本地**一半 + ⌘T 继承本地目录）；S6 只在 roadmap。
> **S4 的诚实口径**：本地 cwd（内核 `gt_transport_child_cwd`）已交付且可靠；
> 远端 cwd（OSC 7）**经实测在我们钉的 embedded host-IO 下到不了宿主**（无 PWD
> action；且 libghostty 要求 OSC 7 host `isLocal`）——接线保留为 seam，远端
> cwd 实际依赖上游支持 / R2。
> 基线：`9992d3d`（OSC 133 / OSC 7501 已透出给宿主之后）。
> 参照实现：iGhostVT（`owngoal-dev/iGhostVT`，核对于 1.6.11 / `860cbc4`）——
> 本计划的关闭判据就是它的 `TerminalTab.hasRunningProgram` 同口径。

## 当前行为与历史方案的差异（2026-10-29）

下文保留 S1–S5 当时的接口形态与测试记录，当前代码以以下行为为准：

- `TabKind` 已增加 `.localFiles`。`AppModel.newTabLikeActive()` 对没有终端叶子的标签创建对应文件工作区，本地文件标签不会因 ⌘T 启动 shell；有终端时仍按本地/SSH 类型新建。导航加号打开连接选择器，与 ⌘T 继承不是同一动作。
- 布局由 `WorkspaceLayout` 统一终端和文件叶子，`PaneLayout` 是只读终端投影。关闭确认检查实际窗格和关联传输，不能仅按标签的 `.sftp` 类型判断；未完成连接的远端文件窗格也可能要求确认。
- tmux 窗格关闭只断开客户端，不结束后台任务，也不沿用普通 SSH 的 UNKNOWN 作业确认。普通本地/SSH 的作业与未知判据继续有效；没有计划恢复自有 daemon。
- 终端自行退出只移除该叶子，文件叶子仍在时保留标签；用户关闭整个标签与 shell 自行退出不是同一生命周期路径。

当前入口见 `AppModel.newTabLikeActive`、`TerminalPane.closeNeedsConfirmation`、`TerminalTab.closeNeedsConfirmation`；后续实现与验收见[标签界面计划](plan-tab-chrome-refactor.md)、[工作区交接](workspace-handoff.md)及[tmux 使用说明](../how-to/tmux-sessions.md)。

## 0. 一句话

关闭**只在该拦的时候拦**（前台有作业、或有在途传输才可能丢东西）；
**新建标签页沿用当前标签页的类型**（本地→本地、ssh→同主机、SFTP→同目标）；
cwd 只做**地基**（本地可靠查询 + 远端 OSC 7 plumbing），不做 shell 集成注入。

## 1. 决策表（已锁定）

| # | 决策 | 值 | 落点 |
|---|---|---|---|
| A | 关标签页切判据 | 前台有作业 / 有在途传输 → 确认；空闲 / 已退出 / 失败 / 连接中 → 直接关 | S2 |
| B | SFTP 与 SSH 一致 | **每次新开**，删去重 | S3 |
| C | ⌘T | 沿用当前标签页类型（本地 / ssh 同主机 / SFTP 同目标） | S3 |
| D | 旧名 | `sshTarget` **删除**，统一 `RemoteTarget` struct，不留别名 | S3 |
| D1 | `TabKind.instance` | **彻底删除**（身份 = `TerminalTab.id`） | S3 |
| E | 关窗口 / 退出 | 有前台作业才弹一次 | S2 |
| F | roadmap | 增「SSH ↔ SFTP 跳转」一行 | S6 |
| D2 | 远端 cwd | 走 **R1**（只做 plumbing，不注入 integration）；**R2 记入文档**；S5 顺带；S6 留 roadmap | S4 / S5 |
| D3 | S6 入口形态 | 右键菜单 + 快捷键**都要能加**，做 S6 时敲定 | S6 |

### 与 iGhostVT 的对照（关闭判据）

- iGhostVT `TabManager.requestClose`：`if tab.hasRunningProgram || tab.isRemote { 弹 } else { 直接关 }`；
  `hasRunningProgram = hasLiveSession && !isIdleAtPrompt`；
  `isIdleAtPrompt = (status == .connected && isShellInForeground)`；
  `isShellInForeground` 来自 **`tcgetpgrp(master) == childPID`**（它就是"前台是不是 shell 自己"）。
- 口径与铁律 #6 一致：**detached / 未知状态一律当"可能有事"**（iGhostVT 原话：*an unknown state reads as running*）。
- **注意差异**：iGhostVT 关窗口/退出几乎不弹，是因为它**不杀**会话（shell 活在 daemon 里，关窗口 = detach）。
  本历史切片的普通 Ghosterm 会话**没有 daemon**，关闭会终止客户端；当前可选 tmux 已另行实现“只断客户端、保留后台任务”，见上方当前行为，不能将此段作为重引入 daemon 的计划。
- 「空闲提示符 = 直接关」是 iGhostVT 的**默认行为，且无配置项**；本计划也不加配置项。

## 2. 键模型（`TabKind` 重构）

两个键分离：**身份**（`TerminalTab.id`，已有）与**目标**（`RemoteTarget`）。

```swift
// TerminalTab.swift —— 新增
/// 一台远端主机 + 认证主体。`pattern` 是 ssh_config 别名（给人看），
/// `host` 是求值后的真实主机名（给连接用）——两者都要留。
public struct RemoteTarget: Hashable, Sendable {
    public let pattern: String
    public let user: String
    public let host: String
    public let port: UInt16
}

// TabKind 重构：删掉全部 instance、删掉 sshTarget
public enum TabKind: Equatable {
    case localShell
    case ssh(RemoteTarget)
    case sftp(RemoteTarget)

    /// 远端目标（本地 shell 为 nil）。**跳转 / 复建都按它匹配"同一台主机"**。
    public var remoteTarget: RemoteTarget? {
        switch self {
        case .localShell: return nil
        case let .ssh(t), let .sftp(t): return t
        }
    }
    public var isTerminal: Bool { if case .sftp = self { return false }; return true }
    public var isLocalShell: Bool { self == .localShell }
    // defaultTitle / subtitle / symbolName：按新 shape 改写
}

public static func newSSH(_ t: RemoteTarget) -> TabKind { .ssh(t) }
public static func newSFTP(_ t: RemoteTarget) -> TabKind { .sftp(t) }
```

**为什么可以删 `instance`（D1）**：`instance` 当初唯一的用途是让"有去重逻辑"能区分两个同 kind 的标签页。
SSH 已经不去重（commit `3b71d20`），本计划把 SFTP 也改成不去重 → **全仓再无任何按 `kind` 去重的代码** →
`instance` 成为纯累赘；身份本来就有 `TerminalTab.id`。删除后 `TabKind` 回归"是什么 + 在哪"，
是 value-type 的本义。

**风险**：以后谁再把"已开着就切过去"加回来就会踩。由变异 **T6 / T8 / T11** 钉住（这正是它们存在的意义）。

**改动面（约 12 处，机械）**：`TerminalTab.swift`（枚举 + `defaultTitle`/`subtitle`/`symbolName`/`sshTarget`→`remoteTarget`）、
`AppModel.swift`（`selected` / `status` / `openSFTP` / `splitActiveTab` / `connect`）、`RootView.swift`、
`TabStrip.swift` / `Sidebar.swift` / `AppMenu.swift`（新建入口）、测试构造点 `TabManagerTests` / `SidebarChromeTests`。
> `TabKind` 仍留在 `TerminalTab.swift`（不新开文件）——否则要把新文件加进 `mutation-check-tabs.sh` 的 `FILES` 数组。

## 3. 切片（S1–S5 实现，S6 文档）

### S1 — core：前台判定（关闭判据）
- **加** `core/include/ghosterm/transport.h`：
  ```c
  typedef enum {
    GT_TRANSPORT_CHILD_UNKNOWN = 0, /* SSH shell / ConPTY / 查询失败（不猜） */
    GT_TRANSPORT_CHILD_EXITED  = 1,
    GT_TRANSPORT_CHILD_IDLE    = 2, /* 前台就是 shell 自己（空闲提示符） */
    GT_TRANSPORT_CHILD_JOB     = 3,
  } gt_transport_child_t;
  gt_transport_child_t gt_transport_child_state(const gt_transport_t *t);
  ```
- `core/src/transport.c`：`SSH_SHELL`→`exited?EXITED:UNKNOWN`；
  `LOCAL_PTY`→`exited?EXITED : tcgetpgrp(master)==child ? IDLE : JOB`，异常 `UNKNOWN`。
- **验收**：`core/tests/test_transport.c` 四态；`check-abi.sh` 双向对拍（新符号已声明↔已导出）。
  **探针**：`tmp/` 实测 `tcgetpgrp(master)`（iGhostVT 已在 macOS 用同法）。
- **依赖**：无。**风险**：`tcgetpgrp(master)` 语义（探针兜底）。

### S2 — apple：关闭确认降噪 + 关窗口确认
- `SessionController` 透传 `childState` → `TerminalPane.childState`。
- `TerminalTab.closeNeedsConfirmation`：终端看 `panes`（`.job`/`.unknown` → true）；SFTP 看 `transfers` 有无 `.running`。
- `TabManager.requestClose` 收口：
  ```swift
  public func requestClose(_ tab: TerminalTab) {
      if tab.closeNeedsConfirmation { closeRequest = tab }
      else { cancelInFlight(of: tab); close(tab) }
  }
  ```
  + 新增 `cancelInFlight(of:)`（在途连接取消：工作线程回主线程时 tab 已不在 → 不 attach、不弹凭据框）。
- 关窗口（`AppDelegate`）：`windowShouldClose` / `applicationShouldTerminate` 用 `TabManager.hasRunningPrograms`
  弹一次（文案：「关闭窗口会结束 N 个正在运行的会话」）；`applicationWillTerminate` 保持 `tabs.removeAll()`。
- `RootView.swift` 的关闭确认 alert 文案随判据调整（只在真有东西可丢时出现）。
- **验收**（`TabManagerTests`）：`.exited/.failed/.connecting/空闲本地/有作业本地/SSH` 六档 + 关窗口两档 + `hasRunningPrograms`。
- **变异**：**T2 锚点必须更新**（`requestClose` 体变了）；新增 **T9**（空闲→直接关）、**T10**（关窗口静默杀）。

### S3 — apple：TabKind 重构 + ⌘T 继承 + SFTP 每次新开
- §2 的 `RemoteTarget` + `TabKind`（删 `instance` / `sshTarget`）。
- `openSFTP` 删去重（`if let existing …` 整段去掉）→ **每次新开**；签名改为 `openSFTP(_ target: RemoteTarget)`。
- 新增 `AppModel.newTabLikeActive()`：本地→本地、`.ssh(t)`→`newSSH(t)` 新开、`.sftp(t)`→`openSFTP(t)` 新开、
  无标签页→本地。抽私有 `openTerminalTab(target:)` 让 `connect` / `newTabLikeActive` 共用。
- `AppMenu.newTab` / `TabStrip` 溢出「新建标签页」→ `newTabLikeActive()`。
  `ShortcutCatalog` 标题不变（⌘T 仍「新建标签页」）→ 快捷键对拍仍绿。
- **验收**（`NewTabMenuTests`）：`testOpenSFTPTwiceOpensTwoTabs`、`testNewTabInheritsLocalShell` /
  `InheritsSSHHost` / `InheritsSFTPTarget` / `NoTabsOpensLocalShell`、`testSameTargetKindsAreEqual`（钉「kind 不再承载身份」）。
- **变异**：**T6/T8 锚点同步更新**（`connect` 重构、那行注释可能移位）；新增 **T11**（把 SFTP 去重加回）、**T12**（⌘T 写死本地）。

### S4 — core + apple：cwd 地基（R1）——【已落地，远端半如实降级】

> **实测更正（2026-10-14）**：R1 的「远端 cwd 走 OSC 7」在**我们钉的 embedded
> host-IO 下拿不到**——喂 `ESC ] 7 ; file://localhost/path ST` 时 `cb_action`
> 收不到任何 `GHOSTTY_ACTION_PWD`（`apprt/embedded.zig` 没有 surface message
> 的 pwd 通路），且 libghostty 校验 OSC 7 的 host 必须 `isLocal`（只认
> `localhost` / 本机 hostname）。故**本轮只交付本地 cwd**（内核查询，可靠、
> 有断言）；OSC 7 接线（`onPwd` / `reportedDirectory` / shim 的 `PWD` 分支）
> 保留为 seam，远端 cwd 依赖上游支持或 R2（注入且发 `localhost`）。
> 断言按诚实契约写：**没有内核 cwd 的会话 → `currentDirectory == nil`**。

### S4 — core + apple：cwd 地基（R1）
- **本地（可靠，无需 integration）**：`gt_transport_child_cwd()`（macOS `proc_pidinfo(PROC_PIDVNODEPATHINFO)`；
  Linux `readlink /proc/<pid>/cwd`；其余 `UNKNOWN`）→ `TerminalPane.currentDirectory`。
- **远端（OSC 7，plumbing）**：`shim.c` 的 `cb_action` 加 `GHOSTTY_ACTION_PWD` 分支 →
  `ghostty_host.h` 的 `gt_host_callbacks_t` 加 `pwd` → `TerminalSurface.onPwd`（libghostty 线程 → **hop 主线程**）
  → `TerminalTab.currentDirectory`。
- **不做**：不注入 integration（那是 R2，见 §6）。远端 cwd **有则用、无则 nil，不猜**（铁律 #6）。
- **验收**：core `test_transport` 本地 cwd 断言；terminal 侧 `onPwd` 断言（喂 `ESC ] 7 ; file://host/path BEL`
  后 `currentDirectory == /path`）+ 变异（把 `onPwd` 的 hop 去掉 → 红）。
- **依赖**：S1 的 transport 查询同源（可相邻提交）。

### S5 — apple：⌘T 继承本地目录（顺带）
- 本地壳新标签页带 `cwd`：`SessionController.localShell(... cwd: tab.currentDirectory)`（**该参数已存在**，现在只差传值）。
- 远端：仅当 S4 拿到 OSC 7 时，新 ssh 会话以该目录起（远端 shell `cd` 过去——本轮只在有 cwd 时做，无则家目录）。
- **验收**：`NewTabMenuTests` 加「本地 ⌘T 的 cwd == 当前标签页 cwd」断言（用 `proc_pidinfo` 读回新壳的 cwd 对拍）。

### S6 — 只入 roadmap（不实现）
- `RemoteTarget` + `currentDirectory` 是它的接口；入口（右键 + 快捷键，二者都要能加）待做时敲定。

## 4. 契约（不随切片变）

```
关闭确认：能证实"没什么可丢"才不弹；查不出来（UNKNOWN）一律弹。   // S2
新建标签页：沿用当前类型 + 远端目标；永远新开、绝不去重。           // S3
cwd：本地 = 内核查询（可靠）；远端 = OSC 7（有则用、无则 nil）。    // S4
```

## 5. 闸门与变异

- 闸门：`scripts/gate.sh`（含 C `check-abi.sh`）、`check-abi-apple.sh`、`apple-app-gate.sh`、`terminal-gate.sh`、
  `gate-ghostty-patch.sh`（S4 动了 `shim.c`/头 → 必须）。
- 变异脚本：
  - `scripts/mutation-check-tabs.sh`：**T2 / T6 / T8 锚点更新**，**新增 T9 / T10 / T11 / T12**。
  - `scripts/mutation-check-terminal.sh`：若 S4 动 `TerminalSurface` 的 `onPwd`（hop 线程），加一项。
  - 纪律照 AGENTS.md：起跑前扫标记、trap 复原、回滚比对 sha256、双断言造指向性。
- 关键回归点（必须仍绿）：`SettingsUITests.testShortcutCatalogMatchesMenu`（⌘T 标题不变）、
  `TabManagerTests.testCloseStopsTheSession`（关闭仍先停会话）、`testLocalShellAlwaysOpensNewTab` / `testSameHostOpensNewTabEachTime`。

## 6. 明确不做 / 显式延后

- 本历史切片不做 daemon / 常驻 helper / 会话保活 / 标签页恢复。后续可选 tmux 已交付，普通会话和标签跨重启恢复仍未提供。
- **shell integration 注入（R2）**：给本地/远端 shell 注入 OSC 7/133 集成脚本，让 cwd 与语义提示符**无条件**可用。
  等于 Ghostty 的 shell-integration 整块（bash/zsh/fish/elvish/nushell + 远端传输 + 安全审慎），独立里程碑。
  （**OSC 133 透出**已做；缺的是**注入**。）
- "空闲也问" 配置项、永久"新建本地 shell"快捷键（决策 A/C 已定）。
- Windows 的 `child_state` / `cwd`：一律 `UNKNOWN`，不假装知道。
- S6 跳转本体。

## 7. 收口（2026-10-14）

S1–S5 已全部落地 `main`（S6 只在 roadmap）：

| 切片 | 提交（摘要） |
|---|---|
| S1 | `fix(core) 本地 PTY 控制终端` + `feat(core) gt_transport_child_state` + `feat(apple) 绑定` |
| S2 | `feat(apple) 关闭/退出只在有东西会被打断时确认` |
| S3 | `feat(apple) ⌘T 沿用当前类型；TabKind 统一 RemoteTarget；SFTP 每次新开` |
| S4 | `feat(core) gt_transport_child_cwd` + `feat(apple) 本地 cwd 接进窗格`（远端 OSC 7 如实降级） |
| S5 | 同 S4 的 apple 提交：⌘T 继承本地目录 |
| 附加 | `fix(apple) surface 释放搬回主线程`（修 SSH e2e 的 `pthread_join DEADLK` flake） |

闸门（收口时）：`gate.sh` PASS；`terminal-gate` PASS；`mutation-check-terminal`
7/7；`mutation-check-tabs` **25/25**（T1~T14）；`apple-app-gate` **151 tests,
0 failed**。能力矩阵与 roadmap 已按 S4 的诚实口径更新（本地 cwd ✅、远端待上游）。

## 8. 提交序列（Conventional Commits，中文）

1. `feat(core): gt_transport_child_state 前台判定（三态，SSH/ConPTY 报 UNKNOWN）` + 测试 + 探针
2. `feat(apple): 关标签页只在有前台作业/在途传输时确认；关窗口同样只在该拦时拦`（含 T2/T9/T10）
3. `refactor(apple): TabKind 统一为 RemoteTarget、删除 instance 与 sshTarget`（纯重构，无行为变化）
4. `feat(apple): SFTP 每次新开；⌘T 沿用当前标签页类型`（含 T6/T8/T11/T12）
5. `feat(core,apple): cwd 地基——本地内核查询 + 远端 OSC 7（plumbing）`（含 terminal 变异）
6. `feat(apple): ⌘T 顺带继承本地目录`
7. `docs(reference): roadmap 记 SSH↔SFTP 跳转与 R2；能力矩阵补两行；计划文档入库`
