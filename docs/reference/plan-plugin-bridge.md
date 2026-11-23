# 插件桥接架构与开发规范计划

**2026-11-23 设计决定**。插件框架仍处于开发初期，本轮允许不兼容重构。Ghosterm 一次切换到 Lua，清除 QuickJS-NG、JS/TS 插件 SDK、JS/Node/Exec 插件运行档及旧包格式。所有内置插件用 Lua 从源码重写。旧实验包、安装目录、授权及运行记录均不迁移，新包重新构建、签名和授权。实际迁移进度和验收范围见第 12 节。

最小插件只需一个 `main.lua`。作者在 Lua 源码中声明元数据与处理函数，发布工具生成 JSON 清单。需要在另一台机器工作时再加 Executor，跨端数据有结构约束时再加 schema。API 参考 Rex 的 table 参数、函数回调和 `nil, err` 写法。Ghosterm 锁定官方最新 Lua 5.5 正式版 **5.5.1**，引入时记录源码 SHA-256 和许可。[Rex Lua API](https://www.superlogical.com/rex/docs/reference/lua) · [Lua 版本记录](https://www.lua.org/versions.html#5.5) · [Lua 5.5 手册](https://www.lua.org/manual/5.5/manual.html)

## 0 定位与设计决策

插件贡献业务命令、原生视图与任务逻辑。宿主保留安装、诊断、授权和生命周期管理，用户可以把快捷键绑定到已注册命令。普通本地 PTY / SSH 仍是默认入口，tmux 继续由用户显式选择。

## 1 本地与远端架构

Client 随 Ghosterm 运行，Executor 在任务所在机器运行。每侧独立部署、授权，单侧插件只声明所需角色。本机 Executor 走本地 IPC。远端走独立、无 PTY 的 SSH exec，受管 ProxyJump 逐跳认证。终端中的声明只提供候选，实际接入仍由用户确认执行目标、包和权限。

## 2 代码模块与实现接缝

复用 Core 的包快照、命令注册、Session、RPC、ResourceBroker 与 ProcessService。Lua 接入现有 runtime owner，每实例、每侧一个独立 state，由所属 worker 串行驱动。Swift、JNI 和 CLI 继续通过 `ghosterm.h` 使用不透明句柄。桌面及移动端由原生 UI 展示标准 chat、diff、table、task、form 和资源视图。缺能力返回 `GT_ERR_UNSUPPORTED`。

## 3 语言与运行时

- 只装载经校验的包内 UTF-8 源码。多文件可用受控 `require` 引用同一快照内的模块。禁用字节码、动态加载和原生扩展。
- 标准库按函数白名单开放。脚本通过宿主能力访问文件、进程、网络与界面。禁止直接使用 `io`、`os`、`debug`、`package` 及系统模块路径。
- 用 `lua_Alloc` 计入实例和宿主内存预算，用指令 hook 检查取消与执行时限。C 服务另设输入、输出与时间上限。超预算后由 owner 停止实例，脚本捕获错误也不能续跑或提交晚到结果。
- Lua 调用在 C 保护边界内执行。调用 `ctx.rpc.call(...)` 等异步宿主服务时挂起当前协程，完成消息回到所属 worker 后复核代次与授权，再恢复执行。首版不引入 `ltask`。只有实测表明多个 Lua state 在现有 worker 上的调度成为瓶颈时，才重新评估。 [ltask 上游文档](https://github.com/cloudwu/ltask)
- 所有内置插件通过同一 Lua API 工作。Codex 和 Claude 的 Lua 插件通过受控进程服务调用已安装的官方 CLI。移除仓库里的 Node 桥、Agent SDK 依赖和 JS 适配代码。先验证锁定 CLI 支持所需协议，再声明对应能力。CLI 的版本与进程授权独立管理。

## 4 插件包与部署规范

作者在包的主入口（默认 `main.lua`）开头用 `local manifest = { ... }` 声明命令、权限和 Lua API 兼容范围。双侧包也只在主入口声明一次，并显式指定 Executor 入口。打包工具只解析这段静态 table 字面量，支持字符串、整数、布尔值、数组和对象。遇到重复键、函数调用、变量引用或计算表达式时报告源码位置。检查和打包阶段不执行插件代码。

打包工具生成 `manifest.json` 和完整性清单。生成物记录清单 schema 版本、Bridge wire 版本及各角色的 Lua 入口，签名发布时绑定元数据与包内文件摘要。原生安装器仅凭已校验的发布元数据即可展示权限和检查兼容性，用户授权后才加载 Lua。运行时以已校验的清单为准，并按角色核对入口导出的命令处理函数。缺少声明的处理函数或出现额外处理函数时拒绝激活。

发布包还应声明发布者和许可，签名绑定包内源码、schema 与资源的完整性清单。沿用只读版本目录、原子安装和升级前的权限差异确认。签名信任、安装确认与运行授权分别判断。Lua 版使用独立的安装目录和授权记录。导入旧格式时提示重新安装 Lua 版。旧目录与用户数据保持原位，清理由用户显式发起。

## 5 插件开发规范

每个模块用 `local` 保持私有状态，通过 `return` 导出公开函数。多文件用包内 `require` 加载，模块顶层只做定义或装配。入口模块只返回命令处理函数表，触发时宿主传入当前调用的 `ctx` 和经过 schema 校验的 `args`。

```lua
-- main.lua
local manifest = {
  id = "org.example.hello",
  version = "0.1.0",
  api = "1",
  publisher = "Example",
  license = "MIT",
  client = { requires = {} },
  commands = {
    { path = { "hello" }, title = "问好" },
  },
}

local function hello()
  return true
end

return {
  hello = hello,
}
```

这个单文件命令用于冒烟，可用 `gterm plugin check main.lua` 检查。多段命令使用规范化的键，例如 `"ai codex"`。双侧包在所属角色的 `rpc` 列表声明入站方法，在入口返回的 table 中用方法名关联 Lua 函数，用 `ctx.rpc.call` 发起请求。实例只领取本次 Session 协商到且声明为本插件处理的方法。业务失败返回 `nil, err`，错误 table 含稳定 `code` 和可展示的 `message`。

插件内部使用普通 Lua 函数、table 和顺序控制流。宿主托管的协程负责异步等待，公开函数按普通调用编写。预期失败直接传递 `nil, err`，避免用异常或 Promise 风格处理日常分支。可关闭的短期宿主句柄使用 `local handle <close>`，实例结束时由 owner 兜底回收。

RPC 继续使用 JSON 编码。单文件开发方式只改变插件源码和发布清单的编写流程。空数组和空对象由 schema 定型或显式构造，`nil` 表示缺字段，显式 null 使用宿主提供的值。只遍历原始 table 键值，不调用元方法。拒绝稀疏表、混合键表、循环引用、超出协定范围的数值和超预算数据。大文档走资源通道。

同一个命令可从命令面板触发或由用户绑定快捷键，共用处理函数。Client 的 `ctx.context` 可含 `workspaceId`、`paneId` 和 `directory`。宿主只提供当前授权范围内可以确定的值，未知字段省略。手工嵌套 SSH 不推断目标。上下文只描述触发位置，远端执行仍需单独确认和授权。

面板由宿主按插件实例、工作区和视图键管理。带有视图贡献的命令可以声明 `togglePanel = true`，表示再次触发时关闭本工作区该命令的面板。宿主负责定位、停止和回收。用现有 table 插件加一个只读项目任务场景验收。项目 A 可以开关自己的面板，切到 B 后显示 B 的数据，关闭 A 后忽略其晚到回包。这个场景覆盖命令复用、调用上下文和双向 RPC。 [Rex 示例仓库](https://github.com/luongnv89/rex-extensions)

`gterm plugin check`、`test`、`build` 和 `pack` 接收单个 Lua 入口或插件目录。`test --paired` 用无窗口 Core Session 测试双侧 RPC，`test` 用 `--command` 指定入口。开发试跑在临时目录使用快照。插件发布签名用 `gterm plugin release sign`。CMake 在构建和安装时提供随 `gterm` 定位的 Python 开发工具，插件作者无需安装 npm 或运行编译流程。

## 6 插件双向通道、传输接缝与 TTY 候选

沿用已冻结的 wire v1 envelope 和 SSH exec 长度帧。按实例分离权限、代次及终态，保留队列上限、背压、取消和超时。TTY 保持普通终端路径，不承载业务回包。外部网关只预留字节流接缝。

## 7 路径解析与资源模型

在实际 Executor 按授权根和文件身份读取资源。Client 只收到有界描述符和验证过的快照。长度和摘要一致后才发布。原生预览支持文本、图片和受限的 Markdown / 静态 HTML，显式附件从授权快照解析。

## 8 生命周期、权限与失败语义

权限绑定包摘要、角色、用户确认的执行目标与实例。每次宿主调用复查权限和实例代次。撤销、停止或断线后清理协程、资源租约、RPC 和受管子进程。结果失联保持未知，禁止自动重放。诊断记录阶段和错误码，排除凭据、私钥与业务内容。

## 9 AI 对话插件的实现切片

先用 Preview 验证 Lua 两侧、双向 RPC 与原生资源，再用 Lua 重写 Codex 和 Claude 的协议客户端。Claude 先验证官方 CLI 的流式输出、用户提问、审批与中断接口，CLI 缺少的能力显式报不支持。审批需绑定实际后端请求、会话代次与文档版本。真实账号、模型、工具执行、中断与断线分别验收，协议夹具只计协议证据。

## 10 未来平台接入规范

先覆盖现有 macOS、Android 和桌面/服务器 `gterm`。其他平台逐端验证 Lua 构建、执行位置与原生 UI 能力。插件禁用档继续验证普通终端和文件功能。

Lua 开发构建使用本地锁定源码包。Apple 用 `GHOSTERM_BUILD_PLUGIN_LUA=1` 和 `GHOSTERM_LUA_ARCHIVE` 构建 Core，SwiftPM 用 `GHOSTERM_CORE_TEST_PLUGIN_LUA=1` 选择同一静态库。可用 `GHOSTERM_CORE_BUILD_DIR` 指定隔离构建目录。Android 对应 Gradle 属性为 `ghostermPluginLua=true` 和 `ghostermLuaArchive`，启用档随包包含 Lua MIT 许可。

macOS 开发包使用 `scripts/package-mac.sh --experimental-plugin-lua --out <目录>`，并传入 `GHOSTERM_LUA_ARCHIVE`。本地 ad-hoc 开发包标记 Lua 版本并附许可，更新渠道保持禁用。

该开发包已在本机完成 ad-hoc 签名和严格验签，随包主题、终端与鼠标恢复冒烟通过。可执行文件含 Lua 符号，未发现 QuickJS runtime 创建符号，包内包含 Lua 许可。受本机 Homebrew OpenSSL 影响，此构件最低系统为 macOS 26，作为本地开发证据保存。

Lua 档已在本机完成 macOS Swift Core、macOS Debug App 及 Android arm64 Debug APK 构建。macOS App 使用缓存中同版本 Sparkle 构件。Android ABI 检查为 168 引用、39 登记未用，实际 APK 核对 Lua 许可。这个证据覆盖构建和链接，设备交互与发行构件待验。

## 11 实现切片与验收计划

| 顺序 | 交付 | 退出条件 |
| --- | --- | --- |
| 1 | 锁定 Lua 并接 Core 与 CLI | 默认档和 Lua 档可离线构建。验证入口限制、实例隔离、超时、取消、OOM、关闭与 ABI。 |
| 2 | 接 Lua 清单、源码试跑与两端原生最小样例 | 单个 Lua 文件可静态生成清单和完整性记录。验证声明提取不执行 Lua、签名导入先展示权限、原生点击后才执行，以及声明与导出不一致时拒绝激活。提供无窗口单侧和双侧 RPC testkit。 |
| 3 | 用 Lua 重写 Preview、table、diff、task、form、logs、views | 本机、直连、受管多跳验证 RPC、原生视图、资源字节、并发与清理。 |
| 4 | 用 Lua 重写 Codex 和 Claude | 锁定后端版本并检查审批、进程回收、取消和未知结果。 |
| 5 | 完成切换并删除 QuickJS | 删除 QuickJS submodule、补丁、ABI 档位、JS/TS SDK、Node 桥、插件 npm 依赖与锁文件、旧夹具和构建开关。同步 Core、Swift、JNI、CLI、包工具、CI、打包、许可与文档。验收源码、依赖闭包、应用与插件构件均无旧插件栈残留。 |

旧实现从 Git 历史查阅。Lua 版在同一次重构中替换运行时、包格式和全部内置插件，允许调整插件 ABI、目录和开发命令，同步修改所有调用方。按 [AGENTS.md](../../AGENTS.md) 跑对应 Core、ABI、平台构建与变异闸门。真实设备与云端发行各自留存新证据。

## 12 待验证事项与决策闸门

### 原生工作区、挂接和账号验收

- macOS 命令面板和快捷键共用入口，Table 双工作区已验显式项目参数、并存、重复触发关闭和保留另一侧分页。Lua 已可读取宿主确认的工作区、窗格与目录。Android API 29 的真实两跳 Preview 已验证两个来源标识均保留，返回同一 PTY 时也不会唤起已隐藏的输入法。每个 Android 终端工作区目前只有一个窗格，用其 Session UUID 同时提供工作区和窗格标识，未知目录省略。组内各项排队时记录自己的来源。API 29 上直连与 ProxyJump 各八项已验证来源与关闭隔离。同 APK 的回归总数守卫仍按迁移前的 24 项计数，整轮未签发回执，已改为包含 Lua 控制器用例的 25 项。终端插件命令选择和取消已在 API 29 验证回到同一 PTY 并保持输入法隐藏。Android API 29 的 ProxyJump 双 PTY 回归与 25 项同 APK 配置、模型和控制器对照通过，源码与 APK 摘要一致。回归分别启动两套只读任务样例，从首个工作区的插件命令关闭其面板，再确认第二个工作区仍能经原连接取数。启动参数由测试夹具送入原生启动入口。此前直连和 ProxyJump 八实例及 25 项对照通过的那轮因修改测试源码未签发摘要回执。API 36 直连与 ProxyJump 均通过八实例与双 PTY 用例、同 APK 25 项对照和最终摘要核验。macOS 通过 16 项原生模型和本机、直连、ProxyJump 各两项原生运行。在四项 SSH 场景中，测试通过原生控制器接收远端确认的 Table 页，在界面边界暂存至当前面板销毁后，再把结果交给 Table 模型。旧模型拒绝提交该页，另一个工作区仍可取数。
- 旧 Node/Exec SSH App 驱动、两端专用测试入口和只供旧栈使用的进程夹具已移除。双方授权、取消和断线、资源、前台身份及 tmux 共存的 Lua 证据见下节。Android API 29 的原生 tmux 直连和 ProxyJump 均通过。挂接前结束目标窗格的三条 CLI 路线也已验证。运行中关闭窗格的触发路径也在三条 CLI 路线通过，Android API 29 和 macOS 原生端的同一故障组合均在直连与 ProxyJump 通过。
- 按用户确认，使用本机已登录 Codex 账号实测，Claude 的真实账号验收跳过。安装中的 Codex CLI 为 0.171.0，验收另用经发布 SHA-256 核对的锁定版 0.166.0。首轮在模型调用前发现 Lua Executor 缺少线程策略确认。修复后真实模型通过 Native Client Runtime 和 gterm Executor 完成聊天回合、按序交付匹配回复并回收 owner。受管命令审批、限本轮权限审批、补丁审阅和文件变更审批均通过真实模型与实际工具复验。单独的 CLI 二进制初轮没有产生工具事件，换用经官方 SHA-256 核对的 0.166.0 provisioned 整包后逐项通过。每项各一个审批事件、精确夹具文件内容和正常关闭。权限回合另核对授权范围与命令。错误审批审阅人定点变异及既有五组补丁变异全部通过。macOS 原生界面已实测读取完整补丁、批准、落盘精确文件并收到对应模型回复。前几轮在离屏文字识别处失败，改用真实原生控件的无障碍标识后，授权、创建线程、发送、读取完整补丁、批准、匹配模型回复、停止与进程回收均通过。仅写入一个夹具文件，CLI、gterm、包和回执摘要已核对。Lua 专项在策略修复后复跑通过。生产签名与公开发行另行处理。

### Lua Preview 当前证据

- 当前 main 工作区的默认离线闸门复验通过。Core 610 项 CTest、207 个公共 ABI 符号和未实现入口冒烟通过。Apple 绑定 204 项、登记未绑定 3 项，Android JNI 引用 168 项、登记延后 39 项，均对拍通过。clang-format 19.1.7 验证 266 个已跟踪文件和迁移新增的 18 个 C 文件。Android 最终 Release 复验首轮误选 JVM domain 的 lint 任务，在配置阶段退出。改用仓库指定的 app、bridge、data、terminal 四模块后，未签名 Release/R8、四模块 Release lint 及 APK Lua 许可验证通过。

- Android 发布校验只接受 Lua 与禁用运行时两档。许可守卫的七个 ZIP 夹具验证了正确 Lua 许可、禁用档空许可、缺件、错档、旧 QuickJS 许可混入两种档位和许可字节不符。实际 APK 的许可证据另见下文。

- 原生拒绝授权在 macOS 和 Android API 29 上均通过真实 SSH 直连和 ProxyJump。拒绝后会话结束、Client 未激活、无资源回执，Host 可释放会话。Android 驱动另核对远端明确拒绝及清理。首轮驱动误按普通关闭的未知结果判定，已修正。
- 显式前台挂接授权变异在两端通过。macOS 使用真实直连与原生控件，Android API 29 验证 bootstrap。均取得正常编译、单条目标断言红、独立配置对照、恢复复验及源码 SHA256。macOS 首轮前台夹具漏声明 `preview.open`，补声明后通过。
- 两端均通过三组 Lua Preview 直连资源发布变异。分别覆盖显式工作区授权、拒绝符号链接和拒绝过期 Runtime。每组通过目标断言、独立对照、复原和源码 SHA256。Android 的实例变异首轮锚点同时命中视图注册，限定到资源发布后通过。
- Lua Table 的 macOS 本机停止与暂存清理重试通过变异。夹具移动暂存目录、确认子进程已回收、恢复目录再重试。目标断言、独立对照、复原、编译对象变更和源码 SHA256 均通过。首轮重复的清理断言已合并。Android 活跃 Runtime 停止与传输节奏另有 Lua 变异证据。
- 开发包的前台 Preview 已验证错误实例、命令与参数均被拒绝且保留原挂接凭据，随后正确挂接收到原始资源并关闭端点。首轮正向的 Client 与 Executor 摘要未对齐，统一后通过。签名包同一挂接用例也通过三项拒绝和正确挂接后的资源读取与端点清理，九包签名同轮通过。
- `scripts/test-lua-preview-tmux.py` 在 macOS 的独立 tmux 3.6a 服务通过本机两窗格专项。前台 Lua Preview 窗格有真实 TTY，独立无 TTY 挂接读取原始资源并清理端点；普通 shell 窗格在插件启动前、运行中和关闭后都回显自身命令结果，并保留为唯一窗格。同一脚本的 `--ssh-hops 1`、`2`、`3` 均通过实际 OpenSSH 无 TTY 挂接，目标 tmux 窗格、原始资源、端点清理与普通 shell 的结果均复现。该证据覆盖 CLI、SSH 和实际 tmux，macOS 原生 Preview 已接入 tmux 直连与 ProxyJump 端到端用例，检查显式授权、原生 Markdown、端点释放和普通窗格执行。直连与 ProxyJump 均通过。
- Android API 29 原生 Settings 的 tmux 直连和 ProxyJump 各一项通过，包含授权、Markdown 与附件、远端挂接退出 0、端点清理及普通 shell 保留。应用和测试 APK 安装摘要与构建产物对拍。CLI 的 `--kill-pane` 在直连、两跳、三跳结束待挂接的 Preview 窗格，旧挂接被拒绝，两端点清理，普通窗格仍执行命令。
- `test-lua-preview-tmux.py --kill-active-pane` 在直连、两跳和三跳通过。隔离 Lua Executor 先发布就绪资源并保持请求待完成，收到资源后精确关闭其 tmux 窗格。Host 断言原 RPC 结果为 UNKNOWN、未确认，随后释放句柄及端点。普通窗格在故障前后仍执行命令。
- Android 原生 Settings 的 `--tmux-foreground --kill-active-pane` 在 API 29 直连与 ProxyJump 各通过一项。实际 Lua 首先完成 Preview，再接收独立 RPC，发布就绪资源后保留请求。结束窗格后核对同一请求 UNKNOWN 且未确认，并由原生控件完成释放，远端端点清空且普通窗格继续执行。
- 删除旧 SSH 驱动后的离线 Core 610 项测试、207 个 ABI 符号、Apple 与 JNI 绑定对拍通过。Lua 闸门通过 44 项 Core、14 项 Swift、两跳和三跳的双向 RPC 与资源、三类路径拒绝、九个签名包和前台挂接。

- `lua_preview_resource_roundtrip` 通过。实际 Lua Executor 拒绝越界路径，授权文件经 RPC 发布后由接收端读取并核对原始字节、资源 ID 和 SHA-256。另以两个 Lua runtime 验证实际 Preview Client 命令启动和收件。
- `lua_preview_ssh` 在本机 macOS 上以 Lua 5.5.1 和锁定 libssh2 通过。临时双 SSH 服务只允许首跳直拨，两个 Lua 入口通过无 PTY SSH exec 完成请求和资源发布，Client 收到成功回包，接收端资源库记录一份资源。
- `gterm-plugin lua-build` 已可从 Preview 的 Lua 声明生成 schema v2 `manifest.json` 和完整性清单。连续两次生成的目录逐字节一致，每项大小和 SHA-256 对拍通过。候选包包含两个 Lua 入口、命令参数 schema 和许可。
- `gterm-plugin lua-build hello.lua --out <目录>` 和 `gterm-plugin lua-pack hello.lua --out <ZIP>` 可直接处理单文件插件。工具按原字节将入口收入包内的 `main.lua`，拒绝以符号链接提供源文件。单文件夹具两次打包字节相同，九个官方插件也均从 Lua 声明生成了 ZIP。
- 开发工具的 `check`、`build`、`pack` 默认读取 Lua 源码。九个官方项目静态检查通过，Preview 通过 Lua Host 兼容性检查并拒绝旧运行档。新旧打包命令别名生成相同 ZIP。
- `gterm-plugin test --paired` 在本地 Core Session 启动两侧 Lua，根据清单协商对象 RPC。专项验证 Client 发出请求并断言 Executor 的返回值。单侧启动及失败回报也已通过。
- `gterm-plugin lua-pack` 生成可复现 ZIP，`release sign` 与 `release verify` 复用现有签名描述及渠道策略。内存测试签名通过，篡改签名或 ZIP 分别被拒绝。Lua 发布校验使用 Python 与 OpenSSL，测试签名器复用现有夹具。
- 发布包与渠道策略统一由 OpenSSL 验证 Ed25519 签名，各自保留协议定义的签名域。九个官方 Lua ZIP 均通过内存签名和带已签渠道策略的发布校验。将渠道签名替换为 64 字节零值后，校验返回 `RELEASE_SIGNATURE`。
- 九包签名与校验现为 `gate-lua-plugin.sh` 的自动步骤。使用临时发布者和独立渠道根签名器重新打包、签名并逐包验证全部九个内置插件，逐包改动 ZIP 后确认 `RELEASE_ARTIFACT_MISMATCH`。首轮漏传渠道历史状态文件，补齐后在禁用 Node 的 PATH 上通过整套 Lua 闸门。设备安装仍由各自原生专项覆盖。
- CLI 的签名安装使用 Core 严格 JSON 和原生 Ed25519 校验，已移除安装策略的 QuickJS 脚本。macOS Lua CLI 拒绝撤销公钥与非 NFC 发布者，随后在一次性目录安装并登记实际签名 Preview ZIP，已安装负载包含 `manifest.json`。该构件无 `JS_NewRuntime` 符号。
- `test-gterm-lua-release.py` 验证登记后的普通 `gterm preview ./file.bin` 可进入前台 IPC，观察到准确的 Lua 运行档、命令、参数、方法及两侧摘要。启动前或挂接前改变策略字节均被拒绝，拒绝挂接后端点已清理。Lua 注册表此前被 JS 构建开关拦截，已修正。正常挂接分支已用 Lua Client 允许本次 Session，确认 Executor 响应与收件资源的原始三字节。Client 发送协议关闭帧后，挂接端和前台命令均以 0 退出，IPC 端点已清理。首次夹具直接关闭传输时产生失联，改为按协议关闭后复验通过。Linux 构建待验。
- 独立的 `test-lua-preview-ssh.py --gterm` 也在 macOS 两跳夹具通过。Client 从 Lua 源码启动，生产 `gterm relay --stdio` 核对显式开发包摘要与参数后，从固定快照启动 Lua Executor。Client 收到成功回包及一份资源。普通命令目录分派、原生宿主页面及 Android 专项待验。
- 原生宿主启动 Lua Preview 时协商 `preview.open`。Lua runtime 按当前角色已协商且由插件导出的方法领取入站 RPC，原生服务可继续从同一个 Session 领取其余请求。Core 夹具验证原生服务收到请求并成功回包。该版本通过 macOS Debug App 构建和 Android arm64 Debug APK 构建，原生界面的实际交互仍待验证。
- 九个内置插件的 Lua 声明列出各角色入站 RPC。Core 在执行命令前拒绝缺失的声明实现和额外导出。另用 Preview 包夹具声明并实现一个本次未协商的方法，原命令仍可完成且资源到账。九包静态构建、Preview Core 往返及签名 CLI 前台专项均已复验。
- `bash scripts/gate-lua-plugin.sh` 已在本机通过，使用锁定 Lua 5.5.1 和 libssh2 构建签名 CLI，运行 Core Preview 往返、两跳 SSH 实际 CLI 和签名安装前台挂接专项。macOS full 工作流已加入同一闸门。云端运行结果待验。
- `gterm` 的 stdio、前台声明和命令目录只接收 Lua 执行档。已移除 Node SDK、外部插件运行档和对应暂存参数。Lua 插件声明的受管程序仍经 ProcessService 接入。专项确认显式选择 Node、Exec 或 portable-js 均返回不支持，Lua Preview 本机和两跳 SSH 复验通过。
- Core 已删除 Node/Exec 主运行档的实现、C 配置类型、旧专项和诊断字段。Swift/JNI 与 CLI 同步缩减，Lua 受控子进程和 SSH 传输进程观察仍分别使用各自的接口。最终复验离线 Core 610 项和 207 个 ABI 符号、Apple/JNI 引用对拍、C 格式及 Lua 闸门通过。macOS 与 Android Lua 构建及 Claude 直连专项通过，Android 同轮核对断线诊断和结果未知语义。字段删除前的上一轮 macOS ProxyJump 也通过。
- macOS 和 Android 的原生前台声明只接受 Lua，签名包审查固定读取 `manifest.json`。Android 前台声明用例包含 portable-js、Node 和 Exec 的拒绝断言。
- macOS 默认安装目录为 `LuaPluginPackages`，资源和本机运行记录进入 `LuaPluginResources`。Android 默认使用 `lua-plugin-releases` 保存已安装版本与选择。旧目录保持原位。固定快照只含旧清单时，Core 返回 `LEGACY_PACKAGE_REINSTALL`，两端导入界面提示重新安装 Lua 版。CLI 定向回归已通过。
- Core 包检查与打包器一致拒绝旧脚本、npm 元数据和 Lua 字节码成员。闸门确认普通 Preview 包可通过，同一份包加入摘要正确的 `old.JS` 后被拒绝。首次夹具误用了 CLI 退出码，改为实际的包检查退出码 5 后通过。
- Lua 闸门已逐项验证 `old.JS`、`old.ts`、`old.luac`、`package.json` 和 `package-lock.json`。每个样本单独加入有效 Preview 包并重算完整性摘要，均以成员检查错误拒绝。九包签名与前台挂接同轮通过。
- `lua_vm_budget` 验证超 CPU、实例内存和全局内存预算均可结束执行。Lua allocator 计入进程共享预算，关闭 VM 后用量归零，新 VM 可以继续执行。该用例已纳入 Lua 专项闸门并在本机通过。
- `python3 -B scripts/mutation-check-lua-budget.py` 在隔离 Core 检出验证三组预算保护。逐组编译后目标断言红、同二进制控制绿、恢复后目标绿，源码 SHA-256 对拍一致。首次增量复原触发断言失败，改为每组清洁重建后完整复验通过。
- macOS 发行档使用 `MACOS_PLUGIN_LUA` 与 `package-mac.sh --plugin-lua`。本机以公开测试渠道配置构建 release 形态的 ad-hoc App，通过严格验签、渠道配置、Lua 许可逐字节对拍、旧许可缺席检查和包内终端冒烟。工作流检查随包许可与构建回执，真实生产渠道与云端发行待验。
- 调整拒绝授权控件后的 Android Lua arm64 未签名 Release/R8 和 app、bridge、data、terminal 四模块 Release lint 通过。用仓库许可校验器逐字节复验同一 APK 的 `Lua-MIT.txt` 及旧许可缺席。额外的临时检查误把版本号当包内许可文件名而失败，判据以仓库校验器为准。
- Android 发行档使用 `ANDROID_PLUGIN_LUA`，正式发行汇总复验相同 APK 的 Lua 许可。本机公开测试渠道的 arm64 未签名 Release APK、R8、四模块 lint、构建路径与许可核对均通过。七个许可夹具覆盖正确档位、禁用档位、缺失、重复、篡改与旧许可混入。云端签名 APK 和设备导入待验。
- Android Gradle 只构建 Lua 或禁用运行时，通用 instrumentation 使用实际运行时开关。仍依赖旧包夹具的三个 SSH 测试和 Preview 设置界面测试标记待迁移，旧 JS 专属测试源码退出构建。Lua Debug APK、普通 instrumentation APK 与运行时许可核对通过。本轮证据为构建，设备执行待验。
- Android Preview Settings 专项已改为从官方 Lua 源码生成包。API 36 arm64 模拟器上，直连 Markdown 与两跳 Unicode HTML 各一项通过，涵盖 DocumentsUI 导入、权限列表、显式附件字节和像素渲染、停止与资源释放、远端临时资源目录清理。安装的两个 APK 已对拍摘要。首次夹具仍期待旧能力列表，加入 Lua Preview 的 `session.rpc` 声明后复验通过。其余格式和终端键盘往返专项未在本轮运行。
- macOS Preview 原生专项改为现场构建 Lua 包，已删除独立的旧 Preview 索引生成器。通过隔离 sshd 验证 Markdown 原字节及 PNG 原生渲染，从同一测试捕获 HTML、CSS 与 PNG 后验 WebKit 像素和刷新。远端资源目录纳入清理断言。首轮复现 SSH 关闭 stderr 后 gterm 清理中断，重定向 stderr 的对照通过。gterm 捕获 SIGPIPE 后，保持实际 SSH stderr 的最终复验通过，远端目录清空。九包签名与 Lua 专项闸门复验也通过。
- 后续 macOS Preview 全场景基线的隔离检出在 SwiftPM 获取 Sparkle 时超时，未进入 XCTest。复跑使用已缓存依赖的工作区，仍现场构建 Lua Core、gterm 与 Preview 包，并启动隔离的直连及 ProxyJump 夹具。
- macOS Preview 面板专项已通过原生闸门，覆盖安装、启动授权和停止，以及直连与 ProxyJump 的 Markdown、HTML 与图像。首次复跑在 XCTest 后误要求 AppModel 专项生成的 HTML 回执，修正为仅选择该专项时要求回执后，面板专项整轮退出码为零。
- 同轮执行面板与 AppModel 的补充闸门通过 11 项 XCTest，零跳过。WebKit 使用真实 SSH 回执中的 CSS 和 PNG，首轮绘制核对红色与蓝色像素，刷新后核对红色与绿色像素。
- 已迁移 Android Lua Client 两跳 SSH 测试。API 36 arm64 模拟器首轮暴露 Lua 参数解码器不能写入按查询值分配的等长缓冲区。修正后 Core 专项和 207 个 ABI 符号对拍通过，设备复验已通过参数解码及 Session 建连，后续在夹具调用沙箱未提供的 `assert` 时失败。夹具已改为显式校验。下一轮 instrumentation 断言通过，但过早关闭 Session 时远端回执缺失。加入有界等待并继续推进 Session 后，最终在全新 API 36 arm64 模拟器通过 Lua Client 两跳 SSH、双向 RPC 和远端三条请求加一条 Host 回包的完成回执。范围为 Android 控制器与原生 Executor 夹具。
- 已移除 QuickJS submodule、八份上游补丁、旧生成 SDK 和包校验器；随包许可文档更新为 Lua。保留历史验收记录，仍引用退役引擎的旧脚本需清理。
- 已移除旧运行时 SDK 和包校验器的 JavaScript 生成、校验工具及其两个 npm 锁文件。现行构建和闸门不再引用这些工具。旧混合测试脚本仍有待迁移的通用断言。
- wire 协议独立校验切换到 Python JSON Schema，移除原 Node/Ajv 探针和 npm 夹具。通过 82 个 schema 样例与四组变异、271 个独立参考实例及 618 个 C 解码用例。
- C wire 的 16 组变异复验通过，包含输出清零、失败视图、内存擦除和归还。首轮输出清零的匹配仍指向旧结构，在生成变异前失败。改为定位现行 wire 解析入口后，全部目标断言、独立配置对照、恢复后的完整 wire 套件及 SHA256 对拍通过。
- P0 的 QuickJS 双引擎比较、正则分配失败变异入口和对应 C 夹具已从工作区退役。Lua 的内存和 CPU 时间预算由 Lua 专项闸门及对应变异验证覆盖。
- QuickJS 的三个专属边界夹具已退役，包括两处 JS 对象分配注入和旧元数据校验器的 JS 调用注入。Lua 的预算验收已经通过。出站 RPC 校验使用栈上缓冲区和 Session 已分配的 wire 解析器，请求写入预分配槽位。Lua 编码阶段仍可能因 Lua 栈扩展而失败。Lua 闸门通过循环、过深、稀疏、混合键和超精度整数五种拒绝的夹具，并在同一 Session 成功完成合法 RPC。整轮退出码为 0，包括 44 项 Core、14 项 Swift、两跳和三跳 SSH RPC 与资源、九包签名和前台挂接。隔离构建另外在 Lua 编码时定点拒绝栈扩展。分配器确认发生拒绝，编码返回空结果、恢复栈顶，同一 Lua state 随后成功编码原表。编码预算 CTest 通过。跳过失败分支栈恢复的变异编译后触发目标断言，同一二进制的配置对照通过，复原和源码 SHA-256 通过。CPU、实例内存、全局内存同轮复验，四组全部通过。
- 共享内存记账和包 JSON 的十组 Core 变异已脱离旧 SDK 和引擎。ASan 与 UBSan 下，十组均在编译后触发目标断言，同一 Core 归档的配置对照通过，恢复后目标复验、完整基线与源码 SHA256 核对通过。
- 包快照的 41 项基线及 28 组变异在 Lua 禁用档通过。变异涵盖目录与字节预算、路径编码、硬链接、文件替换与截断、内容摘要，以及描述符释放。各组通过实际编译后的目标断言、同归档配置对照、复原和源码 SHA256 核对。旧锚点首次校验失败，经重新定位相应分支后完成整轮。
- 子进程 RPC 使用 C 子进程夹具完成 23 组变异。目标断言、同归档配置对照、恢复、前后基线与源码摘要核对均通过，ASan 与 UBSan 未报错。
- 移除退役的 Node 子进程入口与夹具后，再次通过整组 Core 子进程 RPC 基线及取消变异。首轮旧分支清理遗漏了成功回执断言所用的请求句柄，编译失败。恢复句柄并保留成功和释放断言后复验通过。
- macOS 渠道 TLS 专项按实际 Core 构建选择 Lua 档。在 Lua 禁用的 Core 上，渠道预检及原生 HTTPS 刷新与失败保留缓存用例通过。
- macOS 使用 Lua 5.5.1 构建通过被动发现的原生运行失败用例，确认传输失败仅清除所属终端窗格的候选。此轮使用工作区中已解析的 Swift 依赖，隔离副本上的变异另验。
- macOS 被动发现的传输失败变异在隔离副本复验通过。首次使用默认 SwiftPM 缓存在 600 秒后超时，改用该轮私有缓存后，编译、目标断言、独立对照、恢复复验与源码 SHA256 核对均通过。
- 清理打包器的旧源码预处理和 npm 环境设置后，Lua 专项闸门复验通过。覆盖 Preview 资源、两跳和三跳 SSH 双向 RPC 与资源传输、九个官方包的测试签名、前台挂接及本地单文件和双侧运行。
- 再次运行离线 Core 闸门，610 项 CTest、207 个公共 ABI 符号和未实现入口冒烟通过。平台引用复验通过，Apple 引用 204 项、显式登记未用 3 项，JNI 引用 168 项、显式登记未用 39 项。
- CLI Preview 的真实 stdio 专项通过普通路径、`./` 路径及 512 字符 Unicode 深层路径三项。宿主显式协商并发送 `preview.open`，同时核对成功回执、资源元数据、实际字节和摘要。迁移过程中发现探针仍走旧版隐式发布流程，且通用回执分支会重复读取 Preview 请求，修正后复验通过。
- CLI stdio 通用回归切换为现场打包的 Lua 夹具。默认组合通过 14 项会话用例和 3 项 Preview 用例，覆盖参数、双向 RPC、拒绝、取消、摘要不匹配、独立管道及断线。旧 JS dispose 回调异常用例随回调接口退役。
- 同一 Lua stdio 探针的独立进程配置和 TTY 专项通过。显式程序映射允许进程能力，缺少映射时拒绝启动，终端作为协议输入或输出时均被拒绝。
- Lua stdio 的六组 CLI 变异通过。覆盖终端拒绝、固定包摘要、命令选择、授权先行、参数数目及 RPC 方法方向，逐组编译、目标断言、配置对照及恢复通过。
- CLI 探针的 Node 与 Exec 专属启动分支、测试脚本和变异脚本已退役。重建后，Lua stdio 的 14 项通用会话测试和 3 项 Preview 用例复验通过。
- CLI 前台 IPC 整组复验通过。使用现场构建的 Lua 夹具验证许可、配对、双工、半关闭、TTY 拒绝、闲置取消和 Preview 资源。
- CLI IPC 的 16 组变异通过。首轮因测试路径超过 Unix socket 长度限制而未进入变异，下一轮缺独立配置对照的构建目标。调整路径并补齐构建后，目标断言、同归档对照、复原、最终基线及摘要核对全部通过。
- CLI 前台检查的 21 项 Lua 用例及最终回执通过。覆盖声明读取、前台许可、实例、终端拒绝、尝试预算、畸形帧和私有字段排除。首轮复验期间修改了共享 IPC 夹具，摘要守卫拒绝回执。冻结输入后，源码及三个链接构件的摘要核对通过。
- 九个官方 Lua 包以临时测试密钥签名后，逐个完成实际安装、独立登记、普通命令启动和前台检查。改动信任策略后，启动与挂接均显式拒绝。九个 Host 并发的全部拒绝、全部允许及混合授权三轮均通过，Codex 使用本地协议夹具。Python 发布工具的 47 项基线和原 18 组变异也复验通过。
- 通用 CLI 发布回归已迁移到 Lua，包含版本与残留维护、策略审查、显式开发登记、授权拒绝与取消。基线完整通过。启用专属故障构建后，版本恢复、残留恢复、策略文件同步和目录同步四项通过。
- CLI 目录的 Lua 基线与隔离构建的 26 组变异通过，包含内置命令名、同包命令前缀、目录持久化故障与按调用目录提供 Preview 资源。前缀变异首轮未命中夹具所走的分支，修正后触发目标断言，同归档配置对照、复原及最终摘要均通过。
- CLI 的包检查和命令解析分别通过 12 项与 15 项 Lua 清单用例，包括未知兼容性、摘要、显式选择、冲突、符号链接和无效 Host 描述。首轮将空 Host 描述判为未知的旧断言已按当前格式校验修正。两组测试中的 Lua 源码包含执行即报错的夹具，验证检查和解析阶段均不执行插件。
- CLI 包检查七组、命令解析六组及写入阻塞时识别断线的变异通过，逐组完成目标断言、配置对照、复原和摘要核对。
- 子进程固定参数前缀首轮复跑发现目标随旧运行档移除。已迁移至原生 Session 与通过检查的官方 Lua Claude 包，并恢复 Lua 档的 C 构建目标。ASan 与 UBSan 下 11 项基线和 10 组变异全部通过。纳入 Lua 专项闸门后，11 项前缀、两跳与三跳 SSH RPC 及资源、九包签名和前台挂接同轮复验通过。
- 已移除 `packages/plugin-sdk` 中的 Portable、Node 和 TypeScript testkit 实现。仍使用的协议 schema 与 wire 夹具移到 `packages/plugin-protocol/schema`。生成的 C wire 定义逐字节相同，Core wire 用例与生成对拍均通过。
- 九个内置插件目录已清除 JS/TS 源码、npm 清单与锁文件和旧包元数据。保留 Lua 声明引用的 schema、许可和 Codex 协议锁。九个包各打包两次，ZIP 逐字节一致。逐包核对 Lua Client 与 Executor 入口、生成清单、完整性索引覆盖范围及每个成员的大小和 SHA-256，均通过。产物未包含旧源码、Lua 字节码、npm 清单、锁文件或 node_modules。
- 当前开发指南与 gterm README 已改用 Lua 源码、静态清单和 CLI 命令。迁移前的详细用法和记录保留在链接指向的 Git 版本。实跑了脚手架、静态提取、单文件检查、目录构建、ZIP 打包、Core Client 试跑及 gterm Preview 兼容性检查。 macOS 另将当前 CLI 安装到一次性前缀，在与源码目录不同的工作目录用已安装的 gterm 执行 init、单文件 check、build、pack 和签名子命令帮助，均成功找到随包工具并生成清单。常规 Core 构建和离线闸门已移除旧 JS CMake 开关引用。
- 已删除退役 TypeScript 开发 worker、专属 npm 锁和旧开发 testkit 夹具。Lua 签名安装闸门改用 Python 与 OpenSSL 的临时 Ed25519 签名器，私钥只通过内存和管道传递。将必定失败的 `node` 探针放在 PATH 首位后，整个 Lua 闸门复验通过，覆盖 Preview 两跳 SSH、签名安装、前台挂接、资源和本地双侧试跑。其他历史签名测试仍有独立的旧夹具引用。
- Lua 签名安装、渠道策略签名及 Android 渠道 TLS 夹具共用同一 Python/OpenSSL 签名器。使用拒绝执行的 Node 探针复验，渠道策略专项、Android TLS 夹具生成与两份签名渠道策略校验、Lua 专项闸门均通过。首次直接调用签名器时 Unix socket 未转换 Path 参数，修正后重跑通过。Android TLS 设备交互及仍引用旧运行档的签名场景另验。
- Android 渠道 TLS 用例已迁入现行测试源集，runner 默认以 Lua 档构建。API 36 arm64 / 4 KiB 隔离模拟器在安装后对拍两份 APK 摘要，通过真实 HTTPS 策略刷新单项。覆盖正常签名策略、不受信证书、重定向、超限、混合或缺失材料、过期、撤销、回滚及失败后保留已提交快照。
- 同一 runner 的 `--settings-cache` 使用两个临时 Ed25519 签名器，现场构建并签名 Lua Preview ZIP，再签首版渠道策略及撤销策略。API 36 arm64 / 4 KiB 隔离模拟器对拍实际安装的应用和测试 APK 摘要，Settings 原生界面与 DocumentsUI 专项通过。确认缓存安装、只读版本发布、安装后未授予来源批准且未启动、显式选择，以及撤销后拒绝提交此前预览。TLS 夹具收到零个请求。
- Core CMake、CLI 和 macOS 打包已拆除 QuickJS 链接，SwiftPM 只选择 Lua 或禁用运行时。Lua 定向闸门复验通过。离线闸门现显式打开 CTest 构建，避免复用先前关闭测试时留下的旧测试清单。首轮旧清单的 SSH 公共 owner 用例因并行源码修改触发摘要守卫，单项复验通过。最终重新配置后离线闸门 610 项与 207 个 ABI 符号对拍通过。旧 JS 专属夹具与脚本仍需清理，原生平台的行为验收仍需继续。
- macOS 与 Android 的签名导入审查和已安装版本审查接入 Lua 的 `manifest.json`。展示前对拍 Core 固定的原始清单摘要，并按 Client 和 Executor 展示所需能力。受管程序列表归入 `process.spawn`。macOS 清单审查 5 项通过，Android Lua arm64 Debug APK 构建通过。macOS 原生安装控制器另通过实际签名 Preview ZIP 导入专项。签名在一次性内存夹具生成。审查、明确安装、只读发布和重读已安装声明均通过，安装后命令仍需独立授权。两端界面交互待实测。

- Android API 36 arm64 隔离模拟器通过 `LuaReleaseInstallTest`。由 `gterm-plugin lua-pack` 打包实际 Preview 并以内存测试签名器签名。Android 控制器验签并展示两侧所需能力，确认安装后生成只读负载，重读安装清单得到相同声明。安装前后均未授予来源授权，命令保持不可解析。

共享 Core 的 Lua 默认预算适用于采用默认配置的运行时。每个 Client 或 Executor state 可分配 16 MiB，单次恢复执行以本线程 CPU 时间计 25 ms，启动阶段墙钟上限 5 秒。macOS 与 Android 的原生启动沿用默认配置，gterm 的 stdio Executor 显式设为每次恢复执行 100 ms。进程内插件内存共用 128 MiB 预算。Core 的运行时配置可覆盖前三项，范围分别为 1–128 MiB、1–1000 ms 和 1–60000 ms。部署测量可用来调整这些默认值。具体口径见 [Lua Runtime](../../core/src/extension/runtime-lua.inc)、[Core 插件内存](../../core/src/extension/memory.h)与 [gterm stdio](../../cli/gterm/stdio.c)。Lua table 到 JSON 的边界验收见上文。公开分发前实测 Lua 包的生产渠道签名、升级、撤销与进程中断。

- Table 的 Lua 包通过公共 Core Session 探针，覆盖百万行的精确分页、空表、过期版本拒绝及无原生表格能力时的显式错误。macOS 通过 16 项原生表格测试及 6 项实际路由场景，本机、SSH 直连和 ProxyJump 各覆盖百万行与空表。首次运行因 Core 归档路径错误停在 SwiftPM 初始化，修正路径后通过。同一 Lua 包的 macOS 变异复验通过 10 组，包括 7 组原生模型和 3 组实际路由保护，均确认目标断言红、独立配置对照绿、复原绿与源码 SHA256。Android 完整验收仍在进行。

- Diff 首轮 macOS 原生用例通过本机、直连和 ProxyJump 的变更、空内容及 Unicode 场景。随后宽字符用例发现启动参数误用 RPC 报文上限。Lua 启动参数改用独立的 64 KiB JSON 上限，公共 Core 探针已用实际 Lua 包通过宽字符、Unicode、换行重组、分页与过期版本拒绝。原生复验在进行。
- Android Table 的 API 29 与 API 36 各通过 24 项原生测试以及 SSH 直连和 ProxyJump 各两项。测试期间工作区迁移了其他用例，输入摘要守卫拒绝签发该轮验收结果。当前工作区已启动复跑。

- Task 的公共 Core Session 探针已切换到实际 Lua 包，九种场景通过，覆盖独立质数计数、连续进度、取消、重复取消，以及发送后丢失确认的未知状态。不同取消请求 ID 返回已确认的 `INVALID_INPUT`，过期代次同样被明确拒绝。

- 调整 Lua 启动参数上限后，重新构建通过离线 Core 闸门的 610 项测试和 207 个公开 ABI 符号对拍。独立 Lua 闸门复验通过，包括 Preview 资源往返、九个签名插件包及签名安装后的前台启动。

- Task 的 macOS Lua 用例通过 28 项原生测试、16 项 Table 和 15 项 Diff 独立回归，并在本机、SSH 直连和 ProxyJump 各通过计算完成、空输入和取消三种场景。变异与 Android 验证待完成。

- Diff 的 macOS 最终复验通过 15 项原生测试、16 项独立 Table 回归、11 个实际路由场景和 11 组变异。宽字符与交错序列用例均绿。Lua 专项闸门另验证两侧收到完整的 3500 字节启动参数后完成 RPC 往返。

- macOS 八实例回归使用 Lua Table，在本机、SSH 直连与 ProxyJump 均通过。测试验证同一连接上的实例独立分页和释放。Lua CI 闸门另核对实际 CLI 链接 Lua，并检查 CLI 与 Core 归档的 QuickJS 符号，复验通过。

- Views 的 macOS Lua 复验通过原生视图模型、服务协商和 Table、Diff、Task 回归。同一官方包通过本机、SSH 直连和 ProxyJump 的视图注册、独立关闭、重开和任务控制。变异与 Android 实际场景待验。
- Android Diff 首轮 API 29 的 Table 对照执行了 24 项且全部通过。脚本仍要求旧的 23 项，因测试数量校验提前退出。更新数量后已启动两档复验。

- macOS Form 的双实例原生用例通过。测试验证同一包的两个实例各自接收回复和取消，并拒绝给另一实例提交表单。此次直接构建测试归档时未钉 macOS 部署目标，链接器提示构件标记高于 Swift 工程目标。应用构建仍使用仓库脚本中钉定的 macOS 13 部署目标。

- Android Diff 通过最终输入摘要核验。API 29 与 API 36 各通过 27 项原生测试、24 项 Table 对照，以及 SSH 直连和 ProxyJump 各四项实际用例。

- macOS Task 变异复验通过 18 组。覆盖快照标识、进度、取消回执、未知结果及原生接线。各组目标断言红、独立配置对照绿、复原绿且源码摘要相同。原生实际场景共 13 次运行，含 9 个本机/SSH 基线和 4 个恢复对照。
- Android 原生 Task 基线最终通过。API 29 和 API 36 各通过 34 项模型、编解码与 UI 测试，并分别经 SSH 直连和 ProxyJump 验证完成、空输入和取消。脚本接受最终输入与构件核对，两台独占模拟器均已关闭。Android Task 变异另验。
- Android Task 在 API 29 的 18 组变异全部通过，包含视图标识、取消、原生服务协商及渲染。逐组目标断言、同一 APK 的 Core 对照、恢复与源码摘要核对通过，最终任务回执通过。

- Python 与 OpenSSL 发布测试通过 47 项，包括九包验证、撤销、摘要与签名、路径及 ZIP 校验、外部签名器无效输出和中断回收。Android API 29 的隔离副本已安装九个签名 Lua 包并重新读取声明，确认每包仍需单独批准。首轮夹具遗漏取消前一版本预览，隔离复跑补齐后通过。

- Android Form 在 API 29 和 API 36 各通过 2 项原生测试，以及实际 SSH 直连和 ProxyJump。最终源码和构件摘要核验通过。
- 发布测试的 18 组变异全部通过，包含验证器退出码、签名、发布者及归档边界。目标断言、独立配置对照、恢复和摘要核对均通过。

- 当前检出通过 clang-format 19.1.7 的 284 文件检查。Apple 绑定 204 项，另有 3 项显式登记。Android JNI 引用 168 项，另有 39 项显式登记。两端分别与当前 207 个公开 ABI 符号对拍。

- macOS 实验 Lua Debug 宿主的原生签名基线通过 15 项。另构建本地 ad-hoc Release App 并通过自包含、实际终端冒烟、严格应用签名核验。随包 Lua 许可证与锁定文本一致，旧 QuickJS 许可未检出。这些构件在启动界面最终清理前构建。
- 启动界面清理后重新构建本地 macOS Lua ad-hoc App。SwiftPM Release、随包字体与终端资源、自包含依赖和 `codesign --verify --deep --strict` 通过。构件用于本地验收，未生成分发 ZIP。

- macOS 与 Android 启动界面已移除 Node／Exec 选择。两端 relay 参数生成只接 Lua，旧请求类型若被直接调用会在连接前报错。Lua 仍使用显式工作区与固定进程映射。更新后的两端构建和测试正在复验，旧请求类型及独立脚本仍待清理。

- 移除 Android 旧启动选项后，Lua Debug App 和测试 APK 重新构建通过，App 的 Lua 许可证对拍通过。设备回归仍按实际测试轮次记录。

- 旧 TypeScript SDK、Codex 与 Claude 的专用构建和 QuickJS 后端验证入口继续退役。当前 Lua 专项闸门通过 Preview 资源、九包签名与双侧参数 RPC；旧专项中的接口行为仍须按 Lua 实现逐项核对，历史结果在 Git 中保留。

- Lua 运行时当前检出复验 CPU、实例内存与全局内存三组预算变异。每组均观察到目标断言失败、同一二进制对照通过和复原通过。

- Android Table 分两档通过全部行为用例，API 29 执行的 12 组变异各自通过目标断言、同 APK 对照、复原与摘要。旧运行档界面在该轮执行期间被修改，脚本的最终工作区摘要守卫拒绝签发整体结果。合并验收仍须使用稳定输入复跑。
- Android Table 的稳定输入基线复跑已通过。API 29 与 API 36 各 24 项原生测试，外加直连和 ProxyJump 各自的百万行与零行用例。两台模拟器均正常退出，源码与构件摘要核对通过。12 组变异仍为前一轮逐组证据。
- Android Table 在同一稳定输入上复跑 API 29 的 12 组变异，全部通过并接受最终回执。覆盖分页模型、原生 RPC 服务与渲染。直连和 ProxyJump 基线、同 APK Core 对照、复原和源码及构件摘要核对均通过。

- 清理旧启动界面和参数后，Android API 29 的 12 项原生启动及签名版本用例通过。macOS 对应测试执行 19 项，17 项通过，2 项缺显式 TERM-ignore 夹具而跳过，正在用 Lua 档的本机日志脚本补验。

- Android Logs 的 API 36 复跑通过 24 项原生测试、SSH 直连与 ProxyJump 的实际搜索，以及最终摘要核对。API 29 的行为在上一轮通过，那轮未形成整体收据。
- Android Logs 的 API 29 独立复跑通过 24 项原生测试、SSH 直连与 ProxyJump 各 5000 行的实际搜索、模拟器退出及最终核对。
- Android 删除旧 Executor 启动类型后，Lua Debug 应用与 instrumentation APK 构建通过。API 29 核对两份安装包摘要并通过 12 项启动和签名测试，覆盖挂接、进程映射和授权。两份更新的旧 SSH 专项夹具尚需端到端验收。
- 后续 Android Lua arm64 未签名 Release APK 构建、app、bridge、terminal、data 四个模块的 Release lint、domain JVM 测试任务及 APK Lua 许可核对通过。启动命令曾误选模块或任务名，均未计作通过。
- 隔离检出中，迁移到 Lua 的 Swift `ExtensionPackageTests` 两项通过，无跳过。该轮使用隔离目录内的测试源，工作区接入和对应变异仍需验收。
- 同一隔离测试源的 Swift 包门面四组变异通过。入口名、能力列表、授权状态和停止操作均在重新编译后触发预期断言，同一 Core 归档的配置测试与复原测试通过，源码及归档摘要一致。注册表与调用参数变异单独运行。
- Swift 注册表三组与调用参数两组隔离变异通过。调用参数覆盖省略整份调用和替换空参数值。初轮旧锚点失败，以及移除空参数导致未达到单断言验收的结果均未计入通过。
- 本地子进程通道的 C 夹具在隔离 Lua 档通过 13 项，包括 Lua 接管会话并保留最终子进程观测。工作区接入与对应变异仍需验收。
- 子进程通道隔离复跑完成。13 项 C 基线、关闭档、九组 C 变异、六项 Swift 基线和两组 Swift 变异通过。首轮 Swift 链接阶段缺 Lua 开关及宿主依赖路径，已在复跑补齐。迁移后的 C 和 Swift 夹具仍在隔离副本。
- 使用真实打包的 Lua Preview，在 ASan/UBSan 的隔离 Core 上通过原生包校验及三个分配位置的逐点失败检查。每次失败保留快照归属并释放临时分配。已将原生包检查驱动切换到 Lua Preview 夹具，驱动和工作区夹具还需复验。
- Lua 原生包检查驱动在隔离 Core 上通过七组变异。覆盖内容摘要、成员大小与路径、成功时快照交接、包摘要、未知宿主及失败时输出清零。每组经过实际重编译、单条 C 断言、同归档配置对照和复原摘要，前后基线也逐点验证分配失败。先前锚点重复、二进制未变或等待错误诊断的三次中止均未计为通过。
- macOS Preview 整轮在最后一组附件刷新变异未取得验收。变异触发了目标断言，随后的解包又报告第二条失败。测试现于缺少替换视图时在目标断言后返回，保留正常路径的后续验证，最后一组单独复跑。
- Lua 闸门现包含 13 项启用档的原生进程通道测试，以及 Lua 包检查与逐点分配失败检查。当前工作区整套 Lua 闸门通过，七组包校验变异也在工作区复验通过。C 格式检查覆盖 273 个文件。
- 接入工作区后的进程通道复跑通过 13 项 C 基线、关闭档、九组 C 变异、六项 Swift 基线和两组 Swift 变异。
- 隔离移除 Node/Exec 专用暂存实现、私有头和旧 C 夹具后，Core 构建及 207 项公共 ABI 对拍通过，归档无旧暂存符号。删除已接入工作区并启动离线闸门。
- 旧暂存实现移除后，工作区离线闸门通过 610 项 CTest、207 项公共 ABI 和未实现入口冒烟。
- macOS 删除旧 Executor 启动类型后的隔离 Swift 构建通过八项启动与签名测试，无跳过。首轮卡在 Sparkle 构件下载，改为复制已解析的本机依赖后通过。删除已接入工作区。
- Preview 附件刷新定向复跑通过目标断言、独立对照和复原，随后链接归档摘要变化使整轮回执失效。Lua 参数解码头生成已改为仅在内容变化时写入，并补充归档差异诊断。
- 连续两次同输入 Lua Core 构建的 Core、Lua 和宿主 libssh2 归档 SHA-256 一致。Preview 附件刷新定向回执复跑已启动。
- macOS Preview 附件刷新最终定向复跑通过。直连和 ProxyJump 合计 10 项流程、31 项资源寿命用例、附件刷新目标断言、对照、复原以及源码和链接归档摘要全部通过。其余变异取前一轮逐组输出。
- 原生 Lua 启动验收在工作区通过 macOS 八项基线与 Android API 29 的 12 项基线和三组变异。Android 每组包含同 APK 配置对照、复原和摘要。macOS 首轮误指定空的 Core 构建目录，显式构建 Lua 档后复跑通过。
- Lua CLI 多路复用通过 14 项预检拒绝、八实例往返、拒绝、资源、取消和实时复用。资源用例对八个实例实际调用 Preview 并核对各自响应和文件，禁止跨实例读取。早期夹具将 JSON 对象键顺序当作结果的一部分，改为逐字段解析后通过。已纳入 Lua 闸门。
- Lua 闸门用实际编译出的 `gterm` 对本计划中的单文件示例运行 `plugin test`、`check` 和 `pack`，再通过 CMake 安装后的 `gterm` 复验打包和签名命令入口。构建树与安装树生成的 ZIP 字节一致。同轮通过 44 项 Core 测试、两跳和三跳 SSH、九个内置签名包及前台挂接。该轮在 macOS 执行。
- 加入 CLI 多路复用后整套 Lua 闸门复验通过。四项 Lua Runtime 会话归属测试另行通过，覆盖待发请求、已领取请求、未领取请求和资源所有权，已加入下一轮闸门。
- 四组 Lua Runtime 会话归属变异在 ASan/UBSan 隔离构建通过。每组核对实际二进制变化、单条正常断言、同归档配置对照、复原与 SHA-256。
- macOS 启动变异首轮通过进程环境映射，前台授权变异未触发断言。已补用无其他无效字段的未批准挂接，并让本机和 SSH 两条签名启动路径共用一条拒绝断言，复跑已启动。
- macOS 三组启动变异完整复跑通过，包括进程环境映射、前台挂接授权和签名策略授权。每组核对目标断言、独立对照、复原及 728 份输入摘要。
- Lua 包检查新增开始时与提交前的取消用例，含提交前取消变异的八组复跑通过。随后清理探针中的旧包格式与引擎专属分支，已启动同一套复验。
- 精简原生包检查探针后，九项夹具、三个逐点分配失败位置和八组包校验变异再次通过。
- Lua 闸门复跑通过 Preview 的非法根描述符、标识、大小和数量边界，拒绝均保留调用者的会话与描述符。旧 JS ResourceBroker 适配器夹具已移除，根标识与大小两组 ASan/UBSan 变异另行复跑。
- 根标识和大小两组 ASan/UBSan 变异通过，核对实际构件变化、目标拒绝断言、配置对照、复原和摘要。首次漏传临时目录引发参数断言，未计为通过。
- 11 项原生 Lua 视图测试通过并加入 Lua 闸门。C 侧视图变异驱动已移除旧 SDK/npm，七组隔离复跑已启动。
- C 侧视图七组变异通过，包括角色、授权、撤销后的快照、RPC 方向、容量、重复 ID 和关闭。各组通过单条正常断言、同归档配置对照、复原和摘要。
- 公共包接口换用独立 Lua 夹具后通过十项启用档测试，含包与会话接管、注册表、参数解码、取消、覆盖与摘要不匹配拒绝。初轮沿用旧引擎的作业配额字段导致两项接管失败，改用 Lua 运行档配置后复验通过。十项均已加入 Lua 闸门。
- 完整 Lua 闸门通过 40 项启用档 CTest、九项包校验夹具及分配失败、CLI 多路复用、多跳 SSH RPC 与资源、九包签名和前台挂接。公共包接口的五组变异已接入 Lua 原生变异脚本，隔离复跑待确认。
- 公共包接口的五组隔离变异通过，覆盖捕获取消、校验取消、未知状态、包摘要和停止后拒绝接管。
- 内部注册表 Lua 夹具通过冲突、替换、角色隔离、容量和内存保护，连同十项公共包测试通过。已将其加入 Lua 闸门并启动七组变异。旧包加载器的大段 JS 夹具已移除，保留停用档行为测试。
- Lua 内部注册表七组变异及公共包与内部加载器的两项停用档均通过。公共注册表仍需单独核对输出清零、状态映射、选中快照独立性和命令 schema，新夹具复跑已启动。
- 公共包与注册表的 13 组 Lua 隔离变异完整复跑通过，包含选中快照独立性、状态映射、命令 schema、输出清零和会话接管边界。
- Android API 29 的 Lua JNI 基线通过 12 项 instrumentation。驱动核对两份安装 APK 摘要、JNI 构件来源和 Lua 许可。旧 SDK 注入路径已移除，JNI 变异另行复跑。
- 公共包与内部注册表共 14 项通过。插件 ID、版本、包摘要和显式关联的四组接管变异通过目标断言、配置对照、复原及摘要核对。旧 JS 包加载器变异脚本退役，现行入口为 `mutation-check-plugin-package-check.py` 的原生包、公共包和注册表档及 Lua 闸门。
- 加入内部注册表和接管约束后，完整 Lua 闸门复验通过。Android API 29 的 JNI 五组定向变异通过，覆盖未知与拒绝状态、包入口、执行步数和资源上限，每组核对实际构件、单条断言、同 APK 配置对照和复原摘要。
- 现行 Core Runtime 变异驱动已切到 Lua，四组会话归属变异完整复跑通过。C 格式检查通过 269 个文件。修正归档稳定性后的 macOS Preview 全量变异重新运行。
- Lua 闸门通过 44 项 CTest。静态提取与确定性 ZIP 验收同轮通过，两份单文件源码包字节一致，打包未执行文件里的主动报错。动态值和重复字段的退出码与解析诊断已独立核对。
- Android 的签名、安装、目录、归档、HTML 和 OSC 52 手工闸门已改用 Lua Gradle 开关及锁定归档。迁移脚本通过 Python 语法检查，API 29 HTML 基线用于验证接线，其余脚本设备复跑另计。
- 手工执行上述 Android 脚本时，通过 `GHOSTERM_LUA_ARCHIVE` 指向本地官方 `lua-5.5.1.tar.gz`。脚本将同一文件传给 Gradle，Core 配置核对档案摘要。`gate-lua-plugin.sh` 在没有指定归档时会下载并核对固定版本。
- 切换 Lua 构建参数后，Android API 29 的 HTML 基线通过 19 项原生测试。
- Table 新增显式的 `sample-a`、`sample-b` 只读任务样例。Lua 5.5.1 源码探针通过样例隔离、拒绝跨实例页键和百万行数字回归。实际 gterm 在一个多路复用进程中通过八实例取数，关闭旧实例后拒绝其分页请求，保留兄弟实例。macOS 隔离构建的 16 项原生测试通过，两套样例经本地、SSH 直连和 ProxyJump 共六次验证表头、首行数据和原生单元格标识。Android API 29 和 API 36 两套样例分别通过直连和多跳。首次 API 36 轮末源摘要因并行编辑失败，串行复验的 24 项原生断言、四个 SSH 场景、摘要与清理通过。命令面板及重复触发关闭仍待验证。
- Android API 29 设备以已迁移的 Lua Claude 插件和受控 SSH 进程夹具验证活跃 Runtime 停止。移除 Kotlin 活跃状态的停止分支后，重新编译 APK，`Runtime stop missing` 单条正常断言失败，同 APK Core 配置对照通过。还原后专项通过并核对 SHA-256。夹具不发起模型请求。
- Lua 专项闸门复验通过。两个静态清单合法的 Executor 分别缺少声明处理函数和增加未声明处理函数，实际启动均以 `GT_EXT_RUNTIME_PROTOCOL` 拒绝。九包签名及签名前台入口同轮通过。
- Android API 29 的官方 Lua Preview 直连 SSH 用例通过。设置页授权后收取 Markdown 与 256 KiB PNG，验证 PNG 字节数、SHA-256 和渲染，再检查停止、释放及远端退出后的资源清理。旧传输节奏变异的替代验收另行验证。
- Android API 29 用 `mutation-check-plugin-execution-pump.py` 重编译单步传输变异。每次原生循环最多收取一帧时，官方 Lua Preview Client 以 `EXPIRED` 结束，设置页未收到资源，命中单条正常断言。同 APK 的 Core 配置对照通过，还原后 256 KiB PNG 专项通过并核对源码 SHA-256。
- Android API 29 恢复运行插件 SSH 提示测试。两跳真实 SSH 夹具通过主机密钥确认、发布凭据提示及取消，不进入远端 Executor。
- 七组 Android 启动、安装草稿、清单声明、资源呈现及版本选择夹具改用 Lua 元数据。API 29 隔离 APK 的 48 项 instrumentation 通过。真实签名的 Lua 目录另在同 API 通过签名绑定及文档选择器 UI 两项测试，验证确认策略前后均不会自动批准插件源码。
- Android 签名归档基础夹具已换成 Lua，保留存储和 DEFLATE 正例及身份、完整性和 CRC 三个故障样例。旧 JavaScript 签名目录和源码夹具已删除。API 29 的归档读取和 Core 预检共 23 项通过。归档声明与实际文件选择器测试进一步核对 Lua 的角色能力、程序约束及 RPC 方法声明，API 29 另通过三项。
- macOS 官方 Lua Preview 原生闸门整轮通过。10 个流程分别以有窗和离屏方式运行，40 组隔离变异均通过目标断言红、独立对照绿、还原绿及轮末输入摘要检查。接入仓库原生驱动后，`--project-samples` 新一轮通过 16 项原生测试及两套样例的六个本地、直连或 ProxyJump 场景。
- Android 安装、安装中断与目录测试已切换到 Lua 签名归档。新夹具保留同一包的两种 ZIP 编码、完整性故障及独立渠道根签发的三代策略，删除旧归档声明数据。API 29 首轮目录界面误读旧路径，修正后同轮 15 项安装、目录、归档声明及真实文件选择器测试通过。进程中断专项另记。
- Android 版本选择、切换、移除和策略审查已换成两个签名 Lua 版本，保留能力差异、版本身份不匹配和预检期间禁止执行源码的探针。控制器回归及真实 SSH 中切换版本时保持既有 Session 的专项已启动。
- Android API 29 再次通过九个官方 Lua 包的真实设备安装专项。逐包检查落盘的完整性清单 SHA-256，重开已安装声明并确认安装本身不授予运行权限。Android 版本、策略和维护界面测试已接入 Lua 安装目录，设备组合正在验证。
- Android API 29 的 12 项版本、策略、维护界面及终端返回测试通过。旧轮的两种文件选择与刷新时序问题和权限断言差异均已修复并复验。
- Android API 29 的安装中断专项通过 9 个检查点和 10 次实际进程结束。重开后验证安装树未重放、恢复与维护结果以及独立 Core 对照。真实两跳 SSH 的另一控制器切换版本专项也通过，既有 Lua Client 继续使用已接管的快照并完成双向 RPC。
- Android 版本控制器首轮 37 项有四条失败。Lua 能力差异夹具现声明两套不同的 Executor 程序，升级与回滚断言按实际 Lua Client 能力和程序约束核对。第二轮修正剩余的旧权限数量断言后，API 29 复验 37 项通过。Apple、Android 声明显示已移除 schema v1 解码分支，Android API 29 的 12 项声明、归档和控制器专项也通过。
- 删除 Core 的旧 QuickJS 构建开关后，离线闸门通过 610 项 CTest、207 项公共 ABI 和未实现入口冒烟。macOS 插件页可将已配置的命令加入当前工作区，主菜单与 ⇧⌘P 打开可搜索的面板。面板按工作区分发并沿用原授权检查，通过 16 项原生测试及 Table 六个本地和 SSH 场景。macOS 另通过五项 Lua 清单显示测试。
- macOS 增加 ⇧⌘Y 命令绑定。偏好只保存包 ID、摘要和命令路径，启动配置仍属于当前进程的工作区；快捷键和面板使用同一命令入口。16 项原生测试及六个 Table 场景通过快捷键接线复验。
- `togglePanel` 已接入 Lua 静态构建、Core 包读取、CLI 检查和 Swift、JNI 命令绑定。Lua 闸门拒绝类型错误及没有视图的启用声明，并读取实际 Table 包的布尔值。Core 610 项、207 个公开 ABI 符号及 Swift、JNI 绑定对拍通过。macOS 的 16 项原生测试和六个 Table 场景通过，验证重复触发关闭、清除表格数据和释放 Runtime。Android 首轮遗漏 App 模型字段导致编译失败，补齐后 API 29 的 24 项原生测试、四个 SSH 场景和摘要检查通过。Android 当前验收范围为清单字段读取。
- macOS 传输节奏变异已接到官方 Lua Preview。256 KiB PNG 首轮基线未收到资源，已增加收件状态诊断并显式提高业务读取上限。尚无变异验收结论。
- Android 最新 Lua 命令 ABI 对应未签名 Release/R8、四模块 lint 及 APK 内 Lua 许可复验通过。
- macOS 工作区准备命令已改用独立调用 owner，共享已批准的包目录。16 项原生测试及六个本地、SSH 直连、ProxyJump 场景通过。每个场景同时打开 A/B，验证数据归属，关闭一侧后继续向另一侧请求分页。
- Android 的业务环境保护已接到真实 SSH 上的 Lua Claude 进程夹具。获选变量内容传给 CLI，未选的环境标记应被滤掉。仅使用本地协议夹具，设备与变异复验进行中。
- Android API 29 的显式业务环境基线通过，真实 SSH 上的受控 CLI 验证了获选变量和未获选变量的隔离。变异首次构建遇到并行 ABI 变更，已同步调用方并复验。Lua 闸门通过 Client 精确上下文字节和 Executor 缺省的配对断言。Android 原生入口已传入可确认的终端窗格身份，目录未知时省略。含 JNI 的原生复验正在执行。
- macOS 的 256 KiB PNG 直连专项测到资源接收超过默认 5 秒激活时限。仅对声明 `resource.receive` 的 Client 使用 15 秒激活时限后，原生面板通过原始资源字节对拍。将每轮最多 32 次处理减为 1 次后，单条资源接收断言触发超时，同一变异构建的独立对照、恢复后的资源用例及源码 SHA256 对拍通过。该轮后续前台夹具失败单列。
- macOS 的业务环境变异入口已改用 Claude Lua、真实 SSH 和无模型进程夹具。首轮在执行第一条输入前检查进程回执，当时 Executor 尚未启动 CLI；把断言移到第一条输入得到终态之后，真实直连与 ProxyJump 基线通过。隔离变异副本初轮遇到 SSH 命令 1024 字节上限。缩短路径后观测到环境回执，但首轮业务响应未确认。调整夹具目录层级后，隔离运行仍出现业务响应未确认。环境变异改用实际进程在首条请求时写出的回执。真实 SSH 直连的基线、单条目标断言红、同构建独立对照、恢复及源码 SHA256 对拍均通过。完整会话仍由独立的直连与 ProxyJump 基线覆盖。
- Swift Core 的运行时夹具改用 Lua，启用档 14 项通过，涵盖双向 RPC、Session 所有权、CPU 与加载失败、跨线程停止、256 KiB 发布和超限收件释放。关闭档的一项测试确认 Lua 不可用且 Session 未被接管。初轮的旧 JS jobs 计数断言和重复传输夹具失败后，改查运行步数及独立传输收据。启用档已加入 Lua 闸门。首次把带 OpenSSL 签名验证的 CLI 静态库直接链接到 Swift，链接失败。改用单独构建的 Swift 宿主 Core 后，整套闸门通过，含 44 项 Core 测试、14 项 Swift、两跳和三跳 SSH、九个签名包及前台挂接。
- 调用上下文接入后的离线 Core 闸门通过 610 项测试与 207 个 ABI 符号对拍。Apple 绑定引用 204 项、登记未引用 3 项，Android JNI 引用 168 项、登记未引用 39 项，两端对拍均通过。C 格式检查覆盖 269 个文件。
- macOS 加入调用上下文后，16 项原生测试和六个 Table 并行工作区场景复验通过。Android 业务环境变异的初轮观察到泛化会话失败。随后在 API 29 上，通过 Lua Claude 的实际 SSH 调用及夹具回执验证选中变量到达、未选变量缺省。删去选中变量后正常编译并触发单条目标断言，同一 APK 的 Core 配置对照、恢复后的目标用例及源码 SHA256 对拍均通过。API 29 另通过一项 JNI 调用上下文测试，包括非 ASCII 工作区与面板标识。
- Android API 29 的实际两跳 Preview 设置页测试通过。启动记录保留原终端窗格身份，未知目录省略，关闭后回到同一 PTY，隐藏的键盘保持隐藏。
- `gterm plugin` 已接入随 CLI 定位的开发工具。新增构建目录和安装目录的命令对拍，验收检查、试跑、可复现 ZIP 及签名命令入口。
- 加入 Table 项目样例后完整 Lua 闸门通过，包含九包签名和多路复用。C 格式 269 个文件通过。两个样例的 macOS 原生渲染在隔离副本启动复验，Android 原生渲染另行进行。
- Android 原生样例首轮通过 24 项基线，真实渲染发现样例行 ID 含冒号，违反原生 ID 契约。已将 ID 改为连字符并启动 Android、macOS 和 CLI 复跑。CLI 探针改为逐字段核对两行 ID。
- 修正后的 Table 项目多路复用通过，串用项目数据和接受旧分页键两组 Lua 变异通过目标断言、同 Core 配置对照、复原和摘要。变异已接入 `test-gterm-multiplex.py --case table-projects --mutate-table`。
- Android API 29 的项目样例复跑通过 24 项基线、两个样例的实际直连及 ProxyJump 渲染、模拟器退出和最终核对。macOS 隔离首轮本机 A 通过，B 的 OCR 识别断言失败。已改为独立核对模型文字及原生任务单元格的无障碍标识，复跑另计。

- 当前 Core 把真实 Lua gterm 与同源原生 Host 探针经隔离 SSH 相连。两跳、三跳各通过纯双向 RPC 和含文件资源传输的双向 RPC。纯 RPC 夹具按该模式协商与清理，资源夹具仍核对文件内容、摘要和释放。

- 多跳用例已纳入 `gate-lua-plugin.sh`，并随当前 Lua 闸门通过。测试脚本和 gterm 使用同一 Core 构建，两跳、三跳的资源与纯 RPC 均由专用断言验收。

- Lua 本机日志脚本首轮通过四个独立进程退出后的新进程检查和显式释放，随后 Swift 阶段超时。已用独立 Swift 缓存、当前 39 项测试数量和 Lua 静态库摘要复跑。

- Lua 本机日志补验通过四个进程退出恢复场景及 39 项原生测试，包括先前因缺夹具而跳过的两项 TERM-ignore 用例。

- 当前工作区的 macOS Views 变异复跑通过全部目标、独立对照、恢复和最后的输入摘要核对，本机、SSH 直连及 ProxyJump 的实际用例一并通过。

- Lua 参数上限变异通过。将启动参数缓冲区替换为 RPC 上限后，真实 Diff 宽字符调用在启动处准确触发断言。较短调用和独立配置对照均通过，复原后全部 Diff 探针用例通过，源码摘要一致。

- Android Views 在 API 29 与 API 36 各通过 31 项原生注册、模型与 UI 测试，另通过 Table、Diff、Task 独立回归及真实 SSH 直连和 ProxyJump。最终源码与构件摘要核验通过。Android Views 变异待验。
- Android Views 在 API 29 的 15 组常规变异通过，覆盖实例与代次、视图注册与释放、聚焦、无障碍、任务轮询及原生绑定。真实 SSH 直连与 ProxyJump、同 APK Core 对照、复原及最终摘要核对通过。一次性前台挂接失联专项在 API 29 上通过 31 项原生对照及直连、ProxyJump 各两项实际场景。失联变异目标断言、同 APK Core 对照、恢复与变异切片 SHA-256 已通过。整轮摘要守卫发现运行期间格式化了新 C 文件，拒绝签发回执。格式化后的固定输入复跑重新通过全部 31 项原生对照、直连与 ProxyJump 各两项实际场景、前台失联变异及最终源码和构件摘要，脚本退出码为 0。
- Android Views 在 API 29 的前台挂接失联专项通过。直连和 ProxyJump 均验证挂接与失联，目标断言、同 APK 对照、复原及最终回执通过。
- Lua Table 的 CLI 服务协商在本机通过三个拒绝用例，分别覆盖未知服务、重复服务和缺少 RPC 声明。服务白名单及 RPC 声明检查两组变异均经实际编译后触发目标断言，同一 Core 归档的配置对照通过，恢复后复验通过并核对源码 SHA256。CLI 测试构件开启 ASan 与 UBSan。

## 13 源码落点与官方资料

- 受控进程专项在 macOS 使用一次性本机包和摘要固定的 `/bin/cat`。Lua 拒绝未声明程序，按授权根启动回显，核对 stdin、stdout、EOF、退出码和回收。范围为 Core Session 及 Lua 进程桥。Codex / Claude 协议与平台原生验收另记。
- Codex Lua Executor 接入命令与权限审批事件。Lua 5.5.1 的进程和宿主 RPC 夹具核对了数值与字符串请求 ID、未提供决定和重复决定的拒绝、其他回合的审批隔离、权限审阅令牌不匹配时不发送响应、允许后只授予当前回合的已审阅范围、拒绝后返回空授权，以及未知权限字段导致协议失败。
- Codex Lua Executor 将文件补丁保存在内存，宿主使用 UTF-16 偏移分段读取，批准时检查当前补丁版本。Lua 5.5.1 的模拟进程与 RPC 夹具核对了代理对中间偏移、重复补丁复用、过期版本拒绝和当前版本批准，另用 480,000 字节含非 BMP 字符的快照核对分段与完整重组。Codex 进程 JSON 上限为 1 MiB，单个补丁快照上限为 512 KiB，全部快照内容上限为 1 MiB。RPC 报文与 JSON 工具默认仍为 3072 字节。Lua 专项闸门在实际宿主沙盒核对了 256 KiB JSON 往返、默认与显式上限、超限、重复键拒绝以及大对象中的数组、null、布尔与小数。Codex 包静态构建通过。前一轮离线 Core 闸门 610 项与 207 个 ABI 符号通过，Apple 与 JNI 引用对拍通过。原生界面和真实 CLI 仍待端到端验收。
- macOS 的 `test-plugin-codex-patch-app.py` 以 Lua 内置包、实际 gterm、原生界面和隔离本机 sshd 通过 8 项。直连与 ProxyJump 均覆盖两版完整补丁的审阅、过期审批拒绝、当前版本批准、中断和最终事件、回复丢失、Executor 断线。首轮发现宿主不识别包声明的通用聊天和审批能力，改为沿用命令专用视图声明。第二轮小分段读取超过测试时限，扩大分段后复跑全绿。app-server 为无模型协议夹具，真实 Codex CLI 另行验收。
- Android API 36 arm64-v8a / 4 KiB 页的隔离模拟器通过 8 项原生 Settings 专项。直连与 ProxyJump 均覆盖两版完整补丁、过期审批拒绝、当前版本批准、Host 关闭后的 Executor 未知结果、中断与最终事件、回复丢失和 Executor 断线。使用实际 gterm、SSH 和无模型 app-server 夹具。首轮测试类仍在退役 JS 源集中，初始化失败。迁入现行测试源集后重建 Debug 应用及测试 APK，在模拟器安装后对拍摘要。补丁审批两项与故障场景六项分轮使用同一组 APK 和插件包。JNI 引用对拍为 168 项引用、39 项登记未用。
- Android Lua 档未签名 Release/R8 构建和 app、bridge、data、terminal 四模块 Release lint 通过。初次误用 `:domain:lintDebug`，该 Kotlin/JVM 模块没有此任务，改用仓库规定的四个 Release lint 入口后通过。
- `mutation-check-android-codex-review.py` 已切到 Lua 构建档。在隔离 API 36 模拟器上通过只读界面、补丁身份、JSON schema 三组。每组变异成功编译后单条目标断言按预期失败，同一 APK 的 Core 对照通过，恢复源码后目标断言及 SHA256 对拍通过。范围为 Android 审阅端。
- `mutation-check-plugin-codex-changes.py --lua <Lua 5.5.1>` 直接加载隔离副本中的 Codex Lua Executor，用进程、JSON 与 RPC 夹具通过五组补丁变异。覆盖快照 ID、单快照容量、分段报文预算、审批对应的文件项和过期版本。每组确认编译成功、目标断言按预期失败、同一变异源码的会话启动对照通过，以及复原后目标断言与 SHA256 对拍通过。报文预算用需转义的内容与固定封套开销断言。真实宿主和实际 SSH 的绿色专项另见上文。
- macOS 使用已校验 SHA256 的官方 Codex CLI 0.166.0。协议检查对拍 Lua 版本常量及官方 CLI 生成的类型和 JSON Schema。`test-plugin-codex-handshake.py` 在独立配置目录运行实际 Core、gterm 与 Lua 插件，确认 app-server 完成 initialize 和临时 thread/start。首轮构建漏启 CLI 测试目标，其后诊断出夹具未创建 CODEX_HOME，创建后通过。该证据限于无账号握手，真实模型回合与工具调用另验。[Codex CLI 0.166.0](https://github.com/openai/codex/releases/tag/rust-v0.166.0)
- Codex 双 Runtime 探针用现场构建的 Lua 包和官方锁定版 0.166.0 复验通过。macOS 上实际 Lua Client Runtime 与 gterm Executor 验证线程创建及事件、过期补丁快照结果和双方关闭。归档与官方 SHA256 一致，使用隔离配置，未启动模型回合。
- Claude Lua Executor 分段接收 stream-json 行，单行上限 512 KiB，响应收据上限 128 KiB，宿主继续分页读取收据。中断后丢弃残余 stdout，进程回收后才允许再次发送。Lua 5.5.1 模拟进程夹具验证约 12 KiB 含非 BMP 字符的消息跨多块完整交付、终止中断后进程回收与再次发送，以及未中断的截断输出显式失败。Claude 包静态构建通过，官方 CLI 与原生宿主仍待验收。
- Claude 两端原生宿主识别 Lua Executor 的 CLI 后端。Lua 以独立命令序号生成收据，按当前输入关联结果，子进程回收后才交付终态。macOS 的 `test-plugin-claude-app.py` 用真实 SSH 直连和 ProxyJump、实际 gterm、原生控件与无模型 CLI 夹具通过两项，覆盖长 Unicode 消息展示、关联结果、中断、进程回收和停止。首轮原生命令序号与收据 ID 混用导致收据拒绝，修正后双路通过。Android Lua 档 Debug 应用和测试 APK 构建通过。旧 Node 夹具的回包超时、只读观察、工具审批和提问未计入本轮。真实 Claude CLI 和 Android 原生专项另验。
- Android 的 `test-plugin-claude-lua-android.py` 在隔离 API 36 arm64 / 4 KiB 模拟器上通过三项，安装后对拍应用与测试 APK 摘要。SSH 直连与 ProxyJump 均经原生 Settings 验证长 Unicode 消息、结果关联、中断与回收、停止与释放。独立参数用例覆盖连续 200 次空参数解码和未知参数拒绝。首轮发现启动表单仍要求 `claude-node` 映射，改为 `claude` 后通过启动。随后夹具过早点击中断，等待 Host 确认输入并启用按钮后复验通过。使用无模型 CLI 夹具，真实 Claude CLI、回包超时和只读观察未在本轮验收。
- Claude 运行中停止会等待子进程回收后返回关闭终态，收据保留尚未确认的输入 ID。macOS 两项、Android API 36 三项以同一插件包复跑通过，四条 SSH 用例均在 CLI 正等待时触发原生停止，核对关闭状态与最后输入结果未知。该轮仍使用无模型夹具。本机 Claude CLI 为 2.1.119，插件锁定版本为 2.1.323。
- 使用官方 macOS arm64 Claude CLI 2.1.323，按发行元数据核对 SHA512 后直接提取原生二进制。隔离且禁网的 CLI 验证确认实际参数可用，并发现未登录结果同时携带 `subtype: "success"` 与 `is_error: true`。Lua Executor 和两端宿主按 `is_error` 判定失败，保留原始字段。macOS 实际 gterm、Lua 插件、SSH 直连和原生控件通过未登录失败端到端，Android Lua Debug 应用及测试 APK 构建通过。该轮未使用账号或执行模型、工具、审批和提问。[Anthropic 官方原生包](https://www.npmjs.com/package/@anthropic-ai/claude-code-darwin-arm64/v/2.1.323)

- Core 与公开接口见 [core](../../core) 和 [ghosterm.h](../../core/include/ghosterm.h)。原生宿主见 [apple](../../apple) 和 [android](../../android)。
- macOS Claude 断线探针在收到待处理输入后，仅终止该轮启动的远端 relay，检查结果保持未知和 Host 清理。首轮未到断线点，因为短目录前缀与无模型 CLI 夹具的校验约定不同。恢复目录前缀后，真实直连与 ProxyJump 均通过。Host 保留原输入标识和未知状态，随后释放运行所有权。Android API 29 直连与 ProxyJump 均通过同一场景。安装 APK 与构建产物摘要一致，两路各一项原生断言通过，远端 gterm 均返回 130 和已取消诊断，Host 保留原输入标识及未知状态并释放资源。
- macOS 签名安装全组首轮运行 35 项。两处安装夹具仍查询 JS 入口或旧清单的权限说明，导致三条断言失败。已改用 `main.lua` 篡改测试及 Lua 清单的准确 Client 能力断言，扩展到安装、版本与策略的 125 项复验还发现四个旧 Host 夹具缺 `session.rpc`，以及版本按钮在 AX 重绘时取到过期节点。补齐现行 Preview 能力并在同次等待中读取和触发按钮后，复验定位到余下的旧清单文件名及权限差异夹具。改用 schema v2 清单并给签名候选声明实际的执行端能力增减及程序映射后，125 项 XCTest 零失败。驱动仍期待 124 项，修正计数后，125 项基线及活动版本、回收、策略审查、渠道撤销和版本选择五组均通过。由于活动场景的宿主构建重用了 CLI 的 Core 目录，最后归档摘要对拍失败。改用单独预构建的 Swift 宿主 Core 后，125 项及五组实跑再次通过，工具、归档与源码摘要全部对拍，验收收据通过。该轮为原生基线，未新增变异组。
- macOS 原生精确包批准与日志搜索夹具改用 Lua。隔离副本两项通过，覆盖两个同名候选的批准与拒绝、参数 schema，以及首轮无命中后继续搜索、显示命中行并清除查询。
- Views 的 16 个结构用例迁移到 Python 协议校验器，保留 `packages/plugin-protocol/schema/views-v1.candidate.json`。独立校验通过这 16 个用例、82 个桥协议用例及四组桥 schema 变异。实际 Lua 注册、重复和容量、代次、关闭和重开由 Core 与两端的原生测试验证。已移除旧 Node/Ajv 适配器夹具。
- Preview 的两个 Lua 角色和生产 gterm 在实际两跳 SSH 上通过同一受控根目录的正常文件读取、上级路径拒绝、末级文件链接拒绝和中间目录链接拒绝，三种拒绝均无资源回执。这组检查已接入现有 Lua 闸门的 SSH 入口。
- 已移除仅可运行旧 Claude Node SDK 的探针、npm 夹具及 C peer 构建目标，以及无调用方的 gterm Node 夹具。Lua Claude 使用两端的原生无模型夹具和固定版本的未登录探针。移除旧目标后，Lua 闸门复跑通过 44 项 Core、14 项 Swift、两跳与三跳 SSH、九个签名包及前台挂接。旧脚本中的账号回合仍查阅迁移前记录，Lua 版的真实模型回合待单独验收。
- TypeScript / QuickJS 的既有实现、P0–P6 分批验收和发行记录见[迁移前计划快照](https://github.com/hanxi/ghosterm/blob/c579ed985b92c82260aa91500faf927d447aaf11/docs/reference/plan-plugin-bridge.md)。这些结果是迁移前的行为基线。

## 历史验收引用

<a id="p6-实施第十九批android-安装暂存持久归属与显式维护2026-11-17"></a>

- [迁移前记录](https://github.com/hanxi/ghosterm/blob/c579ed985b92c82260aa91500faf927d447aaf11/docs/reference/plan-plugin-bridge.md#p6-实施第十九批android-安装暂存持久归属与显式维护2026-11-17)

<a id="p6-实施第二十批android-归属未知目录的完整核对与显式删除2026-11-17"></a>

- [迁移前记录](https://github.com/hanxi/ghosterm/blob/c579ed985b92c82260aa91500faf927d447aaf11/docs/reference/plan-plugin-bridge.md#p6-实施第二十批android-归属未知目录的完整核对与显式删除2026-11-17)

<a id="p6-实施第二十一批android-当前策略复核与显式批量禁用2026-11-17"></a>

- [迁移前记录](https://github.com/hanxi/ghosterm/blob/c579ed985b92c82260aa91500faf927d447aaf11/docs/reference/plan-plugin-bridge.md#p6-实施第二十一批android-当前策略复核与显式批量禁用2026-11-17)

<a id="p6-实施第二十二批android-升级回滚的完整能力声明差异2026-11-17"></a>

- [迁移前记录](https://github.com/hanxi/ghosterm/blob/c579ed985b92c82260aa91500faf927d447aaf11/docs/reference/plan-plugin-bridge.md#p6-实施第二十二批android-升级回滚的完整能力声明差异2026-11-17)
