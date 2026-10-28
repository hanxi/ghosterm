# 原生产品会话协调器

2026-10-28：默认入口已切回普通 PTY/SSH；侧栏恢复、断开保留与启动恢复入口已退役。
下文保留待删除组件的历史行为，不能当作当前产品默认行为。见 [tmux 实施进度](optional-tmux-implementation.md)。

`NativeSessionCoordinator` 组织运行时校验、远程准备、持久化记录、窗格挂接与管理操作。本地和远程均通过 `TerminalPane.attachRecoverable` 进入同一套 Transport，不转发终端字节。

本地创建从应用运行时清单选择 helper；远程创建先用共享认证流程探测 Linux 架构、校验并上传对应预编译环境，再建立独立的 SSH 长连接。准备工作带取消票，已关闭窗格不能因迟到结果创建会话。运行时哈希与记录不一致时明确拒绝，不启动兼容适配或替代 shell。

恢复只使用原实例及会话 ID，或原实例与已获准创建的 nonce 查询。没有获准创建的记录不能自动重建。应用移动或同协议 helper 路径更新时，先持久化新路径，再恢复原实例；远端重新检查安装包，仍使用记录中的原 runtime 目录。

显式关闭先撤销 Transport，排空其所有者队列，再读取持久化记录。这样不依赖可能落后于磁盘的界面记录，避免漏掉刚获准的 CREATE。后续终止由独立管理 Transport 执行，协调器保持存活直到操作结束；关闭窗格、销毁视图后仍可完成。未知结果不会自动重发。查看到终止状态与管理操作完成是两个时点，管理结束前禁止遗忘记录。

断开保留只撤销 attachment，记录重新出现在恢复清单。遗忘只删除本地元数据，不声称停止远端进程或释放后端保留状态。应用退出及视图销毁继续保持 detach 语义。

真实本地测试覆盖 shell 实际 PID、启动目录、更换产品所有者后恢复原 Source 与新 Surface、旧输入失效、关闭后实际终止、所有者提前释放以及最终遗忘。隔离变异入口为 `scripts/mutation-check-session-coordinator.py`。远程底层安装、原生窗格和网络恢复证据见 `session-native-setup.md`。

默认 AppModel 的本地创建、SSH 创建及分屏现已使用该协调器。侧栏保留会话清单提供恢复、检查、结束、遗忘，标签菜单提供断开保留，错误在窗口中显示。启动扫描仅恢复已确认的会话，未确认创建与停止未知不会自动重建或重发；后台 Source 的目录与作业元数据已经接线。

2026-10-21 默认入口证据：macOS 全包 293 项（8 项专用环境跳过）通过；另在 Mac→Linux aarch64 原生与 x86_64 QEMU 运行实际 RootView 的安装、启动恢复原 PID/目录与显式停止。aarch64 默认入口另通过 TCP reset 与静默黑洞、旧 upstream 租约、重新认证及新 Surface 恢复。本地 daemon 与 Linux aarch64 远端的实际 GUI 进程均另经 SIGKILL 后重新启动：完整 RootView 挂载、新随机标记命令获得新响应、原 session/instance/PID/目录一致，随后显式结束；此项仍不表示完整 P0 资源预算通过。

记录扫描、路径更新和遗忘在专用队列遇到短暂锁竞争时有界重试四秒；主线程不等锁。准备阶段即安装关闭/停止取消票，迟到的 catalog 或安装结果不能再发布窗格。应用层启动一次共享引擎主循环，以接收低频标题、焦点和终端事件；输出字节仍直接在 I/O 侧送入 Surface。

远端新建 Source 通过 `/bin/sh -c 'exec "$SHELL" -l'` 启动 SSH 环境中的用户登录 shell；
引导 shell 被 exec 替换，权威 PID 不变。执行路径与参数仍走现有 CREATE argv，
没有新增协议字段或旧版本适配。恢复原 Source 不重新运行这条命令。专用 Linux
账户为 `/bin/bash`，实际 `$0` 与 `shopt -q login_shell` 均已验证；退回固定 `/bin/sh`
的隔离变异会触发普通 XCTest 失败，同变异的主机配置读取对照仍绿。入口为
旧的 session-login 变异随默认入口退役；普通登录 shell 仍由 login-shell 闸门验证。
