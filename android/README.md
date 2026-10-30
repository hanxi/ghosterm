# Android 宿主

最终页面方案与开发顺序见[详细实现文档](../docs/reference/android-implementation.md)：§6.5 给出真实文件浏览、多选、待粘贴清单、冲突与任务的接入次序；§7.1 给出 16 项设置的存储、即时应用、失败恢复和验收。A4 本机/SAF 只读浏览与多选已由 §9.43 交付；远端文件、复制任务和完整偏好仍按这两节实施。当前信任管理已有共享读取快照、TOFU/移除事务和删除桥。repository、公钥指纹及设置管理页已通过 11 项定向、17 组变异及完整 175 项设备回归，见[§9.39](../docs/reference/android-implementation.md#939-主机验证记录管理页面2026-10-30)；A3 网络矩阵和 A7 完整偏好仍未完成。

A3 网络收尾具体要求见[§4.6](../docs/reference/android-implementation.md#46-a3-网络与取消收尾)：候选地址推进已接入同一个 native owner，TCP 阶段尝试备用地址、共享剩余总预算，协商/信任失败不换地址。12 项真实 SSH/模拟器定向及 14 组变异见 §9.40；通道打开取消已由 §9.41 接入；真实域名 DNS 及断线/设备矩阵仍待完成。

状态（2026-10-30）：**A0、A1 已在 API 36 arm64 模拟器档完成**。首页读取真实 SSH 配置，支持搜索、添加、稀疏编辑和配置文本编辑；设置页有 SSH 身份、主机验证记录、配置导入与关于入口。**A2 本地终端首片已验、整个阶段尚未完成；SSH native 认证/远端终端后端已接（§9.25），DNS/认证页面、Keystore 容器与多会话、SAF 导入、凭据、身份设置、主机关联、信任读写桥、候选地址与通道取消与本机/SAF 文件首片已通过完整 214 项回归（§9.43）；远端 SFTP 与完整设置尚未接入**，没有 Android CI。来源展示、共享别名提示及包含文件/重名块只读保护已接入；显式覆盖/恢复继承已完成；主机别名重命名、配置读取/冲突错误恢复已接入；SAF 导入预览、取消、错误恢复及表单校验已完成 A1 验收。

§9.41 的 200 项对应通道切片 `6817bd8` 的源码；通道十三组、产品十五组及共享 SSH/exec/tmux 回归已收口。早期失败和未覆盖的真实网络/设备范围保留在 §9.41，不表示整个 A3 已完成。

**文件浏览首片**：`FilesScreen` / `FilesViewModel`、domain 模型和 `LocalFileBrowser` 已接应用 HOME、系统授权树、只读 UTF-8 预览与多选。14 项定向、18 组变异和完整 214 项设备回归通过；终端会话/文件/键盘导航已对齐，原有编译和 fd 泄漏问题已修复。远端位置、文件复制清单/任务和 16 项持久偏好仍待接入，见 [§9.43](../docs/reference/android-implementation.md#943-a4-本机与-saf-文件浏览首片2026-10-30)。

当前表单显示 HostName/User/Port 的默认、继承或显式来源；早期规则遮蔽本块设置时也会说明。只读主机可打开配置页调整，返回后刷新字段和编辑权限，详见[实现计划 §9.5](../docs/reference/android-implementation.md#95-字段来源与编辑归属2026-10-29)。这些三字段来源不代表全部高级配置已支持；身份来源和容器关联另见 §9.34–§9.35。

`ConfigBridge.identities` 已提供原序来源查询；主机编辑支持关联已导入身份、删除本块条目与恢复继承，认证页按配置预选候选，身份详情显示引用主机及读取错误。引用写入配置为 `ghosterm-identity:<规范 UUID>`，仅对本安装加密容器有效；移除容器不删除配置引用、不关闭活动连接。普通路径保留并提示，由用户明确选择仅本次身份；不自动匹配无关密钥。实施契约见[§4.4](../docs/reference/android-implementation.md#44-主机身份关联的实施顺序)，当前完整 160 项及变异记录见[§9.35](../docs/reference/android-implementation.md#935-主机身份关联与引用详情2026-10-30)；§9.34 的 46 项定向不累加。

当前三个字段可独立“使用继承值”：关闭后写入显式设置，开启后删除本块指令并重新求值；即使显式值等于继承值也能保存。新主机允许全部继承。此前工作区草稿的 Kotlin 编译失败已解决，最近 A1 完整验收为 **47 项设备测试 / 59 组配置变异**见[§9.11](../docs/reference/android-implementation.md#911-saf-导入预览与-a1-收尾2026-10-30)；API 36 arm64 / 4 KB 页模拟器之外的档位仍未验证。

- [最终交互原型](../docs/design/android-prototype.html)：主机首页、终端、文件与完整设置，单一 HTML。
- [详细实现计划](../docs/reference/android-implementation.md)：模块/线程、配置与认证、输入法、复制、设置默认值、A0–A10 与验收。
- [跨端路线图](../docs/reference/roadmap.md)、[能力矩阵](../docs/reference/capability-matrix.md)。

A2 首片接入系统 shell、原生 owner、VT 网格与 Canvas、InputConnection、会话切换和关闭确认；Activity 重建与系统键盘尺寸已有真实路径证据。首片完整 63 项设备测试与 21 组终端变异的历史记录见[§9.13](../docs/reference/android-implementation.md#913-a2-真本地终端与生命周期首片2026-10-30)。文本操作切片另接长按选词、选区手柄、系统复制粘贴及确认、本地清屏、正常 shell 退出后的浏览/复制；该片验证集中记录在[§9.14](../docs/reference/android-implementation.md#914-a2-文本操作工作区与文档复核2026-10-30)。选区边缘自动滚动、锚点跨越与 Ctrl 状态清理已交付，85 项完整设备测试及 35 组文本/21 组终端变异见[§9.15](../docs/reference/android-implementation.md#915-选区与-ctrl-工作区进度2026-10-30)；本片已验 Ctrl 标点、模式感知 Enter 与真实 Gboard 拼音基本路径，完整 90 项设备测试和本轮选组变异见[§9.16](../docs/reference/android-implementation.md#916-ctrl-标点与键盘协议2026-10-30)；后续按键重复/释放、焦点归属与辅助键成对事件已通过完整 94 项设备测试和 18+2 组选组变异，见[§9.17](../docs/reference/android-implementation.md#917-按键重复释放与焦点归属2026-10-30)。完整软硬键盘矩阵与性能仍待完成。

与最终原型的差距仍需保留：终端底栏目前是主机/会话/键盘，文件入口等 A4；字号当前为 14sp，原型的默认 12sp 与其余完整偏好等 A7。文本剪贴板不等于文件待粘贴清单，后者属于 A5/A6。

修饰键、数字键盘与组合音符扩展已完成本片验收：完整 **99 项设备测试，0 失败/错误/跳过**；文本变异选跑 **24 组**通过，含新增 16 组与旧八组，见[§9.18](../docs/reference/android-implementation.md#918-修饰键数字键盘与组合音符2026-10-30)。脚本总量 71 组不是本轮全量重跑；证据仍限 API 36 arm64 / 4 KB 页模拟器。

静止画面的闪烁文字已修复：隐藏光标且没有新输出时仍自动重绘，普通文字保持可见。该切片完整设备测试 **100 项全通过**，五组渲染变异通过，证据见[§9.19](../docs/reference/android-implementation.md#919-静止画面的闪烁文本2026-10-30)；不替代 A2 性能与真机验收。

后续已接入输入后 120ms 内按显示帧取快照、随后回到 32ms 轮询；模拟器 debug 性能基线、四组性能变异和五组选组回归通过。最近完整通过记录 **214 项，0 失败/错误/跳过**（§9.43，含文件浏览与既有真实 SSH/凭据/通道夹具；§9.41 的选区早期失败及复验记录继续保留）；性能基线见[§9.21](../docs/reference/android-implementation.md#921-输入后短时取帧与性能基线2026-10-30)；真机/release、长负载与完整输入矩阵仍待验收。

共享主机信任前置已修复撤销/CA 标记误信任，信任写入/关闭失败会阻断认证（§9.22–§9.23）。共享连接配置准入和 SSH native 后端分别见 §9.24–§9.25。当前首页在最终配置检查通过后进入 DNS/主机验证/凭据与远端终端；默认 ViewModel 工厂已修复。同主机多连接按 UUID 选择，明确新建保留旧连接；Keystore AES-GCM 容器只落盘密文。具体生产入口、定向证据与未完成项见[§9.27](../docs/reference/android-implementation.md#927-ssh-产品页面与加密身份容器2026-10-30)。

产品首片的 27 项定向测试、15 组产品变异及完整 130 项回归见 §9.27。后续 SAF 私钥导入的旋转、取消、只读材料与旧请求归属已通过五项定向、七组新增变异及完整 **135 项设备测试**；历史失败与修正见[§9.29](../docs/reference/android-implementation.md#929-saf-私钥导入生命周期2026-10-30)。成功密码登录和加密 OpenSSH ed25519 口令重试已在 §9.31 验证；身份设置首片见 §9.33，主机身份关联见 §9.35，信任记录管理已由 §9.39 交付，网络矩阵仍待完成，整个 A2/A3 不勾选完成。

SSH 身份设置已接通列表、详情、指纹、导入与移除，身份设置八项定向/十组变异通过，后续主机关联切片完整 160 项设备测试通过；早期清理/旋转观测失败保留在[§9.32](../docs/reference/android-implementation.md#932-身份设置工作区复核2026-10-30)，修正与当前证据见[§9.33](../docs/reference/android-implementation.md#933-ssh-身份设置与公钥信息2026-10-30)。A7 的五类/16 项偏好仍未实现。

**最新依赖进度**：Android 已应用 libssh2 EOF/初始化清理补丁，真实设备六场景及最新完整 214 项回归通过；§9.44 的选区偶发失败尚未定位原因。SFTP helper 仍未被文件 owner 或 Kotlin 产品调用；40/42、82 ABI 是未验收草稿数量，macOS 默认依赖仍未切换，不表示远端浏览已实现。详见 [§9.45](../docs/reference/android-implementation.md#945-android-sftp-依赖修复与设备复验2026-10-30)。

## 本机与 SAF 文件验证

```bash
ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-files.py
```

真实本地文件与系统树选择器；18 组目标红/同 APK 配置对照绿/恢复绿/SHA。无需 SSH 夹具即可定向跑；完整套件仍按下文使用 SSH/凭据/协议端夹具。首片只读浏览与有界 UTF-8 预览，远端文件/复制队列未接，详见 §9.43。

## SFTP 依赖验证

Android 构建器从校验后的 libssh2 1.11.1 源码应用 EOF/初始化清理补丁，补丁哈希纳入依赖收据；远端文件 owner 仍待接入。

```bash
python3 scripts/test-libssh2-sftp.py --fetch --mutate
python3 scripts/test-libssh2-sftp.py --android emulator-5554
```

第一条在隔离主机库验证六场景与五组 native/两组收据变异；第二条从实际 Android archive 构建探针，在指定设备验证六场景。须先运行 `bootstrap-ssh-channel-tests.sh` 与 Android 构建，详见 [补丁说明](../patches/libssh2/1.11.1/WHY.md)。它们不替代 JNI owner、文件页面或完整设备回归。

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
python3 scripts/check-abi-android.py
python3 scripts/mutation-check-android-abi.py
python3 scripts/mutation-check-android-native.py
python3 scripts/mutation-check-android-config.py
python3 scripts/mutation-check-android-terminal.py
# A2 文本与按键切片验证，各轮完整/选组范围见 §9.14–§9.18
python3 scripts/mutation-check-android-text.py
python3 scripts/mutation-check-android-vt-libc.py
bash scripts/gate.sh
```

A1 验收时 JNI 引用 11 项、其余 62 项；A2 首片引用 19 项（新增 Transport 与错误读取），其余 54 项在 `bridge/deferred-abi.json` 按阶段登记，连接检查切片引用 20 项、待办 54 项；已验 SSH 后端为 32/42，`c53cfb8` 已提交的身份容器首片为 33/41；SAF 导入切片未增加公共 ABI 引用；IdentityFile 来源查询切片为 34/41，主机信任记录快照切片为 38/41、79 个公共符号；事务/移除绑定为 39/41、80 个公共符号（§9.37）；通道取消入口替换旧 shell 引用后为 39/42、81 个公共符号（§9.41）；未验收 SFTP 工作区为 40/42、82 个符号（§9.44）。以当前检出的 `check-abi-android.py` 输出为准，不能把未绑定能力称为已实现。变异脚本会在指定设备重新安装 debug APK，仅用于测试设备。原生变异分别破坏 UTF-8 配置往返、NUL 拒绝与 VT feed，目标断言红、同一 APK 独立对照绿、恢复后绿并校验源码 SHA256；中断会恢复带标记的源码。

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

八项定向与十组变异已通过；范围见[§9.33](../docs/reference/android-implementation.md#933-ssh-身份设置与公钥信息2026-10-30)。主机引用与身份映射已按 §9.35 接入，known_hosts 管理已由 §9.39 交付，完整偏好仍未实现。

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
