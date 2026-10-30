# 主机 libssh2 构建与 SFTP 打开状态

**A5 远端暂存创建（2026-10-31）**：已接独占创建、源 fd 校验和分阶段结果，以及 Swift/JNI/Kotlin 绑定。44 CTest / 86 ABI、Apple 84/2、JNI 46/40；22 组 Core/无库/Swift、九组 Android 新变异及既有 owner 18 组通过，两端原生构建通过。最终目标发布、复制清单和任务队列仍待实现；macOS 文件产品尚未消费新接口。 设备范围与失败记录见 [§9.54](android-implementation.md#954-a5-远端独占暂存创建2026-10-31)。

**A5 共享前置（2026-10-31）**：传输打开顺序、覆盖截断、关闭结果与排队取消已修复；43 CTest / 85 ABI、13＋8 组变异、两端构建、Swift 1 项与 Android 22 项定向通过。非覆盖提交与复制产品仍待完成；未重跑完整设备/Apple 宿主/打包/CI。见 [§9.53](android-implementation.md#953-a5-共享传输结果前置2026-10-31)。

核对日期：2026-10-30。受控 SFTP 打开接口、主机补丁依赖与构建/打包接线已完成本机验收，见 Android 实现计划 §9.47。此前 `cef17ac` 记录的依赖守卫测试失败已修正并复验：十组直接守卫变异及一组真实 CMake 调用方变异通过。§9.49 已完成受控目录浏览与目录句柄关闭契约，§9.50 新增有界预览。§9.51 已接 Android 专用文件 owner/JNI/Kotlin 及中止后关闭；Android 远端页面已由 §9.52 接入，macOS 产品迁移仍待完成。

## 构建依赖的实际来源

| 路径 | 当前工作区行为 | 边界 |
|---|---|---|
| `scripts/build-host-ssh2.py` | 校验 libssh2 1.11.1 源码包，在 `tmp/host-ssh2` 解包并应用三份 SFTP 补丁（第三份按 §9.49 验证），安装到 `build/host-native` | 默认不下载；首次缺源码需显式 `--fetch`。依赖 pkg-config 可发现的 OpenSSL libcrypto |
| `core/cmake/HostSSH2.cmake` | 默认 SSH 构建校验收据并链接上述共享库 | 缺失或校验失败直接停止；不再查找系统 libssh2 或自动降级 |
| 两个 Apple `Package.swift` | 从仓库 `build/host-native/lib` 链接 libssh2 | SwiftPM 不负责准备库；CMake 自定义 prefix 不会自动同步到 SwiftPM |
| `scripts/build-core.sh` / `scripts/gate.sh` | 前者先准备/验证本地依赖；后者只配置、构建和验证现有依赖 | gate 保持离线；缓存不足时应先准备依赖 |
| `scripts/package-mac.sh` | 嵌入 libssh2 及递归依赖，放入收据和 libssh2 许可，再验证包内代码/数据节与已校验库一致 | 改写加载路径和签名会改变文件摘要，因此包校验比较 Mach-O 文件节；十一组依赖守卫变异另有通过证据 |
| `.github/workflows/macos-package.yml` | 已有准备锁定依赖的 `--fetch` 步骤 | 步骤已接线，尚无本轮 CI 成功证据；现有 20 份变异范围未增加 |
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

上述命令对应本机验收入口，CI 与其他平台分别记账。`--fetch` 只用于依赖准备；协议测试还要求真实 sshd/ssh-keygen 与已准备的测试 Python 环境。

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
| Core | 非阻塞 init 与 EAGAIN 等待中检查 deadline/取消；补丁修复库内部 EOF 循环与未完成 init 的清理 | 后续 SFTP 操作另立取消/预算契约；当前打开验证不扩大为整条连接有界 |
| Swift 绑定 | `SSHSession.sftp(timeoutMilliseconds:cancellation:)` 已存在，定向/变异有通过记录 | macOS 产品工厂仍调用无参 `ssh.sftp()`；原认证预算未传入文件 owner |
| Android native | `files_jni.c` 的专用 owner 调用 `gaj_ssh_open_sftp`，从原认证取剩余预算并封顶 5000 ms；目录/预览各有请求预算和停止信号 | `RemoteFileSession` 后端与远端 repository、认证 UI、文件页已按 §9.51/§9.52 接入；复制与完整矩阵待补 |

§9.47 已验收基线为 82 ABI、Apple 80/2、JNI 40/42；§9.49 受控目录浏览为 83 ABI、Apple 81/2、JNI 40/43。§9.51 为 85 ABI、Apple 83/2、JNI 45/40；§9.54 为 86 ABI、Apple 84/2、JNI 46/40；§9.56 当前工作区为 87 ABI、Apple 85/2、JNI 47/40，新增受控读取到 fd。Android 后端已消费受控接口，新增读取专项通过但完整设备运行有 1 项失败，整片未验收。这些数量不是产品可用功能数量。macOS 文件产品的列表、realpath、预览、任务和关闭仍按原契约运行；opener 测试不证明整个文件连接可取消。

## 受控目录浏览与产品迁移

当前新增 `gt_sftp_browse_controlled` 与 Swift `SFTPSession.browse`，将 realpath、完整列目录及 closedir 纳入同一次预算/取消检查。调用方必须独占已认证的 SSH/SFTP，不能与终端、其他通道或传输 job 共用；既有 job 记录也会被拒绝。成功才发布规范路径和全部条目；非法参数/预先取消不启动 I/O，开始 I/O 后的失败会中止专用 SSH，必须依次释放 SFTP/SSH 后重新连接。

预算与条目上限必须大于零，条目上限不超过 65536；路径/输出缓冲限制见 `core/include/ghosterm/sftp.h`。此接口只关闭本次目录句柄，不替调用方关闭 SFTP 会话。旧 list、realpath、预览与传输没有自动获得新契约。

`gt_sftp_stat_t` 追加大小已知和普通文件标志；Swift 已映射，Android §9.51 的 JNI/Kotlin 后端也已保留这些信息，文件页已在 §9.52 消费。属于公开结构布局变更，所有绑定与原生消费者须重编译；符号对拍本身不检查旧结构布局。macOS 产品的 `FileEntry` 仍丢弃这些标志，未知大小/特殊类型的页面语义尚未修复。

第三份补丁 `0003-sftp-name-bounds.patch` 校验 NAME 返回路径长度不超过实际 payload，并拒绝空目录项名称。当前主机构建及 Android 构建均应用三份补丁，历史两补丁的设备与打包变异证据不得自动移记为第三份通过。

§9.49 为 41 CTest / 83 ABI、36 项目录协议、19 组浏览变异、独立提交 UAF 变异，以及 SFTP 八/exec 三/SSH 五组通过；主机与 NDK 依赖同九场景、七 native + 两收据变异通过。macOS 原生与 app-only 包通过，Android 最终完整 214 项设备回归通过。早期 Swift 定位失败、共享竞态/超时修复及设备中间轮失败见 [§9.49](android-implementation.md#949-受控目录浏览与共享会话修复2026-10-30)。以下保留 §9.47 的历史基线。

## 已有证据与失败记录

§9.47 已验收切片的最终验证及其各自范围如下；已有设备结果沿用相同产物的 SHA256 证据，不把历史日志冒充本轮设备重跑。

| 检查 | 结果 | 日志与范围 |
|---|---|---|
| 默认 Core | 39 项 CTest / 82 ABI 通过 | `tmp/sftp-controlled-final-gate.log`；不等于所有协议故障已覆盖 |
| 默认补丁库的受控打开 | 17 项通过 | `tmp/sftp-controlled-final-protocol.log`；正常/明确拒绝，channel/subsystem/VERSION 的取消、超时、IO，部分报文取消/超时与头部/正文 EOF |
| 接口变异 | 7 C + 2 Swift + 1 无库组通过 | `tmp/sftp-controlled-mutations.log`；包含 17 项基线、目标红/独立对照绿/恢复与 SHA256 |
| 既有回归 | SFTP 8、exec 2、SSH 5 组分别通过 | `tmp/sftp-controlled-legacy-sftp.log`、`tmp/sftp-controlled-legacy-exec.log`、`tmp/sftp-controlled-legacy-ssh.log` |
| macOS 增量构建 | 通过，链接仓库补丁库 | `tmp/sftp-controlled-macos-build.log`；不是完整宿主测试 |
| macOS 本机包 | `.app`、ad-hoc 签名、自包含依赖、网格/主题冒烟与新增普通依赖校验通过 | `tmp/sftp-host-package.log`；app-only，无本轮 ZIP/DMG、发布或 CI 证据 |
| 新依赖守卫变异 | **11 组通过** | `tmp/host-ssh2-mutations-final.log`；源码/补丁/构建器/产物/crypto、包节/收据/许可/格式/路径及 CMake 错误传播；每组目标红、独立对照绿、恢复绿与 SHA256 |
| 既有打包变异 | 63 项检查 + DMG 3 组通过，无 SKIP | `tmp/sftp-host-package-mutations.log`；真实依赖嵌入、签名、minos、主题及磁盘映像清理等原有保护 |
| Android 当前构建 | debug/测试 APK 通过 | `tmp/sftp-controlled-final-android-build.log`；debug APK 与 §9.45 完整 214 项运行时 SHA256 相同 |
| §9.47 沿用的 Android 设备证据 | §9.45 的 214 项通过 | API 36 arm64 / 4 KB 页模拟器；§9.47 按相同产物摘要沿用。后续产物的独立完整回归见 §9.49/§9.52/§9.54 |

此前守卫失败是测试观测面错误：破坏 prefix 收据后，整包校验又比较包收据，因此撤去 prefix 守卫仍被另一道检查拦住。现在 prefix 变异直接执行生产 `verify`，包变异执行 `check_bundle`；两条路径都有合法产物和独立损坏对照。新增 CMake 组实际配置 Core，撤去失败传播后目标红，独立非法 target 对照仍绿，恢复后再次拒绝坏收据。初次 CMake 断言未处理错误输出折行，改为仅归一化空白后复验，不忽略退出码或错误原因；历史失败没有用放宽生产校验来消除。

两端产品接线另验原总预算、取消后停止后续请求、晚到结果隔离和 owner 释放。Android A4 远端浏览/预览后端已由 §9.51 接入，页面随后由 §9.52 接入，A5/A6 复制尚待开发；macOS 产品仍用旧 opener，配置、设置、剪贴板和传输错误处理的缺口继续见[路线图](roadmap.md#macos-下一步与共享缺口)。本轮不新增完整 Apple 宿主、其他设备档位或 CI 证据。


## 专用连接中止与 Android 消费边界

`gt_ssh_abort` 由独占连接的 IO owner 调用，停止 socket 双向通信并将连接置为不可用；它不释放句柄，也不是可由 UI 并发调用的取消 API。调用方先停止其他操作，再 abort，依次关闭 SFTP/通道与 SSH。Swift 已有 `SSHSession.abort()`，macOS 文件产品尚未调用；Android 文件 owner 正常退出、取消和失败均先中止再清理，Kotlin close 仅发停止信号。普通 SFTP 传输的关闭契约没有因此改变。

§9.51 的 13 项新增设备测试、18 组 owner 变异、3 组公开 abort 变异及最终 52 项定向回归通过；本轮重跑打开 17、目录 36、预览 46 项协议。macOS 原生与 Android APK 构建通过，无本轮完整 Apple 宿主、打包或 CI 证据。完整边界与失败记录见[实施记录](android-implementation.md#951-android-远端文件-owner-与-jni2026-10-30)。
