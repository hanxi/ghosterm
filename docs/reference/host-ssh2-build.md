# 主机 libssh2 构建与 SFTP 打开状态

核对日期：2026-10-30；已提交基线为 `40f28c5`，下述主机构建、controlled API 和新增守卫来自已有未提交工作区。**构建接线已存在，切片尚未完成验收**：默认库的协议与接口变异通过，新依赖守卫变异失败。本次文档审计不提交或修改这些实现。

## 构建依赖的实际来源

| 路径 | 当前工作区行为 | 边界 |
|---|---|---|
| `scripts/build-host-ssh2.py` | 校验 libssh2 1.11.1 源码包，在 `tmp/host-ssh2` 解包并应用两份 SFTP 补丁，安装到 `build/host-native` | 默认不下载；首次缺源码需显式 `--fetch`。依赖 pkg-config 可发现的 OpenSSL libcrypto |
| `core/cmake/HostSSH2.cmake` | 默认 SSH 构建校验收据并链接上述共享库 | 缺失或校验失败直接停止；不再查找系统 libssh2 或自动降级 |
| 两个 Apple `Package.swift` | 从仓库 `build/host-native/lib` 链接 libssh2 | SwiftPM 不负责准备库；CMake 自定义 prefix 不会自动同步到 SwiftPM |
| `scripts/build-core.sh` / `scripts/gate.sh` | 前者先准备/验证本地依赖；后者只配置、构建和验证现有依赖 | gate 保持离线；缓存不足时应先准备依赖 |
| `scripts/package-mac.sh` | 嵌入 libssh2 及递归依赖，放入收据和 libssh2 许可，再验证包内代码/数据节与已校验库一致 | 改写加载路径和签名会改变文件摘要，因此包校验比较 Mach-O 文件节；不代表所有新守卫已验收 |
| `.github/workflows/macos-package.yml` | 已有准备锁定依赖的 `--fetch` 步骤 | 是工作区配置，不是 CI 已运行成功的证据；现有 20 份变异范围未增加 |
| Android | 继续用 NDK 构建的显式 libssh2 target，应用同一组补丁 | 不使用主机共享库；§9.45 的设备证据不替代主机或产品接线验收 |

收据记录源码、补丁、构建器、动态/静态库、三个公开头及 libssh2 许可的 SHA256，并核对实际 libcrypto 摘要。构建缓存另比较宿主平台/架构。包内收据去掉构建机 libcrypto 路径；这不是签名证书或可自动更新的授权。

macOS 当前仍链接 Homebrew OpenSSL。自建 libssh2 不等于发行系统下限已经降低；打包仍按主程序与嵌入依赖的实际 minos 计算。未新增 Intel、macOS 13 或 Linux 宿主验证，不能由脚本分支推定这些档位已通过。许可新增落盘事实仅为 `Contents/Resources/licenses/libssh2.txt`，不能代替全部组件的许可盘点。

## 准备与验证命令

以下命令在仓库根运行，临时内容只使用工程 `tmp/`。准备 C 编译器、CMake ≥3.19、Python 3.12+、pkg-config、OpenSSL 3；macOS 另需完整 Xcode。Python 下限保证构建器使用的 `tarfile.extractall(filter='data')` 可用。本机若 pkg-config 无法发现 libcrypto，先将实际 OpenSSL 安装目录的 `lib/pkgconfig` 加入 `PKG_CONFIG_PATH`。

```bash
# macOS 工具依赖（已有则省略）
brew install cmake pkg-config openssl@3

# 首次准备允许下载；缓存源码/产物齐备后不必加 --fetch
python3 scripts/build-host-ssh2.py --fetch
bash scripts/build-core.sh
bash scripts/gate.sh
bash scripts/check-abi-apple.sh
python3 scripts/check-abi-android.py

# 真实 SSH 与故障协议端的测试环境，准备步骤可能联网
bash scripts/bootstrap-ssh-channel-tests.sh
PATH="$PWD/build/ssh-channel-toolchain/bin:$PATH" \
  bash core/tests/scripts/ssh_e2e.sh "$PWD/scripts/test-sftp-open.py"
python3 scripts/mutation-check-sftp-open.py

# 正常包验证与新增依赖守卫分开运行；图标工具依赖见 Apple README
bash scripts/package-mac.sh --out "$PWD/tmp/host-ssh2-package" --app-only --no-zip
python3 scripts/mutation-check-host-ssh2.py --app tmp/host-ssh2-package/Ghosterm.app
```

最后一条目前有已知失败，不能把该命令清单理解为全部已经通过。`--fetch` 只用于依赖准备；协议测试还要求真实 sshd/ssh-keygen 与已准备的测试 Python 环境。

只有明确需要验证无 SSH 能力时，另建隔离目录：

```bash
cmake -S core -B tmp/core-without-ssh -DGHOSTERM_WITH_LIBSSH2=OFF
cmake --build tmp/core-without-ssh
ctest --test-dir tmp/core-without-ssh --output-on-failure
```

这档显式返回 `GT_ERR_UNSUPPORTED`，不覆盖 SSH/SFTP 功能，也不用于 Apple 产品链接。不要复用默认 Core 构建目录后误把旧缓存当成当前默认配置。

## 受控打开契约与调用方

`gt_ssh_sftp_controlled` 接受由调用方独占的已认证 SSH、剩余预算、取消回调和 status。零预算非法；预先取消在协议 I/O 前返回；开始 I/O 后超时、取消或打开失败会中止该专用 SSH，调用方不能再拿它承载其他通道。正常打开恢复原 blocking/timeout 设置。连接/协议失败保留 IO 语义，不能伪装成“不支持 SFTP”。

| 调用层 | 当前实现 | 仍需实现/验收 |
|---|---|---|
| Core | 非阻塞 init 与 EAGAIN 等待中检查 deadline/取消；补丁修复库内部 EOF 循环与未完成 init 的清理 | 新依赖守卫完成有牙验证；后续 SFTP 操作另立取消/预算契约 |
| Swift 绑定 | `SSHSession.sftp(timeoutMilliseconds:cancellation:)` 已存在，定向/变异有通过记录 | macOS 产品工厂仍调用无参 `ssh.sftp()`；原认证预算未传入文件 owner |
| Android native | `gaj_ssh_open_sftp` 从原认证取剩余预算并封顶 5000 ms，接停止信号 | helper 无文件调用方；没有文件 owner、JNI/Kotlin 文件 API 或远端浏览/预览 |

公开 ABI 工作区为 82 项，Apple 引用 80/登记未用 2，JNI 引用 40/登记未用 42。这些数量包含 helper 引用，不是产品可用功能数量。列表、realpath、预览、任务和关闭仍按各自旧契约运行；opener 测试不证明整个文件连接可取消。

## 已有证据与失败记录

以下是本次审计读取的已有日志；本次重新执行的检查另记在 roadmap，不将历史日志冒充本轮重跑。

| 检查 | 结果 | 日志与范围 |
|---|---|---|
| 默认 Core | 39 项 CTest / 82 ABI 通过 | `tmp/sftp-controlled-gate.log`；不等于所有协议故障已覆盖 |
| 默认补丁库的受控打开 | 17 项通过 | `tmp/sftp-controlled-protocol.log`；正常/明确拒绝，channel/subsystem/VERSION 的取消、超时、IO，部分报文取消/超时与头部/正文 EOF |
| 接口变异 | 7 C + 2 Swift + 1 无库组通过 | `tmp/sftp-controlled-mutations.log`；包含 17 项基线、目标红/独立对照绿/恢复与 SHA256 |
| 既有回归 | SFTP 8、exec 2、SSH 5 组分别通过 | `tmp/sftp-controlled-legacy-sftp.log`、`tmp/sftp-controlled-legacy-exec.log`、`tmp/sftp-controlled-legacy-ssh.log` |
| macOS 增量构建 | 通过，链接仓库补丁库 | `tmp/sftp-controlled-macos-build.log`；不是完整宿主测试 |
| macOS 本机包 | `.app`、ad-hoc 签名、自包含依赖、网格/主题冒烟与新增普通依赖校验通过 | `tmp/sftp-host-package.log`；app-only，无本轮 ZIP/DMG、发布或 CI 证据 |
| 新依赖守卫变异 | **失败**：`Mutation did not expose missing guard: source` | `tmp/host-ssh2-mutations.log`；第一个 source 组没有达到预期变异效果，后续组未完成，不能记“十组通过” |
| Android 最近完整设备 | §9.45 的 214 项通过 | API 36 arm64 / 4 KB 页模拟器；此次文档审计未重跑，不新增档位 |

守卫失败需要修正验证方法并复跑。源码显示变异目标先破坏 prefix 收据，`check_bundle` 随后还会比较整份包收据；第二道检查可能遮住被撤去的第一道守卫。这个源码解释不等于失败已经修好，也不说明实际坏库已被接受。

整片退出前还需：新守卫的目标红/独立对照绿/恢复绿与 SHA；受影响的既有打包变异及当前平台编译；CI 实际结果独立记账。两端产品接线另验原总预算、取消后停止后续请求、晚到结果隔离和 owner 释放。Android A4 远端浏览/预览与 A5/A6 复制尚待开发；macOS 配置、设置、剪贴板和传输错误处理的既有缺口继续见[路线图](roadmap.md#macos-下一步与共享缺口)。
