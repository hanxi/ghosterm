# 自动清理 shell 提示符处的鼠标上报残留

适用现象：tmux 里的程序开启鼠标上报后异常退出，shell 重新接管输入，但 tmux 的
`mouse_any_flag` / `mouse_sgr_flag` 仍为 1。移动鼠标时，合法的上报序列被 shell
当作文本输入；tmux 前缀键仍可用，detach / attach 甚至换一个终端连接也不能清理
该窗格的残留。这个机制已用 Ghosterm 的真实 libghostty、PTY 和 tmux 复现。
它不证明所有休眠后的同类症状都出自这个原因。

恢复集成在 **Bash/Zsh 即将显示下一次提示符时**关闭鼠标上报及其编码模式，不清屏、
不丢弃输入、不改键盘协议，也不在 TUI 运行时重置状态。它保留现有提示符钩子和命令
退出码，可重复加载。正常运行的 tmux / TUI 仍可按需重新开启鼠标功能。

## 安装到实际运行 shell 的主机

安装脚本先预览目标，加 `--apply` 才写入；修改启动文件前保存逐字节备份，原文件
内容保持不变，仅追加一个加载块。脚本遇到符号链接或已有不同加载块时明确拒绝，
可用 `--rc-file` 指定实际目标。所有临时写入在检出目录的 `tmp/`，跨文件系统替换
会明确失败而不会改写启动文件。

本机 zsh：

```sh
python3 scripts/install-shell-mouse-recovery.py --shell zsh
python3 scripts/install-shell-mouse-recovery.py --shell zsh --apply
```

Bash 默认安装到 `~/.bashrc`。若登录 shell 不加载它，需用 `--rc-file` 明确指定已有的
登录启动文件，或在该文件中加载 `.bashrc`；脚本不擅自创建可能遮蔽 `.profile` 的
`.bash_profile`。自定义 `ZDOTDIR` 同样用 `--rc-file` 指定其 `.zshrc`。

远端 tmux 必须在**远端主机**安装；只改 Mac 的 `.zshrc` 不会改变已经在远端运行的
shell。可以在远端检出中运行同一脚本，或在用户可写目录中保留 `scripts/` 层次，
连同 `scripts/shell-integration/` 一起复制安装脚本后运行。此安装没有 SSH 包装器、后台服务或远端自动执行步骤。

新建 shell / tmux 窗格会自动加载。安装前已经运行的 shell，需要重新加载其启动文件
一次才能获得钩子；之后异常退出回到提示符时自动恢复，无需每次输入 `reset`。
若程序仍在前台运行，则不会触发提示符钩子，不能把这项修复宣称为该情况的通用修复。

## 验证

```sh
python3 scripts/test-shell-mouse-recovery.py --mutate --codex --ssh
```

全部夹具在工程 `tmp/`，独立 tmux socket、独立 loopback sshd，不改用户启动文件，
也不操作用户现有 tmux。合成的 AppKit 鼠标事件经生产链路 `TerminalHostView → libghostty →
SessionController → PTY → tmux`；`--ssh` 在这条路径中另插入系统 SSH。
测试先复现序列文本输入，再加载交付的 shell 脚本，验证自动清理、重连、新 surface、
正常 TUI 的真实鼠标接收及前缀键。测试同时保护重复加载、原提示符钩子、退出码和
启动文件备份。变异在隔离副本进行，要求目标红、同变异独立对照绿、恢复绿和 SHA256。

`--codex` 需要本机安装 Codex CLI：使用独立前台实例测试输入框、模拟唤醒及重连，
只输入测试草稿，不提交给模型。它验证本机版本的实际结果，不把未复现
计为“已修复”。通知注入、进程暂停/恢复均不等于整机休眠；真实机器休眠仍须单独验收。
这些是手工验证，不增加 macOS full CI 的变异覆盖数量。

卸载时移除启动文件中带 `Ghosterm automatic mouse recovery` 标记的加载块，
删除所安装的 `~/.config/ghosterm/shell-integration/mouse-recovery.*`，再启动新 shell。
备份可用于核对原内容；如果安装后又改过启动文件，勿用旧备份直接覆盖新改动。

## 本轮证据（2026-10-30）

macOS 26.6.2、tmux 3.6a：本地和系统 SSH → loopback sshd 两条路径中，Bash/Zsh
均先复现、再自动清理通过；原 shell PID、tmux 前缀键、正常 TUI 鼠标接收及新
surface 重连均验证。八组变异和离线 `scripts/gate.sh` 通过。

Codex CLI 0.164.0 的真实输入框在本地及上述 SSH 路径中，继承残留模式、模拟
唤醒和 tmux 重连后均未出现序列文本输入，草稿和继续键入正常；不记为 Codex
缺陷修复。测试要求先确认 Codex 界面确实启动，避免把 CLI 启动失败后的 shell
误当输入框。实际整机休眠和用户真实远端服务器尚未验证。
