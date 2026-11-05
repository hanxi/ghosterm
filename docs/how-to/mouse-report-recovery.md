# 自动清理 shell 提示符处的鼠标上报残留

适用现象：tmux 里的程序开启鼠标上报后异常退出，shell 重新接管输入，但 tmux 的
`mouse_any_flag` / `mouse_sgr_flag` 仍为 1。移动鼠标时，合法的上报序列被 shell
当作文本输入；tmux 前缀键仍可用，detach / attach 甚至换一个终端连接也不能清理
该窗格的残留。这个机制已用 Ghosterm 的真实 libghostty、PTY 和 tmux 复现。
它不证明所有休眠后的同类症状都出自这个原因。

只在点击时出现 `0;131;34M0;131;34m` 一类文字，也属于需要检查的鼠标上报泄漏：
按下与松开使用不同的结尾。只报告点击的模式可以在完全不报告移动时仍然开启，
因此“移动正常”不能作为点击恢复的验收结果。

恢复集成在 **Bash/Zsh 即将显示下一次提示符时**关闭鼠标上报及其编码模式，不清屏、
不丢弃输入、不改键盘协议，也不在 TUI 运行时重置状态。它保留现有提示符钩子和命令
退出码，可重复加载。正常运行的 tmux / TUI 仍可按需重新开启鼠标功能。

## macOS 本地 zsh 自动恢复

Ghosterm 的普通本地 zsh 入口现在自动加载随包提示符钩子。更新并重启 App 后，
新建本地终端即可生效，无需编辑 `.zshrc` 或每次手动 `source`。这解决的是本地
执行 `ssh → ssh → ssh → tmux`、SSH 异常断开后回到本地提示符仍泄漏点击序列的路径。

自动接线在用户启动配置执行完、第一次准备读取输入时安装。用户 `.zshenv` 只读一次，
`ZDOTDIR` 在其它启动文件运行前恢复，历史记录仍在用户原来的位置；已有手工钩子
可重复加载。`.direct` 命令、带自定义启动参数的入口（仅 `-i` 除外）、其它 shell 及远端 shell 不使用
此自动接线；Bash 和远端 zsh 继续使用下面的独立安装器。此钩子只清鼠标模式，
不提供 OSC 133、远端 cwd 或自动重连。

自动接线不修改用户启动文件，不向正在运行的 shell 注入命令，也不在唤醒时重置
前台 TUI。已有旧 App 进程中的窗口不会因磁盘上的 App 更新而获得新启动接线。

## 远端与手工安装

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

对 `ssh → ssh → ssh → tmux`，最内层 tmux 窗格的 shell 需要加载钩子；如果某层
SSH 断开后回到了中间主机的提示符，接管输入的那层 shell 同样需要加载。
本机安装不会把钩子沿 SSH 链传播。若字符出现在仍运行的前台程序中，提示符钩子
不会执行，需要另行记录该程序及断线/重连状态，不能据此宣称已经修复。

在**确认已经回到普通 shell 提示符**时，可先用 Ctrl+C 取消当前行的残留输入，
再执行一次下面的命令清理鼠标模式；这只是当前状态恢复，不代替安装或根因修复：

```sh
printf '\033[?9l\033[?1000l\033[?1002l\033[?1003l\033[?1005l\033[?1006l\033[?1015l\033[?1016l'
```

本地已经安装、但问题发生在一直开着的旧 zsh 时，可在上述提示符中键入：

```sh
source ~/.config/ghosterm/shell-integration/mouse-recovery.zsh
```

这会给**当前** shell 加载钩子，并在返回提示符时清理模式。可用
`type __ghosterm_mouse_prompt` 和 `typeset -p precmd_functions` 检查当前 shell 的实际加载状态；
启动文件里有加载块，只说明以后启动的 shell 有安装入口。

新建 shell / tmux 窗格会自动加载。安装前已经运行的 shell，需要重新加载其启动文件
一次才能获得钩子；之后异常退出回到提示符时自动恢复，无需每次输入 `reset`。
若程序仍在前台运行，则不会触发提示符钩子，不能把这项修复宣称为该情况的通用修复。

## 验证

```sh
python3 scripts/test-shell-mouse-recovery.py --mutate --codex --ssh
# 点击模式单独验证，经过三层实际 SSH PTY：
python3 scripts/test-shell-mouse-recovery.py --mutate --codex --ssh --ssh-hops 3 --mouse-mode clicks
# 中断真实 SSH 连接，检查回到本地 Bash/Zsh 提示符：
python3 scripts/test-shell-mouse-recovery.py --ssh --ssh-hops 3 --mouse-mode clicks --scenario disconnect
# 不安装用户脚本，验证生产 zsh 自动启动及四组隔离变异：
python3 scripts/test-shell-mouse-recovery.py --ssh --ssh-hops 3 --mouse-mode clicks --scenario disconnect --automatic --mutate
# 对实际打出的 .app 检查加载、清理、资源缺失/损坏及同包独立对照：
python3 scripts/test-shell-mouse-package.py --app tmp/Ghosterm.app
```

全部夹具在工程 `tmp/`，独立 tmux socket、独立 loopback sshd，不改用户启动文件，
也不操作用户现有 tmux。合成的 AppKit 鼠标事件经生产链路 `TerminalHostView → libghostty →
SessionController → PTY → tmux`；`--ssh` 在这条路径中另插入系统 SSH。
测试先复现序列文本输入，再加载交付的 shell 脚本，验证自动清理、重连、新 surface、
正常 TUI 的真实鼠标接收及前缀键。测试同时保护重复加载、原提示符钩子、退出码和
启动文件备份。变异在隔离副本进行，要求目标红、同变异独立对照绿、恢复绿和 SHA256。

点击检查经真实 AppKit 按下/松开处理，泄漏判据同时识别移动与点击序列；正常 TUI
对照必须实际收到按下与松开的原始字节。`--mouse-mode clicks` 只开启 DEC 1000，
在程序异常退出后先断言移动没有泄漏，再复现点击泄漏及提示符清理。
`--ssh-hops 3` 在同一个隔离 loopback sshd 上建立三层 SSH/PTY，验证嵌套转发，
不代表三个不同服务器、网络中断退回中间 shell 或真实整机休眠已经验收。

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

## 点击与三层 SSH 补验（2026-11-05）

上轮探针只发送移动事件，未覆盖点击，不能把上轮通过当作点击已经修复。
本轮在 macOS 26.6.2 / tmux 3.6a 上补发真实 AppKit 按下/松开事件；默认移动模式
的 Bash/Zsh 本地回归通过。单独点击模式经本地及三层 loopback SSH 的 Bash/Zsh
均先复现“移动无泄漏、点击泄漏”，再验证交付钩子清理、重连和新 surface；
正常 TUI 实际收到按下/松开字节。既有八组变异与离线 `scripts/gate.sh` 通过。

Codex CLI 0.166.0 在本地和三层 loopback SSH 中，点击、合成唤醒、重连及草稿继续
输入均通过，未复现其输入框泄漏，不记为该程序的缺陷修复。初轮只补探针与说明，
生产鼠标处理和提示符钩子未改；真实多主机、SSH 中断退回中间 shell 及硬件休眠
仍未验收，用户报告的实际休眠问题尚不能标为已解决。

用户随后明确：SSH 已断开，字符出现在本地 shell。`--scenario disconnect` 因此将
钩子只加载到本地 shell；在远端 TUI 实际收到按下/松开字节后，终止隔离 sshd 的
外层连接进程，等待本地前台进程组及提示符恢复。三层 SSH 下 Bash/Zsh 各自的
未加载/已加载四项通过：未加载先复现点击泄漏，已加载自动清理；未加载的旧 shell
现场键入 `source` 后也恢复，原本地 PID 与后续命令执行均保持。

探针初版把恢复命令按粘贴语义发送，遇到断开后残留的 bracketed-paste 模式而失败；
另一次把提示符限定在网格末行，遗漏了断线后仍留有远端画面的情况。改为取消残留
输入、按键入语义发命令，以及前台进程组/提示符联合判定后重跑通过。这些初版失败
不计作通过，也未据此扩展生产鼠标钩子到其它终端模式。真实休眠和用户旧窗口的
实际钩子状态仍待核对；没有向用户正在运行的 shell 注入命令。

## 本地 zsh 自动接线与随包验证（2026-11-05）

用户明确要求先修点击问题后，生产本地 zsh 启动已接随包恢复钩子，使用现有 Core
PTY 环境参数，未改 C ABI、Transport 字节链或终端引擎。资源位于 AppModel 资源
bundle；打包时对拍独立脚本原件，且实际运行包内二进制验证提示符清理。
App 必须使用自身资源，不允许 SwiftPM 的开发目录回落掩盖包内文件缺失。

三层实际 loopback SSH 断线自动恢复通过，正常远端 TUI 确实收到点击按下/松开。
五项真实 zsh 启动组合覆盖未设/自定义/启动中改动 ZDOTDIR、非登录交互与用户重设
`precmd_functions`；验证启动顺序、退出码、历史路径、环境恢复、旧手工钩子去重及
用户文件不变。自动接线四组变异分别护调用、`.direct` 排除、清理动作及用户启动文件
转发，实际构建后正常断言红、同变异 TUI 对照绿、复原绿与 SHA256。实际 `.app` 的
四组变异护资源目录缺失、bootstrap 缺失、钩子未加载及清理动作缺失，正常包内检查红、
同包配置检查绿、恢复绿与资源 SHA256。既有八组独立脚本变异也通过。

Core 52 项 / 95 ABI、Apple 引用 91 / 显式未用 4、31 项宿主定向回归、默认入口四组
变异、release 原生打包与既有打包 63 项 / DMG 三组变异通过，均为分轮证据；没有
重跑完整宿主套件或增加 CI 20 份覆盖。测试包为本地 ad-hoc，不记作云端发行。

初版重定向多个启动文件导致系统默认历史路径落到资源目录，已撤回并改成仅在
`.zshenv` 恢复原目录后延迟加载；历史路径和资源文件集合均有断言。初版包内资源
缺失变异还发现 SwiftPM 会借开发目录加载成功，已改为 App 严格只取自身资源后复验。
尝试系统 Bash 3.2 的自动 ENV 启动未生效，未纳入生产；Bash 仍按手工集成记账。
Xcode 编译的复杂类型推断失败已拆分表达式修正；工具链失败未计作变异目标红。

本片为本机 macOS 26.6.2 / tmux 3.6a 的隔离连接及原生包证据，未做用户真实硬件
休眠复测，不扩大为仍在前台运行的程序、全部 shell 或全远端环境已解决。
