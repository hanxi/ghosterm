# OSC 52 标准文本剪贴板

2026-11-12 接通 macOS full libghostty 与 Android libghostty-vt 的宿主回调。
两端均由引擎解析 OSC 52 与编码查询答复，TerminalSurface 不区分本地或 SSH；
PTY 输出仍直接进入引擎，UI 只处理低频剪贴板请求。

## 当前行为

- 支持标准系统文本剪贴板（`c`，以及引擎归一化为标准剪贴板的默认目标）。
  不开放 primary/selection、二进制内容、URI 转文本或 Kitty MIME 列举。
- 写入默认允许；macOS 遵循 Ghostty `clipboard-write` 策略，策略要求询问时显示单次确认。
  Android 仅可见且有窗口焦点的终端执行写入。
- 读取默认逐次询问；拒绝不发送剪贴板文本。macOS 遵循引擎 `clipboard-read` 策略，
  使用确认时的原始快照；Android 在用户允许时读取系统文本，不持久保存授权。
- 移除宿主额外的 1 MiB 文本限制，沿用引擎与系统剪贴板自身边界。非法 UTF-8 不写入系统剪贴板；不把不支持的格式当成成功。
- macOS 的未决确认在隐藏/销毁时取消，并用请求代次隔离旧确认回调。
  Android 在切换、离页、禁用输入、隐藏与关闭时拒绝未决请求；请求 ID 隔离旧答复。
  Android owner 的同步回调使用释放 session 锁的条件等待，最多 30 秒；关闭立即唤醒，
  JNI 不调用 VT，界面保持可操作。
- 此实现不等于文件复制清单、选中自动复制、全局菜单自动路由或 Cocoa 菜单危险粘贴确认
  已完成验收。Android 原有用户粘贴确认路径保留。

## 验证入口

macOS（完整 Xcode，临时文件落在工程 `tmp/`）：

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer TMPDIR="$PWD/tmp" \
  swift test --build-system native --package-path apple/GhostermTerminal
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  python3 scripts/mutation-check-osc52.py --platform apple
```

Android：先构建并在明确指定的测试设备安装 Debug/测试 APK，运行
`com.hanxi.ghosterm.TerminalOSC52Test` 与 `TerminalOSC52ProductTest`。
变异脚本自行构建/安装到指定设备，不能指向用户日常设备或与其它构建同时执行：

```bash
ANDROID_SERIAL=测试设备 python3 scripts/mutation-check-osc52.py --platform android
```

两端各六组手工变异保护写入、读取、拒绝、隐藏取消与大文本双向传递，每组要求编译后断言红、
独立网格/Core 配置对照绿、复原绿与源码 SHA256。
日志按平台落在 `tmp/osc52-mutations/{apple,android}/`，不增加 macOS CI 二十份脚本。

## 首轮接线证据（撤销额外限额前）

Android 新建隔离模拟器 `ghosterm_osc52_36` / `emulator-5584`，API 36 arm64，
真实 PTY/VT 六项与系统剪贴板/弹窗三项共九项专项通过；最终与既有复制、粘贴确认、
URI 和 Core/VT 冒烟组成 17 项同轮通过。设备两 APK SHA256 与本地构建一致，页大小 4096。
Android Debug/测试 APK、未签名 Release/R8 及 app/bridge/data/terminal 四模块 release lint 通过。
macOS 最终完整终端 34 项通过，包含实际 NSAlert sheet 按钮、原始快照及标准菜单路径。
两端各四组变异最终均编译后正常断言红、独立对照绿、复原绿与 SHA256。
最终 Swift 源码摘要 `6f57024a737a2828e1c3f4988c41a7b2bfdf24b61fd4b31babb9f22bcec130ed`，
JNI 源码摘要 `199b34410c69fa02d5efd4f090d749d4ba247a0c010d8f3761631b4e9545d47c`。
本片五个 C 文件通过 clang-format 19.1.7，C ABI 177 个符号对拍、API 冒烟 59 项通过。
原始证据在工程 `tmp/osc52/`、`tmp/osc52-*.log` 和 `tmp/osc52-mutations/`。
不据此宣称 API 29、物理设备、厂商剪贴板、真实 SSH/tmux 或全客户端矩阵验收。

失败不计通过：首次 macOS 选择 CommandLineTools 导致缺 XCTest、测试的剪贴板快照类型推断
编译失败；Android C 指针符号编译失败；首轮设备测试把 `dd` 已创建的零字节文件误判为
已泄露数据、错误假设 BEL 查询返回 ST、未等待弹窗，修正后九项通过。
初轮变异的字符串比较失败识别/日志目录未按平台分开、权限变异导致未用参数编译失败，
以及 macOS 隐藏测试没有固定非空夹具而未杀死变异，均不能计入通过组。

初次接线时工作区同时存在插件相关修改；当时全仓格式检查命中这些修改，Core 离线闸门
为 548/643（95 项失败），不记整仓绿。普通 macOS App Release 在当前启用的 Core JS
构建配置下缺 QuickJS 链接；最终显式匹配 `GHOSTERM_CORE_TEST_PLUGIN_JS=1` 的 macOS App Release 构建通过，
不能冒充普通发行包。未发行。

## 撤销宿主 1 MiB 限制（2026-11-13）

用户确认后移除两端宿主额外的 1 MiB 剪贴板限制：macOS shim/Swift、Android VT 回调、
JNI 答复与 View 读取均不再施加该阈值。文本仍须为 UTF-8，授权、请求代次、隐藏取消
及 Android 等待超时规则保留。Android 普通键入/粘贴命令队列和选区复制的预算不属于
OSC 52 请求路径，本次不调整。

写入仍受锁定 Ghostty 的 OSC 动态捕获缓冲 8 MiB 限制，包含 Base64 等序列内容，
不能将其当作 8 MiB 原始文本；`clipboard-write-limit-bytes` 默认 64 MiB 仅适用于
Kitty OSC 5522。本片不覆盖系统剪贴板、平台 IPC、可用内存或引擎自身的边界。

新增实际长度超过 1 MiB 的 macOS 系统剪贴板写入/读取回包，以及 Android native
真实 PTY→VT→JNI 写入与 Java→JNI→VT→PTY 读取逐字节测试。Android 大文本 native
专项不宣称系统剪贴板服务必定接受同样大小；平台服务拒绝仍显示错误。

首轮 Android 新模拟器专项暴露旧启动就绪判据可能匹配命令回显，导致文件尚未落地即
读取结果；已收紧为清屏后的首行精确匹配。首轮失败日志保留，不计通过。

收紧启动判据后仍有大文本文件偏短（已收前缀一致）。临时增加完整计数接收器后仍能
复现，最终定位为后续完成判据也会匹配 shell 回显中的命令文本，从而过早读取仍在
写入的结果文件。全部完成标记改为清屏定位、首行精确匹配后，恢复普通 `head` 采集，
仍按完整预期字节逐一比较；临时接收器源码已撤回。期间一次组合被重装 APK 中断、
一次标记转义错误均单独保留，不计通过。早先单项及十次独立复验通过不能代替最终组合。

最终证据：macOS 完整终端 **35 项**（含真实系统剪贴板超过 1 MiB 的双向测试）与普通
App Release 构建通过；Android 最终同轮 **18 项**通过，包含大文本 native 全字节传递、
系统剪贴板/单次授权、既有复制/粘贴/URI 和 Core/VT 冒烟。两端各六组变异在最终生产
源码上均编译后正常断言红、独立对照绿、复原绿与 SHA256。Android 测试完成判据收紧后
六组完整复验通过；源码摘要为 Swift `257961c1697089325340a8bdbd82957cf2497312fef1bc61cc1e11a4fb52e9f8`、
JNI `42fe850b35d5e3bbdfb778d9b33937a28c23f9f9a644e622090466b11193e9ea`。

当前普通 Core 配置的离线闸门 **474 CTest / 177 ABI** 全绿，C 格式 **214 文件**通过；
Android Debug/测试 APK、未签名 Release/R8 与四模块 release lint 通过。
对象为本轮新建 `ghosterm_osc52_size_36` / `emulator-5584`，API 36 arm64 / 4 KiB 页。
原件在 `tmp/osc52-size/`；最终组合为 `device-marker-final.log`，两端变异汇总为
`apple-mutations.log` / `android-marker-mutations-final.log`。
不将这些手工证据扩成真机、API 29、完整客户端或发行验收，不增加 CI 二十份。

最终精确完成标记下的大文本连续十次独立复验也通过（`marker-repeat-01.log` 至
`marker-repeat-10.log`），不会把仍在写入的文件当作完整回包。已安装两 APK 从指定设备
拉回并核验 SHA256，原件为 `marker-final-app.apk` / `marker-final-test.apk`，收据为
`marker-final-receipt.txt`。并行构建会更新共享 APK 输出，本轮以实际安装快照为证据，
不声称它与后来重建的共享输出相同。

## macOS 菜单粘贴后续（2026-11-17）

菜单用户粘贴现使用引擎安全检查和捕获请求；读、写、粘贴的确认类型分开，原 OSC 52 六组已复验，超过 1 MiB 的既有标准文本能力保留。新十六组、窗口查询注入与原生 sheet/本地 PTY 范围见[剪贴板说明](macos-clipboard.md)，不新增 Android 结果或扩大本文其它表示的边界。
