# 计划：SFTP → 统一文件浏览器（M4.6）

> 决策记录 + 进度。**分阶段切片**，每片独立提交、闸门绿才提交（AGENTS.md）。
> 状态：**S1 ✅ / S2 ✅ / S3 ✅ / S4 ✅ / S5 ✅ / S6（macOS）✅ / S6-iOS ⏳ / S7 ✅（文档收口）**。
> 相关：`roadmap.md`（M4.6）、`capability-matrix.md`（SFTP / exec / files 行）。

## 目标形态（已定的设计）

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
- **远端加速走 exec 通道**，不装常驻守护进程：SFTP v3 的 readdir 是**每条目
  一次往返**，大目录/递归搜索慢到不可用；远端 `find`/`grep` 一次往返即可。
  服务器禁 exec → `GT_ERR_UNSUPPORTED`，**显式回退** SFTP walk（档位在 UI 标注），
  绝不静默降级（铁律 #6）。不做 vscode-server 式常驻代理（另一套安全模型）。
- **单会话串行**：每会话一把 I/O 锁 + 单条 I/O worker 线程驱动 SFTP job；
  请求/响应在锁内完成，传输分块推进、块间释放锁——浏览与传输交替推进。
  并发 v1 = 1（正确性优先）。
- **不做**：队列跨启动持久化；远程↔远程传输；v1 不创建符号链接；权限框 v1
  只做 chmod 模式位（uid/gid 仅只读数字）。

## 切片与进度

### 性能证据（exec vs walk）
e2e 里造 300 文件宽目录，对比「远端 exec `find`」与「SFTP 逐层 walk」：两条路
都找全；**环回上 exec 反而慢（~40ms vs ~3ms，起远端进程的开销）**，exec 的
优势只在**真实 RTT** 链路显现（walk 每条目一次 RTT × 条目数）。故只断言正确性，
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
在 `GhostermTerminal` 就因 AppKit 失败。前置为独立上游里程碑。见 roadmap S6-iOS。

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
