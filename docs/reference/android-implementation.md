# Android 最终方案实现计划

日期：2026-10-29。状态：**设计已定，A0 完成，A1 主机与配置首片已实现；完整客户端仍在开发**。页面与操作基准为唯一的
[Android HTML 原型](../design/android-prototype.html)。本文定义原生实现的模块、状态、数据边界、实施依赖与验收；HTML 的模拟主机、文件、键盘和进度不构成 Android 能力证据。

仓库约束以 [AGENTS.md](../../AGENTS.md) 为准；跨端进度见[路线图](roadmap.md)，已实现范围见[能力矩阵](capability-matrix.md)。A0 与 §9.2 所列 A1 首片已有代码和模拟器证据；其余行为仍为拟实现，不能从设计表推断功能已经可用。

## 1. 交付范围与当前基础

首版交付主机首页、普通 SSH 与本地系统 shell、单屏终端与会话切换、统一文件浏览与复制粘贴、五类设置。主机不分工作/个人组；文件默认点按打开、长按多选；终端不放常驻加号，只有一个键盘开关。应用不自动连接、不自动恢复、不安装远端 daemon。

| 能力 | 当前代码与证据 | Android 需要补齐 |
|---|---|---|
| 宿主 | Compose 主机列表/搜索/添加/编辑、配置页与导入入口；debug APK | A1 细节补齐、会话生命周期、终端/文件与完整设置 |
| 终端 | `terminal/android` 已有 VT 交叉构建与 JNI 装载；vendor pin 为 `81598385d608dda4946093d6d6a234d204a2f225` | 真实 surface、渲染、输入法、字体与宽字符 |
| Transport | `core/include/ghosterm/transport.h`；Core 已经 NDK/Bionic 编译，POSIX PTY 与 SSH shell 已有桌面运行证据 | Android JNI owner、真 PTY、线程、退出和尺寸验证；编译不等于设备运行 |
| SSH 认证 | `ssh_auth.h` 有可取消、分步认证；Apple 已统一接线 | DNS、主机密钥确认、Keystore、请求归属和取消 |
| 配置 | Core 解析/求值/序列化；A1 已接私有目录、保存事务与导入入口，非连续重复指令有拒绝写回保护 | 显式/继承来源、别名与 Include 编辑归属、SAF UI 验收、身份引用映射；共享编辑缺陷待修复 |
| SFTP | `sftp.h` 有目录/属性/操作、路径式上传下载、暂停取消 | 本地/SAF 适配、四种复制方向、任务编排、非覆盖写入能力 |
| tmux | `tmux.h` 与真实 macOS/SSH 验证已存在 | Android 管理 UI、专用 owner、列表与 attach 接线 |
| 环境包 | `runtime-payload/` 只有规范，无安装器 | 独立后续 PoC；不作为首版 SSH/系统 shell 的依赖 |
| CI 与出包 | 现有工作流只构建 macOS | Android 构建、设备测试、APK 与证据归档 |

Apple 的 `SessionController`、`SSHAuthenticationFlow`、`FilesPanelModel`、`TransferManager` 可供行为对照，**不能由 Android 引用或移植为跨端 UI**。已有 C 接口不等于 Android 已可使用：A0 已隔离交叉构建：显式传入 Android libssh2 target，不读取宿主 pkg-config；宿主测试目标在交叉构建默认关闭。

## 2. 页面与导航契约

原型外侧的四个预览按钮、说明栏、模拟系统外观与“重置演示”属于设计工具，**不进入 App**。产品以主机首页为根；终端和文件依附当前会话或明确选择的文件端点；设置为独立页面栈。

| 页面 | 固定入口 | 行为 |
|---|---|---|
| 主机 | 顶栏只有设置；底部本地终端、添加主机 | 搜索名称/地址，单一列表；有活动会话时显示继续入口 |
| 终端 | 顶栏返回、会话菜单；底部会话选择、文件、键盘 | 菜单仅清屏与关闭连接；辅助键条单独横向滚动 |
| 会话面板 | 当前会话、其他会话、新建会话 | 选择切换，新建进入连接选择；不把首页点击与强制新建混淆 |
| 文件 | 返回、选择、页面菜单；本机/远端位置切换 | 单击打开；长按或“选择”进入多选；待粘贴卡片常驻 |
| 设置总览 | 返回；五类设置、查看 SSH 配置、关于、恢复默认 | 分类行显示当前值；详情页返回总览，再回来源页 |

**返回优先级**：先交给正在编辑的系统 IME 收起键盘；有模态面板时关闭面板；文件选择模式退出选择；目录内返回上级；分类设置回总览；其他页面回其来源，终端回主机。首页交还系统返回行为。返回不等同断开；页面销毁不直接释放连接。预测性返回与顶栏返回应获得相同目标，取消手势不修改状态。

**会话身份**：`SessionId` 使用独立 UUID，不使用主机名充当 ID。首页点击已连接主机时回到该主机最近活动的会话；若同主机有多个连接，进入会话面板选择。“新建会话”允许同主机再开连接。主机改名不改变活动连接身份；编辑地址只影响下一次连接。HTML 以 host ID 简化会话数量，原生实现不得照搬该限制。

**连接状态**：未连接、解析地址、连接中、待主机验证、待凭据、已连接、失败、已断开分开显示。查不到状态不显示“已连接”；文件连接成功不等于终端已连接。连接中允许取消；晚到的 DNS/认证结果必须携带请求 ID 与 generation，不能落到已关闭页面或另一条会话。

## 3. 模块与线程设计

模块目录（app/bridge/terminal/domain/data 已建；下列 owner、事件泵、会话/文件模型与 TerminalView 为目标职责，尚未全部实现）：

```text
android/
  settings.gradle.kts / build.gradle.kts / gradle/libs.versions.toml
  app/src/main/                       Activity、导航、Compose 页面、资源
  domain/                             会话/主机/文件/任务状态与用例
  data/                               配置事务、设置、身份、SAF、文件后端
  bridge/src/main/cpp/                C ABI 绑定、owner 注册表、事件泵
  bridge/src/main/java/               类型化句柄包装与低频结果转换
terminal/android/
  src/main/                           TerminalView、InputConnection、surface 接缝
  native/                             VT 桥、渲染状态与原生数据通路
```

Gradle 模块引用 `terminal/android` 的 source/project，不把实现再复制进宿主。终端数据链为
`UI → TerminalSurface → Transport → Core`；配置、认证、文件控制面经对应 controller/repository 与类型化 bridge 进入 Core，不能为满足图示而让文件操作经过终端。Composable 不接触 JNI、C 句柄或 libssh2。

- **主线程**：Compose 状态、输入事件、系统授权、IME、View 挂载；不执行 DNS、磁盘、大目录扫描或阻塞 JNI。
- **终端 owner**：每条连接一个串行所有者，唯一调用 `gt_transport_read/write/resize/close`。输出直接送 VT，禁止转为 Kotlin `String`、Flow、UI 消息或逐字节 JNI 回调。
- **渲染**：读取受控的 VT 渲染状态/快照，主线程最多收到合帧的“需刷新”信号。首片选 Canvas 验证正确性，生产自绘方案在 A2 的持续输出、滚动、内存验证后固定；不足时换 GLES，但保持同一 surface 接缝。
- **文件 owner**：SFTP 使用独立 SSH 连接；目录调用不与其他线程同时使用同一会话，核心单 I/O worker 推进传输。传输任务持有后端租约，不依附文件页面。
- **全局事件泵**：只允许一个消费者调用 `gt_core_poll_events`，按 job ID 扇出；当前进度在 `message` 中编码为 `sftp done/total`，桥层集中解析与校验，页面不解析字符串。无法解析时显示进度未知，不能猜百分比。
- **事件终态可靠性**：当前 Core 事件队列容量 256，满时覆盖最旧事件，没有 job 终态查询 ABI。A6 必须验证事件消费能力，并补足终态保留/查询等共享保障；不得通过百分比或超时猜测成功。
- **关闭**：发取消、停止提交输入、owner 有界退出后释放 transport，再释放借用的 SSH，最后安全释放 surface。UI 不同步 join；JNI close 幂等，禁止裸指针反复使用。

JNI 只经 `core/include/ghosterm.h` 引入共享核心；VT C API 只在 terminal 模块内使用，这是引擎边界，不是第二套 Core ABI。句柄采用带代次的注册表，拒绝已释放 ID。字符串显式 UTF-8 编解码，拒绝内嵌 NUL；不要用 JNI modified UTF-8 直接代替文件名与终端 UTF-8。日志只记录错误码、阶段和匿名请求 ID，不写凭据或终端原文。

## 4. 主机、配置与认证

### 4.1 主机列表与编辑

`ssh_config` 是唯一主机配置真相，放在 App 私有配置目录；列表为派生视图，不另存一份 host JSON。首次没有配置显示空态；读取/解析失败显示错误与重试，不能变成空列表。`Host *` 等规则块不是可连接主机，在配置入口显示和编辑，不当作工作/个人分组。

编辑页至少包含别名、HostName、User、Port、身份选择及高级指令查看。别名/地址不能为空，端口为 1–65535；Android 不把进程 UID 用户名作为远端登录名默认值。显示继承值与当前块显式值，未触碰的字段不能回填覆盖全局规则。

**当前表单限制**：别名只允许 1–128 位 ASCII 字母/数字及点、短横线、下划线，首位必须为字母或数字；已有别名只读，改名需配置文本页。User 求值缺省可以为空，但当前保存表单要求非空，不能把列表读到空用户名写成“连接时已自动选择用户名”。连接参数目前只有 HostName/User/Port，身份选择尚未接入。

A1 收尾须明确编辑对象，不能仅以最终生效值反推原配置：

| 情形 | 实现契约 | 验收结果 |
|---|---|---|
| 显式值与继承值 | Core 提供字段来源/目标块元数据，桥层转换；UI 显示“此主机设置”或继承来源，脏字段单独跟踪 | 未编辑的继承字段不生成新指令；设置为与继承值相同的显式覆盖也能准确表达 |
| 多别名 `Host a b` | 编辑前列出同块别名，说明修改影响整块；不得在不提示时只展示 `a` 却修改 `b` | 保存后两者求值与确认范围一致，其他块逐字不变 |
| 重名块与 Include | 来源必须能区分主文件/包含文件及块；首版只允许归属明确的主文件块表单编辑，包含文件显示来源和限制 | 不向错误块追加相同别名，不改 Include 外部源；无法安全定位时明确拒绝 |
| 别名重命名 | 仅改目标 Host 行，检查大小写无关冲突，保留其他别名/行；主机引用与活动会话身份分别处理 | 原始地址/User/Port 与未知指令不变，活动连接不被重新创建；不能用 reset 整块重建替代改名 |
| 复杂重复指令 | 先修共享编辑器按实际匹配行修改；Android 原文检查继续防止无关行变化 | 重复字段之间的注释、未知指令和其他字段原字节保留；同时回归 Apple 调用方 |

当前 `config.h` 尚无完整字段来源/块归属查询契约；不得在 Kotlin 写第二份 SSH 配置求值器。需要新增 C 接口时，先定义所有权与失效规则，再同步 C/Apple/JNI 与 ABI 对拍。上述表格是待实现条件，不表示现有表单已经具备这些能力。

只给真正接入的连接选项提供可用控件。C 的配置求值能返回 ProxyJump、转发、ServerAlive 等字段，不代表 SSH 执行层支持它们；检测到影响连接且未支持的显式配置时，列出指令并报不支持，不悄悄绕过。未知指令仍需无损保留。

### 4.2 配置写入事务

Android 新实现必须按以下顺序完成，不能照搬现有 Apple `ConfigStore` 的失败处理：

1. 读取原始字节、文件身份和内容摘要；UI 编辑生成稀疏修改。
2. 保存前重新检查摘要/身份；发生外部变化则提示重载，不覆盖。
3. 强制备份原字节；备份失败就停止。首次创建没有原文件时不伪造备份。
4. 同目录独占临时文件写入、flush/fsync；用平台支持的原子替换提交，不能先删目标再移动；同步目录并清理失败临时文件。
5. 提交前失败保留旧文件；提交成功后重新读取、解析并更新摘要。若替换已成功但目录同步或回读失败，必须说明“已提交但验证/持久化失败”，不能谎称原文件没变，也不能盲目回滚覆盖后来变化。

当前锁保护同进程仓库及遵守 `.config.lock` 的协作写入者，摘要复核覆盖加载后和暂存期间的变化；它不是针对任意不遵守锁的写入者的文件系统 compare-and-swap。最后一次复核与 rename 之间仍有竞态窗口，不能把 API 36 模拟器故障注入结果扩大成任意并发写入都不会丢失。私有目录/单一写入入口是当前约束；将来开放其他写入入口须共享同一锁与事务协议。

导入通过系统文件选择器读取授权文档，在私有目录保存被用户确认的副本；不承诺持续修改外部 provider 文档。`Include` 与 `~` 在 core 中有桌面路径语义，Android 需先验证私有 HOME 与 Include 基准；首片遇到不可访问的 Include 明确报错，不静默漏掉规则，也不重写用户配置为自造语义。

**现有 macOS 缺口**：`ConfigStore.save` 的备份使用 `try?`，写回先 remove 再 move；它不满足强制备份和完整原子替换。此为待修复实现缺口，见[路线图](roadmap.md)，不是降低规则的理由。

### 4.3 身份与主机验证

使用 `gt_ssh_authenticator_open/step/info/decide/supply/take/close`。DNS 在后台完成，原始 hostname 保留给 hostkey 身份；总 deadline 跨越用户提示，切换页面/取消不重置计时。未知主机显示地址、端口、算法、SHA256 指纹，用户明确选择信任或拒绝；密钥变化直接阻止并提示核对，禁止自动覆盖 known_hosts。

先发现服务器认证方式，再索取受支持的凭据。无口令钥匙不弹口令框；格式未知不伪装成“口令错误”。当前公有入口支持密码和 PEM 私钥，不能因为方法掩码存在就声称已支持任意 keyboard-interactive、agent、硬件签名或 hostbased。

Android Keystore 的不可导出私钥不能直接转成 `gt_ssh_auth_private_key` 所需 PEM。首版拟用 Keystore 管理加密主密钥，SSH 导入材料只进入其保护的加密容器；授权后短暂解密至内存供 core 使用，凭据页面/配置只保存引用，不保留明文文件。不能声称这种方案是 SSH 私钥永不离开硬件的签名代理；后者需要新的共享签名契约。[Keystore 官方说明](https://developer.android.com/privacy-and-security/keystore)

凭据使用可清理缓冲，失败/取消后擦除；禁用凭据备份和调试日志。Keystore 失效或设备凭据变化应要求重新导入/解锁，不回落明文。导入权限属于临时授权，不自动删除用户原文件。身份页面提供名称、类型、指纹、引用主机与移除确认；主机验证页面只展示真实记录，移除信任后下次重新核对。

## 5. 终端与移动端输入

`TerminalSurface` 只接收字节、尺寸、外观、焦点和输入，不知道 SSH、本地或 tmux。VT 构建只使用锁定 pin 的头与库；不得把 Apple full libghostty 的配置/Metal shim 搬来假装 Android 有渲染后端。

| 项目 | 实现要求 |
|---|---|
| 渲染 | 真 VT 网格；ANSI 颜色、宽字符、组合字符、emoji、反色/下划线、光标、选区、滚动与备用屏幕；不以日志 TextView 代替 |
| 字体 | 首版系统等宽字体；测量真实字形，宽字符占格一致；字号变化更新像素与 cols/rows |
| 尺寸 | 内容区扣除状态栏、底部控制、辅助键及 IME insets；同一尺寸下发 VT 与 transport；合帧且拒绝零尺寸 |
| 输入 | `TerminalView` 实现 InputConnection；组合串只本地显示，commit 才编码发送；中文提交不套 bracketed paste |
| 辅助键 | Esc/Ctrl/Tab/四方向，加 `/`、`-`；扩展增加 Home/End/PgUp/PgDn；根据 VT 模式编码，不硬编码所有方向键字节 |
| Ctrl | 单次发送后释放；锁定再点解除；会话切换/断开清修饰状态，防串到另一会话 |
| 系统键盘 | 一个开关；状态来自实际 IME 可见性；外接键盘仍可输入；自动弹出只作用于新连接 |
| 文本粘贴 | 与文件剪贴板分离；走 VT paste 编码，尊重 bracketed paste；不能把文件卡片贴入 shell |
| 清屏 | 只清本地终端显示/滚动历史，明确作用范围；不向 shell 注入 `clear`、Ctrl+C 或回车，不中断命令 |
| 后台 | 不可见 surface 停止绘制但保持 owner 读流；断线显示实际状态；进程被杀不伪装会话仍连接 |

InputConnection 应覆盖 composing/commit/deleteSurroundingText/sendKeyEvent、组合取消与焦点切换，不能只支持示意键盘的点击。[BaseInputConnection](https://developer.android.com/reference/android/view/inputmethod/BaseInputConnection)

首版本地终端显式尝试 `/system/bin/sh`，设置私有 HOME/cwd 与 TERM，不把 `/bin/sh` 桌面默认值带进 Android；只获得 App UID 的权限。若设备不支持 PTY 或 shell，显示不支持与原因；不偷偷换成无 PTY 命令执行。Termux/proot 另列 A9，不阻塞 SSH 交付。

## 6. 文件浏览与复制粘贴

### 6.1 位置与浏览模型

`FileLocation = endpoint + directory`，端点分为应用私有目录、本机 SAF 授权树、远端主机。远端身份含配置引用与解析后的 host/user/port，不用显示名称、当前终端或文件名推断来源。使用独立 `FileEntryId`，文件原始标识与展示名分开；不把 `content://` 转换为猜测的 Linux 路径。

本机默认显示应用文件，并提供“选择设备文件夹”；原型中的 Download/Documents 只是模拟条目，不能据此申请全盘权限。SAF 返回 URI 授权，重启后使用持久授权前需核验；撤销、移动和 provider 失效都显示错误与重新选择入口。[Android 文件访问说明](https://developer.android.com/training/data-storage/shared/documents-files)

远端首次进入查询真实 home/realpath；不把原型 `/home/deploy/ghosterm` 当默认路径，也不声称跟随 shell cwd。每个端点保存本次进程内最后目录，切换位置清选择、保留复制清单。请求携带位置 generation，旧目录晚到结果不覆盖新目录。加载中、合法空目录、权限拒绝、断线分别呈现。

默认点击目录进入、点击文件只读预览/打开；远端预览按需下载受控临时副本，大文件限额并明确提示。长按通过系统手势进入选择并选中当前项，滚动取消长按；TalkBack 和外接键盘可用“选择”入口。选择模式中点按只切换勾选，“全选”只作用于当前可见过滤结果，不递归选择目录内容；复制按钮在零项时禁用。

### 6.2 应用内待粘贴清单

```text
ClipboardSnapshot
  id: UUID
  sourceEndpoint / sourceDirectory
  items: [entryId, rawName, kind, size?, mtime?]
  createdAt
PasteRequest
  id: UUID
  clipboardId + 独立的源条目快照
  destinationEndpoint / destinationDirectory
  conflictPolicy + perItemDecisions
```

复制只创建清单，不传输；完成后退出选择模式。卡片显示来源主机/本机、源路径、名称摘要、数量和“查看清单”；底部“粘贴到当前目录”。切到设置、终端、另一主机仍保留；新复制明确替换旧清单。它不是 Android 系统文本剪贴板，不向其他 App 泄露远端路径。

粘贴前固定源和目标快照，确认页列出两端路径、条目数、冲突及本次策略。确认后导航不能更改目的地。来源失效、目标只读、同一目录无意义复制、目录复制到自身/后代、权限丢失、已有任务占用等均给出明确原因；不能只把按钮灰掉。

### 6.3 四种方向与 ABI 缺口

| 源 → 目标 | 首版路径 | 限制与补齐 |
|---|---|---|
| 本机 → 本机 | 私有文件或 SAF 的流式复制 | provider 不支持创建/写入时明确失败；不依赖真实路径 |
| 本机 → 远端 | 私有文件可走上传；SAF 先流式写入 App 私有暂存，再上传 | 现有 upload 只有路径参数；空间不足在写远端前报错 |
| 远端 → 本机 | 下载到任务私有暂存，再提交至私有文件/SAF 文档 | 两阶段都成功才标完成；下载完成不代表 SAF 写入成功 |
| 远端 → 远端 | 同主机和跨主机均先下载暂存，再上传目标专用连接 | 明示“经本机中转”；不执行拼接的 `cp/scp`；首版不声称服务器端复制 |

暂存按任务 UUID 管理、单文件逐项推进、有容量预算，退出/失败清理；数据敏感性与原文件相同，不进共享缓存或日志。应用自身设备目录与仓库开发 `tmp/` 是不同边界。关闭文件页面不取消任务；清空/更换清单也不取消已确认任务。

**阻塞完整交付的 Core 缺口**：`gt_sftp_upload/download` 没有 exclusive-create、fd/URI 或非覆盖提交参数。上传当前以 WRITE|CREAT 打开，下载以 `fopen(...,"wb")` 打开；不能把“先 stat 再写入”当成防覆盖，也不能把现有 upload 当作可靠覆盖（没有 TRUNC 契约）。A5 必须先补非覆盖创建/提交能力或等价可证明的契约，再启用正式复制。新增 C ABI 经聚合头、各端绑定及 ABI 对拍同步，不通过 Android JNI 直接调用 libssh2 绕过去。

### 6.4 冲突、递归与任务状态

- 默认“逐项确认”，仅提供“保留两者”“跳过”；首版不提供覆盖/移动/删除源。目录同名同样询问，不隐式合并。重名自动编号要独占创建并处理重试，不仅检查字符串列表。
- 建任务时扫描目录并保留相对路径、空目录；拒绝 `..`、绝对子路径和越过授权根的路径。首版不跟随符号链接递归；当前缺少创建链接 ABI，遇到链接明确报告不支持并让用户跳过，不能默默复制目标文件。
- 源在扫描后变化：执行前校验已知 size/mtime/type；不能证明一致时提示变化，不能声称是原始快照。大目录扫描可取消，数量未知时用不定进度。
- 状态包含 queued/scanning/awaitingDecision/running/paused/succeeded/partial/failed/cancelled/interrupted；UI 展示已复制、跳过、失败数量。核心 `OK/FAILED/CANCELLED` 只决定单 job 结果，父任务由全部子项汇总。
- 两阶段中转的传输字节与逻辑完成量分开，不能下载一半就显示整项完成；总量未知不伪造 ETA。暂停为分块协作式，不承诺瞬停；取消后列明已完成项与待清理临时项。
- 断线不自动恢复/重传；重试由用户发起，复核来源、目标与冲突。进程死亡后普通任务不恢复执行，清理清单与残留；必要的清理日志只含任务自有路径，不存凭据。
- “完成后清空”只在实际复制全部成功且至少复制一项时生效；全部跳过、部分成功、取消、失败保留清单。比较 clipboard UUID，旧任务结束绝不清掉后来新复制的内容。

## 7. 设置数据与作用域

使用版本化、类型化持久偏好（拟采用 DataStore），不把 Activity 内存或 HTML 全局变量当存储。页面行显示当前值；读写失败可见且不虚报保存成功。首次值按下表；非法值回默认并记录不含隐私的诊断。更改立即更新预览；写入失败回滚或明确显示未保存状态。

| 分类 / 键 | 默认值 | 可选值与生效范围 |
|---|---|---|
| 外观 `appearance` | system | 跟随系统/浅/深；App 全部页面即时更新 |
| 外观 `terminalTheme` | graphite | 石墨蓝/墨绿/纸白；全部已有及新 surface 即时更新 |
| 外观 `fontSize` | 12 sp | 10–20，步长 1；重算网格，连接与输出保持 |
| 外观 `lineHeight` | standard | 紧凑 1.5/标准 1.85/宽松 2.15；重算尺寸 |
| 外观 `cursor` | block | 方块/竖线/下划线；作为应用默认，终端程序的有效光标模式按 VT 契约处理 |
| 外观 `blink` | true | 闪烁开/关；仅可见且聚焦终端闪烁 |
| 输入 `auxKeys` | true | 显示辅助键条；关闭时释放未发送的修饰状态 |
| 输入 `keySet` | basic | 常用/扩展；键条横向滚动，不缩小触点 |
| 输入 `ctrlMode` | once | 单次/锁定；不修改系统 IME |
| 输入 `autoKeyboard` | false | 只在新连接可输入时请求显示；已有会话切换不自动弹出 |
| 会话 `keepAwake` | false | 仅前台终端 Window 保持屏幕常亮；进入文件/设置或后台即撤销 |
| 文件 `showHidden` | false | 本机/远端显示点开头文件，立即重新过滤 |
| 文件 `sortBy` | name | 名称升序/大小降序；目录无大小不伪造为零字节文件，未知值最后，名称作为稳定次序 |
| 文件 `foldersFirst` | true | 目录优先，其余按所选排序 |
| 文件 `conflict` | ask | 逐项确认/保留两者/跳过；只决定新粘贴任务初始策略 |
| 文件 `clipboardAfterPaste` | keep | 保留/清空已使用清单；按 §6.4 的成功与身份条件 |

五个分类保持原型：外观与终端、输入与键盘、会话与连接、文件操作、身份与主机验证。后两类连接资料/身份信息不是偏好开关；主机地址、用户名、端口仍由配置编辑管理。没有分组开关、自动重连开关、假装支持的全局保活开关。

恢复默认要确认，只重置上述偏好；不删主机、身份、known_hosts、文件、运行会话、传输和复制清单。关于页显示实际版本/commit、许可与来源；不复用 macOS 专用更新地址，也不出现没有 Android 发布服务支持的更新安装按钮。

## 8. 生命周期、后台与系统适配

首版保证前台及页面/Activity 重建内连接所有权稳定，SessionRegistry 不绑定 Composable 销毁。旋转、主题切换、IME 出入只改变 View 与尺寸，不重连、不丢输出、不重复认证。内存压力与 App 进程死亡后重新启动回主机页，旧会话显示不得恢复为已连接。

普通 SSH 不承诺 App 后台常驻。若提供离开前台后的可见传输服务，应单独验证服务类型、通知、用户停止、超时和取消；不能借 `dataSync` 宣称无限 SSH 保活，Android 对该类前台服务有时限。[前台服务时限](https://developer.android.com/develop/background-work/services/fgs/timeout)

tmux 在 A8 接入，共享 `gt_tmux_*` 管理与普通 transport attach；用户显式打开列表才探测，缺失、版本不支持、拒绝与未知分开。HTML 的固定“未安装”只是演示分支，原生必须展示真实结果。关闭只断客户端，不结束后台 session；结束是单独确认动作。重新连接必须取新列表，禁止按旧 ID/名称恢复。手机本地 tmux 不因远端支持而自动成为可用能力。

A0 已锁定 minSdk 29、compile/target 36、arm64-v8a 及工具链版本（§9.1）；现有运行证据仅为 API 36 arm64、4 KB 页模拟器。API 29 声明下限、真机与 16 KB page size 装载均须在 A10 验证；x86_64 须先提供对应 native 库。不能只调整 APK zip 对齐就声明全 native 依赖可运行。[16 KB 页面支持](https://developer.android.com/guide/practices/page-sizes)

采用 Material 3 页面/面板/开关/选择控件，保持石墨色系；触点至少 48dp，字体响应系统缩放，危险动作有文字说明。320dp 窄屏、横屏、分屏、字体放大、TalkBack、硬件键盘和深浅色均纳入验收。状态栏/导航栏/IME insets 不得重复扣除或遮挡操作区。[触点与语义](https://developer.android.com/develop/ui/compose/accessibility/api-defaults)、[窗口 Insets](https://developer.android.com/develop/ui/compose/system/insets-ui)

## 9. 分阶段实施与依赖

每片直接提交 main，原生构建与对应检查通过后才勾完成；C ABI 变化同步 Apple 绑定并真实编译。A0 冒烟和 A1 配置/页面验收已实现；其余业务验收表仍是开发要求，不能当作运行结果。

| 切片 | 依赖 | 可审阅交付与退出条件 | 状态 |
|---|---|---|---|
| A0 工程与构建 | 无 | 原生空壳 APK；锁工具链/ABI/API；隔离 NDK sysroot 与宿主库；设备加载 core/vt；JNI 引用集合纳入 ABI 检查 | 已完成：API 36 arm64 模拟器，见 §9.1 |
| A1 导航与主机 | A0 | 首页/设置/页面返回；配置读取、搜索、增改、备份失败阻断、冲突拒绝、原子提交；假数据只在调试 fixture | 进行中：主机与配置首片见 §9.2 |
| A2 本地终端 | A0 | `/system/bin/sh` 真 PTY→VT→像素；IME/辅助键/resize/暂停绘制/有界关闭；确认 renderer 方案 | 未开始 |
| A3 SSH 与身份 | A1、A2 | DNS→hostkey→凭据→shell；Keystore 安全容器；取消归属、超时、失效身份、两会话隔离；真 sshd | 未开始 |
| A4 文件浏览 | A3 | 本机私有目录/SAF/远端；真实预览与路径；点按/长按、多选；位置 generation；独立 SFTP owner | 未开始 |
| A5 复制基础 | A0、A4 | 先补共享非覆盖创建/提交契约与各端绑定；本地流与暂存；四种方向、冲突、空目录/链接规则、真实结果 | 未开始 |
| A6 复制任务 | A5 | 全局事件泵与终态可靠性、父子进度、暂停取消、失败重试、断线、清单 UUID、关闭页面不丢任务 | 未开始 |
| A7 完整设置 | A1、A2、A6 | 16 项偏好持久化、即时应用与重启读取；身份/主机验证详情；恢复不误删会话资料 | 未开始 |
| A8 可选 tmux | A3 | 真能力探测/列表/管理/attach；任务 PID 保留、未知结果、断线重新选择；不影响普通入口 | 未开始 |
| A9 环境包 PoC | A2，独立后续 | 系统执行限制验证；Termux/proot 来源/哈希/启动/PTY；成功后才排安装 UI，不默认为可交付 | 延后 |
| A10 验收与发行 | A1–A8 | 真机与模拟器矩阵、APK 自包含、安全与隐私检查、清单/许可、CI 实际运行；失败项不记绿 | 未开始 |

A1 的视觉壳可与 A2 的底层实现解耦，但不能用演示终端代替 A2 完成条件。A5 是数据安全前置，不应先做“看起来完成”的粘贴按钮再补防覆盖。

### 9.1 A0 实施记录（2026-10-29）

- 已建 `:app`、`:bridge`、`:terminal`，Compose 首页/设置基础与真实组件版本；没有虚拟主机或假连接。后续产品功能仍按 A1–A8 实施。
- 锁定 JDK 17、Gradle 8.13 + wrapper SHA256、AGP 8.13.2、Kotlin 2.2.21、Compose BOM 2025.10.01、NDK 28.2.13676358、CMake 3.22.1、Zig 0.16.0。compile/target 36、min 29、ABI arm64-v8a；下限声明不等于 API 29 已实测。[AGP 兼容表](https://developer.android.com/build/releases/agp-8-13-0-release-notes)
- OpenSSL 3.5.4 / libssh2 1.11.1 源码哈希锁定并用 NDK 构建；VT 在隔离 pin 副本交叉编译；收据验证输入与 archive SHA256，APK 附各许可。Core 不探测宿主库。
- Bionic 缺少 `GLOB_TILDE`：Core 已自行 `expand_tilde`，改用 POSIX `glob(...,0,...)`，没有删掉 tilde 语义；既有 Include 变异四组与回滚通过。
- `bash scripts/build-android.sh` 生成主 APK 与测试 APK。API 36 arm64 模拟器（4 KB 页）3 项真实 JNI 测试通过：配置中文/emoji/CRLF 逐字节往返、NUL 拒绝、VT 光标指令。
- JNI 引用 4 个公共函数、67 个暂未使用逐项登记；3 组 ABI 变异与 3 组设备 JNI 变异通过，均目标红/独立对照绿/恢复绿并核对 SHA256。Core 32 项闸门和 ABI 71 项通过。
- **未验证**：真实手机、API 29、16 KB 页运行、x86_64、终端绘制/输入/PTY/SSH、后台与性能。没有 Android workflow；A10 仍未完成。

可复现命令、SDK 准备与 APK 路径见 [Android README](../../android/README.md)。首片 `probeCursor` 只做 ABI 冒烟，A2 输出必须留在 native transport→VT，不得复用成 Kotlin 字节搬运通道。

### 9.2 A1 主机与配置首片（2026-10-29）

- 新增 `:domain` / `:data`，原生主机首页从真实私有 `home/.ssh/config` 派生；搜索名称/地址、添加与稀疏编辑、SSH 配置文本编辑、设置/关于导航及未保存返回确认已接入。没有生产假主机；终端连接入口暂显示功能尚未接入，不伪造会话。
- 配置/用户名/端口求值均经 Core；JNI 只传 UTF-8 bytes，不输出裸指针。绑定当前 9 项，62 项登记待办。普通主机排除通配/否定规则，缺省远端用户名保留空，不使用 Android UID 名称。
- 未改字段不写回；保留未知指令、注释与其他块。现有 Core 对非连续重复指令的编辑可能破坏中间行，因此保存前校验非目标行保持原文；复杂块拒绝表单写回，配置页仍可完整编辑。该保护不代表 Core 问题已经修复。
- 写盘采用进程锁 + 文件锁、文件身份与 SHA256 复核、强制独占备份/同步、同目录独占暂存/同步、提交前再次检查与 `ATOMIC_MOVE` 替换。备份或替换失败保留原文件；替换后目录同步失败明确报告“已替换但同步失败”，不谎称原文未改。
- `HOME` 仅在 Android 应用进程内设置为私有 home；相对 Include 和 `~/.ssh` 仍走 Core 原有语义。配置缺失显示空态，目录/读取/无效 UTF-8/不可访问 Include 显示错误。
- 系统文档选择器读取最多 1 MiB 的配置，确认后备份并替换私有副本，不写回外部 provider。已有测试验证存储与 Include；完整 SAF 选择器驱动与取消场景仍待补齐。
- API 36 arm64 模拟器：完整设备套件 17 项通过（13 项配置事务/解析、1 项 Compose 页面流、3 项 A0 JNI），配置/页面变异 11 组目标红、同 APK 独立对照绿、恢复绿且源码 SHA256 一致。Core 32 项、C/Apple/JNI ABI 与 JNI 对拍变异通过。
- **A1 未完成项**：表单明确区分块内显式值与继承值、别名重命名/多别名及 Include 块的编辑归属、SAF 导入 UI 验收、配置错误状态的完整页面验证；身份选择依赖 A3。当前不可勾整个 A1。

### 9.3 当前代码入口与 A1 退出清单

实现基线：`4ed1308`。Android 路径均为相应模块下的实际源文件；它们只证明已接入的 A0/A1 能力。

| 职责 | 代码入口 |
|---|---|
| 页面与表单、导入选择器 | `android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt` |
| 导航状态、未保存确认、后台配置调用 | 同目录 `HostsViewModel.kt` |
| App 私有 HOME | 同目录 `GhostermApplication.kt`，只改变 Android 应用进程环境 |
| 主机派生模型与表单稀疏变化 | `android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/Hosts.kt` |
| 校验、摘要、备份、原子替换和原文保护 | `android/data/src/main/java/com/hanxi/ghosterm/data/ConfigRepository.kt` |
| 配置求值与编辑桥 | `android/bridge/src/main/cpp/config_jni.c`、`core_jni.c`；同模块 `ConfigBridge.kt` / `CoreLibrary.kt` |
| VT 装载/指令冒烟 | `terminal/android/native/vt_jni.c`；尚无生产 TerminalView |

A1 完成前还须通过以下生产路径验收：

- [ ] §4.1 的字段来源、别名与包含文件归属；既有表单校验错误显示在当前编辑页且草稿保留。
- [ ] 系统文件选择器选中有效 UTF-8 配置→预览确认→保存→首页刷新；对外部 provider 不发生写回。
- [ ] 选择器取消、预览取消均不修改配置；超过 1 MiB、无权限、无效编码、无效配置与私钥容器显示原因并保留旧文件。
- [ ] 缺配置显示合法空态；读取失败、Include 缺失与保存冲突显示错误/重载入口，不显示空成功态；返回继续编辑/放弃走真实按钮。
- [ ] 新增断言均有目标红、同变异独立对照绿、恢复绿及 SHA256；现有 17 项设备测试与 11 组配置变异保持通过。

身份选择的完整验收留在 A3，不以一个无后端的下拉框计为完成。

## 10. 验收与变异清单

| 面 | 必须观测的真实结果 | 新增断言的变异示例 |
|---|---|---|
| 导航 | 零/单/多会话，唯一设置/键盘，无终端加号；返回不关连接 | 错把返回接关闭→PID 断言红；正确新建对照绿 |
| 配置 | 未编辑逐字节不变；编辑仅目标行变化；备份失败保留原文件 | 跳过备份失败/先删除目标→故障注入红；合法写入对照绿 |
| 认证 | 未知≠拒绝，变化不信任；请求取消后不再消费凭据 | 省略请求归属/错误映射→认证目标红；其他连接仍可用 |
| 字节与输入 | 真 shell 输出入 VT；中文 composition 不发送，commit 正确；背压不卡 UI | 短路 feed/丢输入尾/提交当粘贴→网格/字节红 |
| 生命周期 | 旋转/切页/设置变化后同一 owner、PID、网格和输入落点 | 每次重组建会话→身份/PID 红；新连接对照绿 |
| 文件手势 | 点按打开；长按选择无误打开；滚动取消；读屏可选择 | 把默认动作改成选中→打开断言红；长按对照绿 |
| 文件端点 | 跨主机不串源，目标目录在确认后固定；撤权明确失败 | 从当前页取目标/源→双服务器内容哈希红 |
| 复制内容 | 四方向逐字节/哈希一致；空目录、重名、部分失败、非 ASCII 路径 | 跳过写/折失败成功/并发创建覆盖→目标红；其他目录对照绿 |
| 任务清单 | 原文件保留；旧任务不清新清单；取消/全跳过不称复制成功 | 无条件清 clipboard/折 CANCELLED 为 OK→状态断言红 |
| 设置 | 16 项默认、持久化、实际已有 surface 与文件列表生效；恢复只重置偏好 | 只改预览/未下发尺寸→真实终端像素和 stty 红 |
| tmux | 客户端关闭任务不死；旧列表不可 attach；未知不报成功 | 关闭变 kill/复用过期 ID→真实 tmux 任务与身份红 |
| 包 | ABI 与设备一致、依赖齐全、无开发机绝对路径、无私钥/环境包 | 删 native 库/塞入 payload→装载/包校验红 |

性能证据至少记录设备/API/ABI、持续输出吞吐、帧耗时、峰值内存、空闲后台 CPU、输入到回显延迟与取消耗时。先测 A2 基线再锁门限；不能用模拟器或静态截图冒充真机流畅度。真正的 UI 测试要驱动按钮、手势与系统 IME，数据模型测试不能代替触控命中与像素检查。

现有可执行检查：`bash scripts/gate.sh`、`bash scripts/check-abi-apple.sh`；C 变化按实际面执行 SSH/SFTP/config/transport/tmux 变异与 Apple 构建。Android 构建入口为 `bash scripts/build-android.sh`；`check-abi-android.py`、`mutation-check-android-abi.py`、`mutation-check-android-native.py` 与 `mutation-check-android-config.py` 已实现。设备测试需显式选定设备；A10 再接入 CI，禁止把本机 Core 绿记为 Android 绿。

## 11. 初始文档审计与后续校正

以下保留初始文档审计基线 `99b8b3c` 的结论。涉及 Android 工程/绑定的历史结论已在表内注明后续变化，当前实现以 §9.1–§9.3 为准；macOS 缺口没有因此自动修复。

| 旧说法 | 对照代码后的结论 |
|---|---|
| Android 只需接 UI、旧 attachment 契约仍待开发 | 初始基线无 Android 工程；A0/A1 后已有构建与主机配置首片。旧协议已退役，使用普通 Transport + 可选 tmux；交互终端/输入/文件仍待开发 |
| macOS 文件仍用 SFTPBrowser / DualPaneFilesView | 均已退役，使用 WorkspaceLayout/WorkspaceHostView 与统一文件窗格 |
| 文件队列 S4/S5/S6 未动 | macOS 队列、拖放、预览已实现；iOS 和远端拖出 Finder 等仍未完成 |
| macOS 标签重排尚无触发面 | 当前顶部/侧边按 UUID 重排，唯一加号；精简侧栏已移除 |
| 64/70 个 ABI、68 个 Apple 引用 | 当前 71 个公共符号，Apple 69 个引用 + 2 个登记未用；数量以脚本实际输出为准 |
| JNI/NAPI 已与 Swift 一起对拍 | 初始基线仅 C/Apple；A1 后已有 JNI 9 项引用、62 项登记待办及对拍脚本。NAPI 尚未实现 |
| ConfigStore 已保证强制备份与原子替换 | 备份异常被忽略、先删后移；能力降为部分实现，修复留在路线图 |
| 私钥总是从 Keychain 导入后读取 | 当前 SSHCredentialSelector 从受限文件读 PEM，Keychain 主要保存口令/密码；完整私钥安全存储接入仍需补齐 |
| 像素验证待屏幕录制权限 | 自进程视图像素验证已实现；真实系统激活/全屏证据单独记账 |
| Apple 图标尚无 .app 打包接线 | package-mac.sh 已生成 icns 并组装自包含 .app |
| iSH 随 IPA、proot 必然绕过执行限制 | 环境包仍按需下载；Android 执行 PoC 尚无代码和设备证据 |

历史计划保留当时日期、测试数量和失败记录，并注明被后续实现替代的范围；当前进度统一以路线图和能力矩阵为入口。
