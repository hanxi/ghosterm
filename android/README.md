# Android 宿主

状态（2026-10-29）：**只有设计与规划，尚无原生工程**。本目录目前没有 Gradle、Manifest、Compose 页面或 JNI；不能构建 APK，也没有 Android CI。

- [最终交互原型](../docs/design/android-prototype.html)：主机首页、终端、文件与完整设置，单一 HTML。
- [详细实现计划](../docs/reference/android-implementation.md)：模块/线程、配置与认证、输入法、文件复制、设置默认值、A0–A10 与验收。
- [跨端路线图](../docs/reference/roadmap.md)、[能力矩阵](../docs/reference/capability-matrix.md)。

拟用 Kotlin + Compose 原生 UI；共享 Core 只经 `core/include/ghosterm.h` 绑定，界面经 controller/repository 操作，不直接调用 JNI。终端实现放在 [terminal/android](../terminal/android/README.md)，用 libghostty-vt + 自绘，不引用 Apple 端代码。

普通 SSH 为主要远端入口。本地首片验证系统 `/system/bin/sh` 与 PTY，**不把它称为随 App 内置的 shell**；桌面 forkpty 已验证不代表 Android Bionic/权限/生命周期已验证。Termux/proot 环境包独立延后，按需下载且不进 APK；执行限制与 PoC 要求见 [runtime-payload](../runtime-payload/README.md)。

首版主机不分组；文件点按打开、长按多选；复制清单跨本机/远端保留；终端只有一个键盘开关，新建会话位于会话面板。原型外侧的预览导航与模拟键盘不进入产品。
