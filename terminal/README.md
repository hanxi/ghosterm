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
| `android/` | libghostty-vt + 自绘 | Canvas | 🟡 本地 Transport→VT→Canvas、InputConnection、文本操作、选区边缘滚动与 Ctrl 生命周期已交付；当前模拟器证据见 Android 计划 §9.15；真实中文 IME、完整键盘矩阵与性能仍待补 |
| `linux/`（规划） | full libghostty | OpenGL 4.3 core | ❌ |
| `web/`（规划） | libghostty-vt + 自绘 | Canvas/WebGL（wasm） | ❌ |
| `harmony/`（规划） | libghostty-vt + 自绘 | ArkUI Canvas | ❌ |
| `windows/`（规划） | 待定（OpenGL on Windows 需 WGL/ANGLE） | — | ❌ |

**铁律**：surface **不得**知道背后是 SSH 还是本地 shell（铁律 #1）。

Android 当前证据来自 API 36 arm64、4 KB 页模拟器，只覆盖当前本地交互终端切片，不代表真机、SSH 或完整 A2 验收；见 [Android 引擎说明](android/README.md)与[实现阶段](../docs/reference/android-implementation.md)。
