# Android 宿主

状态（2026-10-29）：**A0 原生工程与构建基础已实现**。Compose 宿主、Core JNI、VT JNI、arm64 debug APK 和设备冒烟测试已有代码；当前首页为构建验证空态，设置页显示真实组件版本。**主机管理、交互终端、SSH/SFTP、文件与完整设置尚未接入**，没有 Android CI。

- [最终交互原型](../docs/design/android-prototype.html)：主机首页、终端、文件与完整设置，单一 HTML。
- [详细实现计划](../docs/reference/android-implementation.md)：模块/线程、配置与认证、输入法、复制、设置默认值、A0–A10 与验收。
- [跨端路线图](../docs/reference/roadmap.md)、[能力矩阵](../docs/reference/capability-matrix.md)。

## 构建

锁定 JDK 17、Gradle 8.13（wrapper 验 SHA256）、AGP 8.13.2、Kotlin 2.2.21、Compose BOM 2025.10.01、NDK 28.2.13676358、CMake 3.22.1、Zig 0.16.0。compile/target API 36，min API 29，当前只构建 arm64-v8a。API 29 是声明下限，尚无该版本运行证据。

准备 JDK 和 Android command-line tools 后，设置 `JAVA_HOME`、`ANDROID_HOME`；SDK 安装项：

```bash
sdkmanager 'platform-tools' 'platforms;android-36' 'build-tools;35.0.0' \
  'ndk;28.2.13676358' 'cmake;3.22.1'
export ZIG=/path/to/zig-0.16.0/zig
bash scripts/build-android.sh
```

命令在仓库根执行。若未设置环境变量，脚本也识别首次实施使用的 `build/android-toolchain/sdk` 和本地 JDK；CI 应显式配置。依赖源码下载与编译落到工程 `tmp/android-native/`，发布到忽略的 `build/android-native/`。首次需要网络：OpenSSL 3.5.4、libssh2 1.11.1 下载先验证锁定 SHA256；Ghostty 使用仓库 pin，不改 vendor。后续按构建输入和产物哈希复用缓存。

Core CMake 交叉构建要求显式 Android libssh2 target，禁止走宿主 pkg-config；宿主测试默认只在宿主构建注册。APK 包含 native 库与对应许可，不含运行环境包。产物：

```text
android/app/build/outputs/apk/debug/app-debug.apk
android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
```

## 验证

连接 arm64 设备/模拟器后，明确指定序列号：

```bash
export ANDROID_SERIAL=emulator-5554
bash scripts/build-android.sh :app:connectedDebugAndroidTest
python3 scripts/check-abi-android.py
python3 scripts/mutation-check-android-abi.py
python3 scripts/mutation-check-android-native.py
bash scripts/gate.sh
```

JNI ABI 当前引用 4 项、其余 67 项在 `bridge/deferred-abi.json` 按阶段登记；不能把未绑定能力称为已实现。变异脚本会在指定设备重新安装 debug APK，仅用于测试设备。原生变异分别破坏 UTF-8 配置往返、NUL 拒绝与 VT feed，目标断言红、同一 APK 独立对照绿、恢复后绿并校验源码 SHA256；中断会恢复带标记的源码。

本轮证据：API 36 / arm64 / 4 KB 页模拟器，3 项设备测试、3 组 JNI 变异、3 组 ABI 对拍变异通过；首页与关于内容可启动。真机、API 29、16 KB 页运行、性能与完整 UI 验收留在后续阶段，不将 ELF 对齐等同于设备验证。

## 边界

UI 经 ViewModel/controller/repository 使用桥；不直接调用 JNI。终端实现位于 [terminal/android](../terminal/android/README.md)，使用 libghostty-vt + 自绘，不引用 Apple 代码。JNI 字符串经标准 UTF-8 byte array 传递，拒绝内嵌 NUL；终端输出数据通路将在 A2 建立，禁止通过 Compose/String/Flow 搬运。

普通 SSH 为主要远端入口。本地 A2 验证系统 `/system/bin/sh` 与 PTY，不把它称为随 App 内置的 shell。Termux/proot 环境包独立延后，按需下载且不进 APK。
