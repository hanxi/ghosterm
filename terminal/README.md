# terminal — 各端 TerminalSurface

**当前实现（2026-10-31，工作区 §9.71）**：Android 已有复制清单、粘贴预检/冲突确认、任务认证与逐项结果页；`FileWorkRuntime` 在进程内共享任务、清单及任务认证，页面只持有浏览和导航状态。关闭 Activity 不取消文件任务，全进程只接受一个活动请求，取消后等待实际资源释放。排队、前台服务/通知、暂停/重试、进程恢复与完整偏好尚未实现。不同远端命名空间关系未知时阻断，不能据同服务器测试宣称跨主机复制已完成。8 项跨 Activity 定向与 15 组进程所有权变异通过；完整回归仍以 §9.70 为最近已完成记录，当前验收见[实现记录](../docs/reference/android-implementation.md#971-a6-进程任务与共享清单2026-10-31验收中)。

**最近完整验收（§9.70，不覆盖 §9.71 工作区变更）**：§9.70 的 18 项产品定向、24 组新增变异及既有 97 组回归通过；最终完整设备 **437 项通过，零失败/错误/跳过**（1105.73 秒），仅 API 36 arm64 / 4 KB 页模拟器。714 份非 Markdown 源码摘要未变，安装主/测试 APK 与构建一致。Core 47 CTest / 89 ABI、Apple 87/2、JNI 49/40 和两端原生构建通过；未新增真机、API 29、16 KB 页、发行或 CI 证据。

**统一接缝**（各端只实现这一件事）：

```
surface.write_output(bytes)   ← 从 transport 读到的字节送进引擎
surface.on_input(bytes)       → 用户输入/按键编码后交给 transport
surface.resize(cols, rows)    ← 网格尺寸变化（要同时通知 transport）
```

| 子目录 | 引擎 | 渲染 | 状态 |
|---|---|---|---|
| `apple/` | full libghostty | Metal | ✅ macOS 已接通并有网格/像素/IME 证据；iOS 未交付（见 `terminal/apple/README.md`） |
| `android/` | libghostty-vt + 自绘 | Canvas | 🟡 本地/SSH Transport→VT→Canvas、InputConnection、文本操作与键盘协议已有模拟器证据；输入后短时取帧及 debug 性能基线已验（Android 计划 §9.21）。物理键盘/布局矩阵、真机/release 和长时间负载性能仍待补 |
| `linux/`（规划） | full libghostty | OpenGL 4.3 core | ❌ |
| `web/`（规划） | libghostty-vt + 自绘 | Canvas/WebGL（wasm） | ❌ |
| `harmony/`（规划） | libghostty-vt + 自绘 | ArkUI Canvas | ❌ |
| `windows/`（规划） | 待定（OpenGL on Windows 需 WGL/ANGLE） | — | ❌ |

**铁律**：surface **不得**知道背后是 SSH 还是本地 shell（铁律 #1）。

最近完整设备结果见页首和 [§9.70](../docs/reference/android-implementation.md#970-a5-粘贴确认任务认证与结果页面2026-10-31)，属于对应 APK 的整套回归，不是新增终端专项；§9.49 的 214 项保留为历史。§9.44 的选区滚动偶发失败及 §9.49 中间轮的导入旋转观察失败仍保留追踪，最终复验通过不表示偶发根因已修复，文件浏览及终端往返证据见[§9.43](../docs/reference/android-implementation.md#943-a4-本机与-saf-文件浏览首片2026-10-30)；底栏已对齐会话/文件/唯一键盘，进入文件页不关闭终端 owner。通道取消切片的完整 200 项保留为历史；该切片新增 13 项协议阶段/生命周期定向与 13 组变异，见[§9.41](../docs/reference/android-implementation.md#941-ssh-通道打开预算与取消2026-10-30)。Android native owner 将认证后剩余预算与取消传入共享 controlled 入口；字节仍为 Transport→VT，不经 UI。候选推进的真实双栈 SSH 证据保留在 §9.40。当前完整套件仍要求可 root 的 API 36 arm64 / 4 KB 页模拟器及测试协议端依赖，不扩大为公网、真机或整个 A2/A3。首轮选区复制等待失败、产品变异掉线中断和 tmux SSH 失败后的调整及最终复验一并记在 §9.41；本通道切片已完成当前模拟器档验收，选区早期不稳定的根因仍未确定。下文保留各历史切片数量。

静止画面的闪烁文字已修复：隐藏光标且没有新输出时仍自动重绘，普通文字保持可见。该切片完整设备测试 **100 项全通过**，五组渲染变异通过，证据见[§9.19](../docs/reference/android-implementation.md#919-静止画面的闪烁文本2026-10-30)；不替代 A2 性能与真机验收。

Android 当前证据来自 API 36 arm64、4 KB 页模拟器，已验本地终端及后续 SSH native owner→Transport→VT 路径（§9.25，完整 113 项设备测试），产品页面/容器随后已有 27 项定向测试与 15 组产品变异证据（§9.27）；修正本地页面测试判据后完整 130 项回归通过，仍不代表真机或完整 A2/A3 验收；见 [Android 引擎说明](android/README.md)与[实现阶段](../docs/reference/android-implementation.md)。

修饰键、数字键盘与组合音符扩展已通过完整 99 项设备测试及 24 组选组变异，详见实现计划 §9.18；已有模拟器 debug 性能基线（§9.21）；实际物理设备、更多布局、release 与长负载性能仍未完成，A2 保持部分完成。

以上完整 130 项为 SSH 产品首片历史结果；SAF 导入随后通过完整 135 项设备测试、五项定向与七组变异，见[§9.29](../docs/reference/android-implementation.md#929-saf-私钥导入生命周期2026-10-30)。当前仍限 API 36 arm64 / 4 KB 页模拟器，整个 A2/A3 未完成。macOS 的 `4924a2d` 另修复 PTY 子进程信号屏蔽，使尺寸通知到达 ssh/tmux；这属于 Transport 子进程边界，不在 surface 中加入 SSH/tmux 分支。
