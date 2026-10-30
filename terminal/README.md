# terminal — 各端 TerminalSurface

**统一接缝**（各端只实现这一件事）：

```
surface.write_output(bytes)   ← 从 transport 读到的字节送进引擎
surface.on_input(bytes)       → 用户输入/按键编码后交给 transport
surface.resize(cols, rows)    ← 网格尺寸变化（要同时通知 transport）
```

| 子目录 | 引擎 | 渲染 | 状态 |
|---|---|---|---|
| `apple/` | full libghostty | Metal | ✅ macOS 已接通并有网格/像素/IME 证据；iOS 未交付（见 `terminal/apple/README.md`） |
| `android/` | libghostty-vt + 自绘 | Canvas | 🟡 本地 Transport→VT→Canvas、InputConnection、文本操作、选区边缘滚动与 Ctrl 生命周期已交付；Ctrl 标点、Gboard 拼音基本路径及按键重复/释放已有模拟器证据（Android 计划 §9.16–§9.17）；完整软硬键盘矩阵与性能仍待补 |
| `linux/`（规划） | full libghostty | OpenGL 4.3 core | ❌ |
| `web/`（规划） | libghostty-vt + 自绘 | Canvas/WebGL（wasm） | ❌ |
| `harmony/`（规划） | libghostty-vt + 自绘 | ArkUI Canvas | ❌ |
| `windows/`（规划） | 待定（OpenGL on Windows 需 WGL/ANGLE） | — | ❌ |

**铁律**：surface **不得**知道背后是 SSH 还是本地 shell（铁律 #1）。

静止画面的闪烁文字已修复：隐藏光标且没有新输出时仍自动重绘，普通文字保持可见。该切片完整设备测试 **100 项全通过**，五组渲染变异通过，证据见[§9.19](../docs/reference/android-implementation.md#919-静止画面的闪烁文本2026-10-30)；不替代 A2 性能与真机验收。

Android 当前证据来自 API 36 arm64、4 KB 页模拟器，已验本地终端及后续 SSH native owner→Transport→VT 路径（§9.25，完整 113 项设备测试），产品页面/容器随后已有 27 项定向测试与 15 组产品变异证据（§9.27）；修正本地页面测试判据后完整 130 项回归通过，仍不代表真机或完整 A2/A3 验收；见 [Android 引擎说明](android/README.md)与[实现阶段](../docs/reference/android-implementation.md)。

修饰键、数字键盘与组合音符扩展已通过完整 99 项设备测试及 24 组选组变异，详见实现计划 §9.18；已有模拟器 debug 性能基线（§9.21）；实际物理设备、更多布局、release 与长负载性能仍未完成，A2 保持部分完成。

以上完整 130 项为 SSH 产品首片历史结果；SAF 导入随后通过完整 135 项设备测试、五项定向与七组变异，见[§9.29](../docs/reference/android-implementation.md#929-saf-私钥导入生命周期2026-10-30)。当前仍限 API 36 arm64 / 4 KB 页模拟器，整个 A2/A3 未完成。macOS 的 `4924a2d` 另修复 PTY 子进程信号屏蔽，使尺寸通知到达 ssh/tmux；这属于 Transport 子进程边界，不在 surface 中加入 SSH/tmux 分支。
