# Android 宿主

**当前 Android 发行（2026-11-03，`f5eda91`）**：SSH 终端的“文件”打开当前会话主机的独立 SFTP，已配置的无口令密钥自动认证；首页“本机文件”回到本机。实际签名包的公网 SSH/SFTP、两系统 Kitty 动画与覆盖升级已验，初次 16 KiB 模拟器关机后的包解析失败及对照重启单独保留，见 [§9.159](../docs/reference/android-implementation.md#9159-f5eda91-实际发行包的两系统设备验收2026-11-03)。

[共用 dev APK](https://github.com/hanxi/ghosterm/releases/download/dev/Ghosterm-android-arm64-dev.apk) 为 8,129,887 字节，固定发行证书、clean 来源、R8/资源优化、仅 arm64、完整 Nerd Fonts/许可、ELF/ZIP 16 KiB 对齐与构建机路径检查均核验；[Android CI](https://github.com/hanxi/ghosterm/actions/runs/41475692020) 与 [macOS CI](https://github.com/hanxi/ghosterm/actions/runs/41470932423) 成功。两端共用 dev，保留八份平台附件。

**完整回归**：`f5eda91` / clean 在 API 29 / arm64 / 4 KiB 页和 API 36 / arm64 / 16 KiB 页各默认未筛选 **917 项全部通过**，零失败/错误/跳过。两端同源 904 份输入前后不变，具名发现/执行集合、实际安装 APK 与 clean 来源均核验（§9.160）。旧 `c594409` 的十五项夹具失败保留于 §9.155–§9.156；真机、完整键盘/读屏与外部端点矩阵仍待完成。

整个客户端仍未完成：不同提供方/物理服务器、真实手机网络/输入/性能及最终全量矩阵仍待收尾。A9 环境包按用户决定延后。Kitty 图形/动画、正式图标、Nerd Fonts、五类设置、可选远端 tmux 和 DNS 探测兼容已在当前发行中保留；用户已反馈手机域名连接正常，逐项网络矩阵尚未取得证据。

主机管理按真实 SSH 配置显示默认/继承/显式来源，支持稀疏编辑、配置文本、来源遮蔽、别名和身份关联；私钥只保存到本安装的 Keystore 加密容器。默认本地 PTY/SSH，后台 tmux 由用户显式选择。文件点按打开、长按选择，复制后显示清单，在本机/远端任意目标重新预检与确认。

**免重复选钥**：在“编辑主机 → SSH 身份 → 添加已导入密钥”保存关联，后续连接自动使用明确配置的无口令身份。导入或当次选钥不会静默修改主机配置；加密身份仍需口令。SSH 终端中的“文件”使用已连接主机的快照和独立 SFTP，不因后来编辑主机地址而跳到另一台服务器。

- [最终交互原型](../docs/design/android-prototype.html)：主机首页、终端、文件、设置，单一 HTML。
- [实现计划与证据](../docs/reference/android-implementation.md)：模块、线程、配置/认证、输入、复制和 A0–A10。
- [路线图](../docs/reference/roadmap.md)、[能力矩阵](../docs/reference/capability-matrix.md)。

## 远端文件页面

位置面板从当前主机配置发起独立文件连接，共用既有 DNS、主机验证与身份认证；不借用终端连接。端点保留 UUID 和实际 host/user/port 快照，配置编辑不改写已连接端点。真实目录成功后显示规范路径，点按预览、长按选择；离页关闭浏览连接，重新进入须显式重连。

实现入口为 [文件 repository](data/src/main/java/com/hanxi/ghosterm/data/FileBrowserRepository.kt)、[文件 ViewModel](app/src/main/java/com/hanxi/ghosterm/FilesViewModel.kt) 与 [真实产品设备测试](app/src/androidTest/java/com/hanxi/ghosterm/RemoteFilesProductTest.kt)。§9.60 已接复制来源清单；§9.70 已接粘贴任务，外部只读打开已在 §9.92 工作区实现并进行独立验收；浏览连接离页关闭，独立任务连接由任务持有者管理。

```bash
ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.RemoteFilesProductTest bash scripts/test-android-ssh.sh
ANDROID_SERIAL=设备 ANDROID_FILES_PRODUCT_MUTATE=1 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.RemoteFilesProductTest bash scripts/test-android-ssh.sh
```

第二条运行 14 组已编译的产品变异，每组配同 APK 独立配置对照及恢复验证；来源为 [变异脚本](../scripts/mutation-check-android-files-product.py)。完整验证结果与历史失败见实现计划 §9.52；需要先准备 SSH 协议夹具环境，见下文测试入口。

## 粘贴确认与任务页面

本节的 §9.70–§9.73 为交付记录。§9.73 在相同页面上新增历史区和记录错误状态：接纳、取消及清除可能需要等待 SQLite 事务；重启显示只读记录，不恢复清单或执行。详细状态、容量和失败契约见 [§8.2](../docs/reference/android-implementation.md#82-a6-持久任务记录与中断核对)；该切片当时完整 475 项通过，不能作为当前发行版全量结果。

进程所有权入口为 [FileWorkRuntime](app/src/main/java/com/hanxi/ghosterm/FileWorkRuntime.kt)，任务状态机 `FileTaskStore` 与页面 facade 同在 `FileTasksViewModel.kt`。§9.70 新增 [FileTasksViewModel](app/src/main/java/com/hanxi/ghosterm/FileTasksViewModel.kt) 和 [FileTasksScreen](app/src/main/java/com/hanxi/ghosterm/FileTasksScreen.kt)。文件来源卡片提供粘贴入口；任务固定源/目标，预检后确认冲突，独立认证后执行，保留实际位置与逐项错误。默认保留复制清单；取消要等待实际 provider/原生资源释放，不立即释放名额。完整实现及尚未完成范围见[任务契约](../docs/reference/android-implementation.md#619-当前任务页面与-a6-后续实现)。

```bash
ANDROID_SERIAL=emulator-5554 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.FileTasksProductTest,com.hanxi.ghosterm.RemoteFileTasksProductTest,com.hanxi.ghosterm.SafFileTasksProductTest bash scripts/test-android-ssh.sh
ANDROID_SERIAL=emulator-5554 ANDROID_FILE_TASK_PRODUCT_MUTATE=1 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.FileTasksProductTest,com.hanxi.ghosterm.RemoteFileTasksProductTest,com.hanxi.ghosterm.SafFileTasksProductTest bash scripts/test-android-ssh.sh
```

18 项定向使用真实 HOME、系统 DocumentsUI/独立 provider 与 SSH。24 组产品变异覆盖路由、请求身份、取消名额、结果/清单和认证归属；失败过程与最终证据统一记在 §9.70。§9.71 改由进程级 `FileWorkRuntime` 持有任务、清单和独立认证，`FileTasksViewModel` 仅保留页面路由与 presenter UUID；显式结束 Activity 只解除呈现归属。§9.72 已接 FIFO 等待请求，扫描/确认/执行仍串行；§9.74 已接后台通知，§9.98 已接暂停/继续、安全失败项重试与整体 ETA；持久历史的位置/内容核对、显式重新发起和清理已按 §9.104–§9.123 接入；完整外部端点/设备矩阵仍待验，普通任务不自动恢复执行。§9.127 已将任务入口收口到首页和文件页底部，首页右上角仅设置。

## 文件任务排队（§9.72）

队列按 FIFO 接纳最多 32 个未结束请求；等待时仅保存固定来源/目标，不读取文件或建立连接。轮到后扫描、独立认证并再次确认；等待项可直接取消，运行项取消后须等真实资源释放才推进下一项。页面显示前方任务数，旧冲突决定不传给后续请求。该队列切片当时完整 453 项通过；后续 §9.73/§9.74 已接持久观察记录与后台传输服务。重启只呈现记录供显式核对，不自动恢复执行；完整矩阵仍待验。

```bash
ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.FileQueueProductTest ANDROID_FILE_QUEUE_MUTATE=1 bash scripts/test-android-ssh.sh
```

[队列产品测试](app/src/androidTest/java/com/hanxi/ghosterm/FileQueueProductTest.kt)与真实双端点测试合计 8 项定向；[队列变异脚本](../scripts/mutation-check-android-file-queue.py)执行 14 组，最终联合 8 项复验通过。runner 自动在工程临时目录启动两个不同端口/主机密钥/目录的 OpenSSH 服务，全部流量经 adb loopback；这是同宿主双服务证据，不能冒充公网、物理双机或完整四方向复制矩阵。失败与复验记录见[§9.72](../docs/reference/android-implementation.md#972-a6-fifo-文件任务队列2026-10-31)。

## 冲突决策与执行清单验证

`FilePasteDecisions` 绑定请求与确认修订，`FileCopyPlan` 冻结来源、目标名称、跳过原因与大小已知性。`FileCopyNames` 只产生有界候选，名称最终仍须独占创建/非覆盖发布。15 项定向与 42 组变异证据见 §9.63；是该数据层的历史证据；§9.70 已接任务和冲突页面，本片当前模拟器档验证通过。

```bash
ANDROID_SERIAL=emulator-5554 python3 scripts/mutation-check-android-decisions.py
```

## 目标预检验证

`FileDestinationAccess` 读取真实目标，`FilePastePreflight` 保留请求身份、创建能力三态与只读冲突数据。相同目录/目录自身后代被拒绝，读取错误不当作空目录；不同远端命名空间关系仍未知。仅元数据预检，§9.70 已有任务/页面消费者，不能代替写前重验和非覆盖提交。

```bash
ANDROID_SERIAL=emulator-5554 python3 scripts/mutation-check-android-preflight.py
```

13 项定向、34 组变异及最终设备证据见 §9.62。

## 来源树扫描验证

`FilePasteRequest` 固定源/目标和请求身份；`FileTreeScanner` 仅枚举、复核来源元数据。默认深度 32、16384 节点、4 MiB 结果文本和 30 秒预算；失败/超限不发布完整计划，链接与特殊类型等待显式决策。扫描成功不代表目标授权或允许写入，任务连接由 §9.67、执行器由 §9.69、页面由 §9.70 接入。

```bash
ANDROID_SERIAL=emulator-5554 python3 scripts/mutation-check-android-scan.py
```

13 项定向、34 组变异及完整设备范围见实现计划 §9.61。

## 复制来源清单验证

已接选中后复制、跨位置来源卡片、完整清单与确认替换/清空。复制只冻结选择元数据；§9.61 已接来源扫描；§9.70 已接粘贴执行、授权重验与任务页面。§9.71 的清单由进程级 `FileClipboardStore` 持有，旋转、退出页面和显式结束 Activity 后仍保留；进程死亡尚不恢复；不写系统文本剪贴板，也不持有文件连接。

```bash
ANDROID_SERIAL=emulator-5554 python3 scripts/mutation-check-android-clipboard.py
```

9 项定向及 16 组变异范围、设备中断与面板展开修复见实现计划 §9.60。

## SAF 目标文档验证

`SafFilePublisher` 是 SAF 发布数据层；§9.70 已接文件粘贴与任务页面，调用方通过统一执行器消费发布结果。创建后失败可能留下文档；请求保留 URI 与副作用状态，不能自动删除或盲重试。16 项真实系统授权/可写 provider 设备用例和 30 组变异的范围见实现计划 §9.59。

```bash
ANDROID_SERIAL=emulator-5554 python3 scripts/mutation-check-android-saf-publication.py
# --case 名称：运行指定一组，仍先跑完整 SAF 基线；仅用于专用测试设备
```

## 本机与 SAF 文件验证

```bash
ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-files.py
```

真实本地文件与系统树选择器；18 组目标红/同 APK 配置对照绿/恢复绿/SHA。无需 SSH 夹具即可定向跑；完整套件仍按下文使用 SSH/凭据/协议端夹具。首片只读浏览与有界 UTF-8 预览；该首片范围见 §9.43；后续远端页面见 §9.52，任务页面见 §9.70，进程持有与 FIFO 队列分别见 §9.71/§9.72。

## SFTP 依赖验证

Android 构建器从校验后的 libssh2 1.11.1 源码应用 EOF/初始化清理/NAME 边界三份补丁，补丁哈希纳入依赖收据；远端文件 owner 与页面已由 §9.51/§9.52 接入。

```bash
python3 scripts/test-libssh2-sftp.py --fetch --mutate
python3 scripts/test-libssh2-sftp.py --android emulator-5554
```

第一条在隔离主机库验证九场景与七组 native/两组收据变异；第二条从实际 Android archive 构建探针，在指定设备验证同九场景。新增 NAME 报文长度、空名称及正常名称对照，不将旧六场景当作第三补丁证据。须先运行 `bootstrap-ssh-channel-tests.sh` 与 Android 构建，详见 [补丁说明](../patches/libssh2/1.11.1/WHY.md)。它们不替代 JNI owner、文件页面或完整设备回归。

## SSH 通道打开验证

认证后，native owner 使用原连接剩余预算打开 channel/PTY/shell，取消信号进入共享受控入口；不能仅取消页面后继续在后台等待完整五秒。十三项协议阶段用例和十三组变异见 [§9.41](../docs/reference/android-implementation.md#941-ssh-通道打开预算与取消2026-10-30)。错误码区分取消、IO/超时与明确 PTY/shell 拒绝，普通真实 sshd 的 PTY/PID/IO 回归仍独立保留。

```bash
bash scripts/bootstrap-ssh-channel-tests.sh
ANDROID_SERIAL=emulator-5554 \
  ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.SSHChannelTest \
  ANDROID_SSH_CHANNEL_MUTATE=1 bash scripts/test-android-ssh.sh
```

`ANDROID_SSH_CHANNEL_MUTATIONS=cancel-channel,budget-shell` 可选跑，未指定则十三组。协议故障端只绑定宿主回环，以真实 SSH 请求回调标记停滞阶段，经 adb reverse 提供给设备；它不运行真实 shell。Paramiko 及依赖锁在 `scripts/fixtures/ssh-channel-requirements.txt`，仅装到 `build/ssh-channel-toolchain`，不进入 APK。`ANDROID_SSH_ALL=1` 包含本组，需先准备该环境；其中网络丢包组仍要求可 root 的模拟器，本通道组本身不需要 root。

## SSH 候选地址验证

网络用例当前需要可 `su 0` 的 arm64 模拟器；不是普通真机无需 root 的测试声明。TCP 丢包只影响测试的回环地址/端口，带本轮唯一标记；runner 的 finally 在异常/取消路径也清理；SIGKILL 无法执行 finally，需人工检查本轮标记。`ANDROID_SSH_ALL=1` 包含此组，因此当前完整套件也要求该测试环境。

```bash
ANDROID_SERIAL=emulator-5554 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.SSHNetworkTest \
  ANDROID_SSH_NETWORK_MUTATE=1 bash scripts/test-android-ssh.sh
ANDROID_SERIAL=emulator-5554 python3 scripts/mutation-check-android-network-cleanup.py
```

可用 `ANDROID_SSH_NETWORK_MUTATIONS=advance,total-budget` 选跑；未选则跑全部 14 组。脚本使用真实 SSH 和同 APK 独立配置对照，编译/运行异常不作为目标断言红。实现与未覆盖范围见[§9.40](../docs/reference/android-implementation.md#940-ssh-候选地址与-tcp-取消2026-10-30)。

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

主机验证管理定向与变异（指定一次性测试设备，需真实隔离 sshd）：

```bash
ANDROID_SERIAL=emulator-5554 \
ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.KnownHostsSettingsTest \
ANDROID_SSH_KNOWN_HOSTS_SETTINGS_MUTATE=1 bash scripts/test-android-ssh.sh
```

此命令先验证 11 项页面/数据用例，再验证 17 组变异；完整设备套件仍使用 `ANDROID_SSH_ALL=1`。现有身份页面的共用公钥解析回归使用 `ANDROID_SSH_IDENTITIES_MUTATE=1`；各组均要求编译成功后目标红、同 APK 配置对照绿、恢复绿与 SHA256。实际范围见[§9.39](../docs/reference/android-implementation.md#939-主机验证记录管理页面2026-10-30)。

连接 arm64 设备/模拟器后，明确指定序列号。完整套件与凭据定向用例另需 Docker：一次性容器提供密码账户，首次构建需要网络；普通 native SSH 定向用例不需要容器。

```bash
export ANDROID_SERIAL=emulator-5554
ANDROID_SSH_ALL=1 bash scripts/test-android-ssh.sh
# 产品页面与身份容器变异（产品首片，范围见 §9.27）
ANDROID_SSH_PRODUCT_MUTATE=1 bash scripts/test-android-ssh.sh
# SAF 私钥导入：真实系统选择器与七组变异（§9.29）
ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.SSHIdentityImportTest \
ANDROID_SSH_IMPORT_MUTATE=1 bash scripts/test-android-ssh.sh
# 密码/加密私钥产品流程与七组变异（§9.31）
ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.SSHCredentialFlowTest \
ANDROID_SSH_CREDENTIAL_MUTATE=1 bash scripts/test-android-ssh.sh
# 远端单目录请求：12 项设备基线与 20 组变异（§9.65）
ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.RemoteDirectoryTest \
ANDROID_MKDIR_MUTATE=1 bash scripts/test-android-ssh.sh
# 统一目录数据层：真实 SSH/协议与 23 组变异（§9.66）
ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.RemoteDirectoryDataTest \
ANDROID_MKDIR_DATA_MUTATE=1 bash scripts/test-android-ssh.sh
python3 scripts/check-abi-android.py
python3 scripts/mutation-check-android-abi.py
python3 scripts/mutation-check-android-native.py
python3 scripts/mutation-check-android-config.py
python3 scripts/mutation-check-android-terminal.py
# A2 文本与按键切片验证，各轮完整/选组范围见 §9.14–§9.18
python3 scripts/mutation-check-android-text.py
python3 scripts/mutation-check-android-vt-libc.py
# API 29 fd 控制；IME finish 的 raw PTY 证明目前使用 API 36 / 4 KB 页设备
python3 scripts/mutation-check-android-fd-flags.py
python3 scripts/mutation-check-android-ime-finish.py
# 四模块 release lint；对应变异不需要模拟器
bash scripts/check-android-api.sh
python3 scripts/mutation-check-android-api.py
bash scripts/gate.sh
```

A1 验收时 JNI 引用 11 项、其余 62 项；A2 首片引用 19 项（新增 Transport 与错误读取），其余 54 项在 `bridge/deferred-abi.json` 按阶段登记，连接检查切片引用 20 项、待办 54 项；已验 SSH 后端为 32/42，`c53cfb8` 已提交的身份容器首片为 33/41；SAF 导入切片未增加公共 ABI 引用；IdentityFile 来源查询切片为 34/41，主机信任记录快照切片为 38/41、79 个公共符号；事务/移除绑定为 39/41、80 个公共符号（§9.37）；通道取消入口替换旧 shell 引用后为 39/42、81 个公共符号（§9.41）；受控 SFTP 切片为 40/42、82 个符号（§9.47，仅 helper，未接文件产品）。受控浏览切片为 40/43、83 个符号（§9.49，browse 待接）；预览切片为 40/44、84 个符号（§9.50，preview 当时待接）；文件 owner 切片为 45/40、85 个符号（§9.51，browse/preview/清理已接）。后续独占创建为 46/40、86 ABI（§9.54），受控读取为 47/40、87 ABI（§9.56），非覆盖发布为 48/40、88 ABI（§9.57），受控单目录创建为 49/40、89 ABI（§9.65）。§9.76 增加按请求收发进度观察，为 JNI 51/40、91 ABI；§9.77 已消费到任务执行器与页面，当前文件阶段进度已验，不代表整个 A6 完成。该文档复核时未登记的发布 C 草稿已由 §9.57 补齐 Swift/JNI/Kotlin 接线和专项验证；历史对拍失败不再描述当前工作区。以当前检出的 `check-abi-android.py` 输出为准，不能把未绑定能力称为已实现。变异脚本会在指定设备重新安装 debug APK，仅用于测试设备。原生变异分别破坏 UTF-8 配置往返、NUL 拒绝与 VT feed，目标断言红、同一 APK 独立对照绿、恢复后绿并校验源码 SHA256；中断会恢复带标记的源码。

A0 证据：API 36 / arm64 / 4 KB 页模拟器，3 项设备测试、3 组 JNI 变异、3 组 ABI 对拍变异通过。A1 首片为 17 项设备测试与 11 组配置变异；共享重复指令编辑修复后，该片为 **18 项设备测试、12 组配置/页面变异**。C9 验编辑器损坏输出仍被拒绝，C12 验设备使用真正的 Core 修复；均目标红、同 APK 独立对照绿、恢复绿且源码 SHA256 一致。真机、API 29、16 KB 页运行、性能与完整 UI 验收留在后续阶段，不将 ELF 对齐等同于设备验证。

字段来源切片为 22 项设备测试与 20 组配置/页面变异；后续显式/继承编辑切片增至 32 项设备测试与 32 组配置/页面变异，仍为 API 36 arm64 / 4 KB 页模拟器证据。共享契约另以 `python3 scripts/mutation-check-config-metadata.py --apple` 验 8 组 C 和 1 组 Swift 变异；该脚本与 Android 变异均为手工执行，未纳入 macOS 的 20 份 full CI 清单。各次结果见路线图，旧切片结果不冒充本轮复验。

重命名已打通 Core → JNI → 仓库 → 别名输入框；仅改主文件唯一 Host 块中的目标别名 token，保留同块其他别名和非目标行。包含文件、重名块、同头重复别名及目标名称冲突会拒绝改名。本片 36 项设备测试、36 组配置/页面变异、12 组 C / 1 组 Swift 改名变异与平台构建证据见[§9.9](../docs/reference/android-implementation.md#99-主机别名重命名2026-10-30)。仅改名保留指令字节，默认 HostName 和通配规则的生效值可能随新别名变化。

读取失败或保存冲突后，旧快照失效，列表不冒充合法空态/当前数据；保留编辑草稿，成功重载前禁用保存与配置入口。错误跨设置/关于页返回后保留重试入口；重载有未保存内容时先确认放弃，选择继续编辑不动草稿或磁盘。主机重载会刷新来源/值，目标已删除则回首页。此片 40 项设备测试与 47 组变异证据见[§9.10](../docs/reference/android-implementation.md#910-配置错误恢复与重载确认2026-10-30)。

系统文件选择器、预览确认/取消、完整导入、空文档、超限/权限/编码/私钥/NUL/Include 失败与保存冲突已通过真实 DocumentsUI 测试；可写的测试 provider 记录外部写入次数为零，生产 APK 不注册它。确认框的预览有明确截断提示，保存完整字节；错误和重载入口不被长预览遮挡。A1 收尾见[§9.11](../docs/reference/android-implementation.md#911-saf-导入预览与-a1-收尾2026-10-30)。

## 性能测量

明确选择测试设备后运行（安装 debug 与测试 APK）：

```bash
ANDROID_SERIAL=emulator-5554 python3 scripts/profile-android-terminal.py --label baseline
# adb 在 PATH
ANDROID_SERIAL=emulator-5554 python3 scripts/mutation-check-android-performance.py
```

报告保存在工程 `tmp/android-performance/`，含原始样本、窗口帧时长、输入至实际像素延迟、吞吐、CPU、采样内存和关闭耗时，以及 APK 哈希/设备信息。测量使用真实 PTY/VT/Canvas，但运行的是 debug + instrumentation，不能等同于 release 真机性能。调度方案和本片验证记录见[§9.21](../docs/reference/android-implementation.md#921-输入后短时取帧与性能基线2026-10-30)。

身份设置定向与变异（系统选择器、指纹、移除、旋转与取消；同上需要真实 SSH 夹具和 Docker）：

```bash
ANDROID_SERIAL=emulator-5554 \
ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.IdentitySettingsTest \
ANDROID_SSH_IDENTITIES_MUTATE=1 bash scripts/test-android-ssh.sh
```

八项定向与十组变异已通过；范围见[§9.33](../docs/reference/android-implementation.md#933-ssh-身份设置与公钥信息2026-10-30)。主机引用与身份映射已按 §9.35 接入，known_hosts 管理已由 §9.39 交付；§9.91 已接五类设置、16 项偏好与已有 surface/文件页面消费者，证据范围见实现计划。

## 边界

UI 经 ViewModel/controller/repository 使用桥；不直接调用 JNI。终端实现位于 [terminal/android](../terminal/android/README.md)，使用 libghostty-vt + 自绘，不引用 Apple 代码。JNI 字符串经标准 UTF-8 byte array 传递，拒绝内嵌 NUL；A2 首片已建立原生 Transport→VT 数据通路，Canvas 读取网格快照；输出禁止通过 Compose/String/Flow 搬运，输入法契约和生命周期已有定向证据，真实 Gboard 拼音基本路径另有手工证据；完整输入矩阵与性能仍待补齐。

普通 SSH 为主要远端入口。本地 A2 验证系统 `/system/bin/sh` 与 PTY，不把它称为随 App 内置的 shell。Termux/proot 环境包独立延后，按需下载且不进 APK。

SSH 后端通过 `RemoteSession` 复用 native owner 和 VT；产品 DNS、主机确认/凭据页面与 Keystore 已由 §9.27 接入，SAF 导入生命周期由 §9.29 验收。真实 SSH 设备测试需上述隔离 sshd 夹具，不在设备落盘明文私钥；退出状态与历史通道边界见[§9.25](../docs/reference/android-implementation.md#925-android-ssh-认证与远端终端后端2026-10-30)。

加密私钥错误口令被当作协议错误的问题已修复，四项凭据定向、七组设备变异及完整 **139 项设备测试**通过；密码重试、旋转/方式切换清理及取消晚到材料均验真实产品路径。旧失败保留在 §9.30，当前证据与 Docker 夹具见[§9.31](../docs/reference/android-implementation.md#931-加密私钥重试与密码产品流程2026-10-30)。整个 A3 仍未完成。

主机身份关联验证使用真实 Keystore 与隔离 sshd，覆盖配置保存、认证候选、引用详情及活动会话保留：

```bash
ANDROID_SERIAL=emulator-5554 \
ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.HostIdentityTest \
ANDROID_SSH_HOST_IDENTITIES_MUTATE=1 bash scripts/test-android-ssh.sh
```

新增十组变异及该切片完整 160 项设备回归通过；本轮其他专项范围见[§9.35](../docs/reference/android-implementation.md#935-主机身份关联与引用详情2026-10-30)。与其他设备测试、变异脚本串行执行，不能让两个 runner 同时改源码或操作同一模拟器。

主机信任记录的只读桥已提供物理行号、原始字节区间和 marker 等字段；`recognized` 不代表信任或密钥合法；该只读切片当时未接页面，删除桥/共享事务后来由 §9.37 交付，管理页面由 §9.39 交付。历史两项绑定用例与四组变异的范围见[§9.36](../docs/reference/android-implementation.md#936-主机信任记录快照与平台绑定2026-10-30)：

```bash
ANDROID_SERIAL=emulator-5554 python3 scripts/mutation-check-android-known-hosts.py
```

CI 已产出签名且压缩的 release APK，下载与签名说明见 [Android release](../docs/how-to/android-release.md)。首次云端包对应 `d585f0b`；当前发行与实际下载/安装证据见本文开头，不借首次包结果覆盖后续提交。

## 文件阶段进度（§9.77 历史切片）

任务页区分读取来源、写入目标和核对/提交；活动字节独立于已提交字节，不用作续传偏移。总量未知不伪造百分比；平均速度计入阻塞耗时，只估计当前字节阶段的剩余时间，停滞、核验或总量未知时不显示估计。该切片当时未接暂停/重试和整体剩余时间；§9.98 后已接暂停/继续、安全失败项重试与整体 ETA，未知总量及不可估计阶段仍不伪造剩余时间。

```bash
ANDROID_SERIAL=设备 ANDROID_COPY_PROGRESS_MUTATE=1 bash scripts/test-android-ssh.sh
```

25 组新增变异使用真实编译的目标断言红、同 APK 独立配置对照绿、恢复绿及 SHA256。受影响旧变异累计 114 组分轮通过；完整 491 项通过，范围和失败/恢复历史见[§9.77](../docs/reference/android-implementation.md#977-a6-文件阶段进度与任务页面2026-11-01)。该脚本为手工设备验证，未加入 macOS CI 20 份覆盖。


## 可选远端后台会话

主机卡片的「后台会话」显式探测远端 tmux，支持路径/版本、真实列表、新建/重命名/确认结束和独立 SSH 客户端连接；不自动安装或修改远端 tmux 配置。关闭客户端不发送结束命令，结果未知不自动重试，断线须刷新重选。手机本地 tmux 未提供。Core 操作由独立 native actor 管理，终端仍消费普通 Transport，详见 [A8 接线与验收](../docs/reference/android-implementation.md#android-tmux)。
