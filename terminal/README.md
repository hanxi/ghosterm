# terminal — 各端 TerminalSurface

各端 surface 只处理终端字节和渲染，不感知 SSH / 本地 shell。当前平台状态见[能力矩阵](../docs/reference/capability-matrix.md)，历史验收见各端 README 与实施记录。文件队列、前台服务和任务历史属于控制面，进度见[当前状态](../docs/reference/current-status.md)。

**统一接缝**（各端只实现这一件事）：

```
surface.write_output(bytes)   ← 从 transport 读到的字节送进引擎
surface.on_input(bytes)       → 用户输入/按键编码后交给 transport
surface.resize(cols, rows)    ← 网格尺寸变化（要同时通知 transport）
```

| 子目录 | 引擎 | 渲染 | 状态 |
|---|---|---|---|
| `apple/` | full libghostty | Metal | ✅ macOS 已接通并有网格/像素/IME 证据；iOS 未交付（见 `terminal/apple/README.md`） |
| `android/` | libghostty-vt + 自绘 | Canvas | 🟡 本地/SSH Transport→VT→Canvas、InputConnection、文本操作与键盘协议已有模拟器证据；输入后短时取帧及 debug 性能基线已验（Android 计划 §9.21）。已有实际签名 release 的模拟器运行与十分钟负载记录（§9.180）；物理键盘/布局、真机与完整长负载矩阵仍待补 |
| `linux/`（规划） | full libghostty | OpenGL 4.3 core | ❌ |
| `web/`（规划） | libghostty-vt + 自绘 | Canvas/WebGL（wasm） | ❌ |
| `harmony/`（规划） | libghostty-vt + 自绘 | ArkUI Canvas | ❌ |
| `windows/`（规划） | 待定（OpenGL on Windows 需 WGL/ANGLE） | — | ❌ |

**铁律**：surface **不得**知道背后是 SSH 还是本地 shell（铁律 #1）。

Android 当前已接主题/字体/行距、Kitty 与 Nerd Fonts、触屏输入和快捷键连发。输入面与连接无关；专注布局、菜单键盘入口与系统栏由 Android 宿主负责（§9.201–§9.204）。API 29/API 36、4 KiB/16 KiB、debug/release 的历史记录分别见[当前状态](../docs/reference/current-status.md)，不以单轮模拟器结果宣称完整物理设备矩阵通过。
