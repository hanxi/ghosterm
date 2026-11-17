# gterm 前台 CLI

独立构件，不加入 Ghosterm App 包、不安装远端 helper、不修改 shell 配置。
当前接入诊断、固定包兼容性检查、显式 Portable / Node / Exec stdio Executor、一次性前台 IPC、实验 App 的显式 SSH attach、本机启动、被动发现，以及开发 / 签名执行端目录的普通命令分派。
它不内置业务命令；未配置目录时保留 `COMMAND_ACTIVATION_UNAVAILABLE`。签名执行端可离线安装并显式替换登记；完整升级 / 版本维护与发行矩阵继续。

从仓库根目录构建（产物在工程 `tmp/`）：

```sh
cmake -S cli/gterm -B tmp/gterm-build \
  -DGHOSTERM_WITH_PLUGIN_JS=ON -DGHOSTERM_WITH_LIBSSH2=OFF
cmake --build tmp/gterm-build -j 4
./tmp/gterm-build/gterm doctor
./tmp/gterm-build/gterm plugin check --host-json ./host.json -- ./plugin-directory
```

`host.json` 是调用方明确提供的能力观察，遵循包检查器的 host 事实格式。例如：

```json
{"side":"executor","sdkVersion":"1.0.0","protocols":[1],"runtimes":["portable-js"],"capabilities":["session.rpc"]}
```

兼容性检查不验证这些事实对应的服务确实已连接，也不证明发布者身份或授予执行权限。
省略 `--host-json` 时保留 UNKNOWN；不猜测用户环境。该文件限 64 KiB，拒绝 NUL、
最终路径符号链接及非普通文件。包根目录最终路径不跟链接，内部遍历、预算、完整性及
metadata 校验由公共 Core 包 owner 完成。接受相对路径；`--` 后的目录名不再解析为选项。

成功输出单个 JSON 对象，包含包摘要、发布者声明与命令贡献；非 ASCII 字符使用 JSON
Unicode 转义，终端控制字符不会直接写到终端。检查不执行包模块：即使入口含顶层抛错，
元数据兼容的包仍可通过。`compatible` 只表示与给定 host 事实兼容，来源及运行批准固定
为 false。关闭引用失败返回非零；输出错误也不得返回零。

退出码：0 为检查兼容或所查询的 JS 引擎可用；2 为参数/host 输入错误；3 为 UNKNOWN；
4 为不支持；5 为包校验、IO 或其它失败；130 为收到取消。stdout 为 JSON，参数用法与
关闭诊断写 stderr。SIGINT/SIGTERM/SIGHUP 只设置取消标记，采集循环在下一步停止；同步元数据
校验受 Core CPU 预算限制，返回后检查标记，不在 signal handler 调 Core。

```sh
python3 scripts/test-gterm.py --binary tmp/gterm-build/gterm
python3 scripts/mutation-check-gterm.py
```

变异 runner 只改工程 `tmp/` 中的 CLI 副本，锁定 SDK 工具离线构建；Core / 上游为只读
输入。实际重新编译 CLI，目标正常断言红、同 archive 配置对照绿、复原绿并核对源码摘要。
这是 CLI 检查入口验收，不替代未来激活、IPC、SSH 或实际业务插件验收。


显式 stdio Executor 由持有专属 pipe/socket 的原生 Host 启动：

```text
gterm relay --stdio --trust-development-digest PACKAGE_SHA256 --client-digest CLIENT_SHA256 [--method NAME]... [--host-method NAME]... [--root ID DIRECTORY]... [--process-program NAME ABSOLUTE_PATH VERSION SHA256]... [--process-argument NAME VALUE]... [--process-environment NAME]... -- DIRECTORY COMMAND [ARG...]
```

默认运行档仍为 Portable；Node / Exec 的显式配置见下节。
这是当前的前台启动协议，不是向交互终端写回的通道；stdin/stdout 任一为 TTY 都拒绝。
stdout 只传 Core 协议，诊断只到 stderr，且不记录参数、模块异常文本或授权 token。
Host 必须管理专属 stdio 的对端归属；`--stdio` 本身不实现 socket 认证。一次性本机接入使用下节的独立入口。

### 原生宿主明确登记的多实例组（实验）

```text
gterm relay --multiplex --connection HEX32 --instance HEX32 EPOCH ARGV_HEX [--instance HEX32 EPOCH ARGV_HEX]...
```

同一前台 gterm 进程接管一个专属 stdin/stdout，最多八个实例复用同一物理连接。
每项仍是完整的 `--stdio` 启动配置：固定包 / Client 摘要、运行档、RPC 方法、根目录、
执行许可、程序映射及 `-- DIRECTORY COMMAND [ARG...]`。原生 Host 明确分配 connection、
instance 与非零 uint64 epoch，两端按相同身份登记；epoch 遵守 Core 的六十四代准入
窗口，不重复、不回绕。每个 Session 分别握手和批准，远端消息不能创建或授权实例。
整组先完成参数、固定包 / 命令 / 来源预检，再开始任何 Session；签名版本引用持续保留，
运行前照常再次核对当前策略。拒绝、结束或激活失败只拆除对应实例，其余实例继续。

`ARGV_HEX` 是 CLI 私有、未冻结的 argv-v1 编码，不改变 wire v1：字节 `01`，随后
BEu16 参数数量，再逐项 BEu16 UTF-8 字节长度和原始字节，最后转为小写十六进制。
不含 C 字符串终止 NUL；空参数长度为零；不得截断、带尾随字节或内嵌 NUL。每项
1–256 参数，编码前至多 64 KiB，整组参数含终止 NUL 的累计大小也至多 64 KiB。
实际 Native 启动接口另有自己的参数数量 / 字节上限，必须在启动前核对；compact 编码
使八项普通配置可以放进现有 64 个 Native argv 的数量上限。此编码不经过 shell 求值，
业务 argv 之后仍按包的命令 schema 校验。

同一非 UI owner 轮转驱动各实例的 Session / Runtime 与关闭状态，继续使用 Core 的有界
mux 队列 / 调度。资源共享一个私有 Store：128 MiB 总量、64 项、每资源 1 MiB、每实例
16 MiB scope 预算，具体传输仍受 Session 协商值限制。资源 token / 完整 lease、独立
根 fd 与源文件授权继续核验。需要显式的绝对 `TMPDIR`；本文仓库内执行统一设为工程
`tmp/`。关闭等待按实例驱动，正常结束后释放共享连接、版本引用和私有目录。

stdout 仅为 Core 帧，stderr 报公开实例 ID / epoch / 本机退出码和清理错误，不输出
argv、环境值、授权 token 或模块异常原文。整组退出码取配置顺序中首个非零实例结果；
清理错误覆盖为 5。零只表示所有本机实例按已有通道结束规则退出并完成本机清理，
不能替代具体业务回执。SIGINT / SIGTERM / SIGHUP 取消整组；不杀调用 shell 或 tmux。

这是固定启动组入口，尚未接在线添加 / 重用槽位、`--listen` / 被动发现或两端产品实例
管理，不据此声明 SSH / ProxyJump、Node / Exec 混合八实例或完整生命周期矩阵通过。
本机关键冒烟：

```sh
python3 scripts/test-gterm-multiplex.py --build tmp/gterm-build
```

### 一次性前台 IPC 与显式 SSH attach（实验）

```text
gterm relay --listen ABSOLUTE_PRIVATE_DIRECTORY [与 --stdio 相同的显式包/运行档参数]
gterm relay --inspect ABSOLUTE_PRIVATE_DIRECTORY --instance HEX32
gterm relay --attach ABSOLUTE_PRIVATE_DIRECTORY --instance HEX32 -- COMMAND [ARG...]
```

调用方先创建自己拥有的 0700 目录，路径必须为规范绝对路径，不跟最终目录链接。
listen 是原前台 Executor 进程，不 fork daemon；就绪诊断只给本次可公开的 32 位十六进制实例标识，不输出配对秘密。
attach 由明确选定目标的无 PTY SSH / 原生管道启动，stdin / stdout 必须为 pipe 或 stream socket。
两端核对系统提供的对端 UID；临时 0600 permit 保存独立的 256 位随机秘密，不进 argv、TTY 或日志。
attach 必须提供本次公开实例标识与精确的命令 / 原始 argv，包括空字符串和 Unicode 字节。
同一目录后来重新启动的进程拥有新标识；旧页面或命令 / 参数不匹配在 Session 前拒绝，不能消费或激活现有实例。
`--inspect` 使用同一私有目录、系统 UID 和凭据校验，只从仍在线的 listener 读取固定调用声明，
不消费 permit、不启动模块、不延长等待期限；重复检查也计入 16 次连接上限。
输出为最多 150 KiB 的 ASCII JSON，包含公开实例、两端摘要、运行档、命令、原始参数、
RPC 方法和 Client 服务要求；不包含凭据、包 / 根目录、程序映射、环境或固定进程参数。
这只是前台启动配置的声明，不重新检查包内容、不证明兼容或授予来源 / Session 权限。
与 attach 相同，它拒绝 TTY / 普通文件 stdin/stdout，须由明确选择目标的 Native pipe 或无 PTY SSH 调用。
**BREAKING（未冻结的 IPC 原型）**：不再接受省略实例与调用的旧 attach 命令，私有准入前缀为版本 2；Core wire v1 不变，不回退旧入口。
正确接入后，先移除 permit 和 socket 名字，再交给既有 stdio Session；包摘要匹配、consent、
能力与资源授权全部照常执行。接入不等于来源信任，也不授予插件运行许可。

同账户恶意原生程序不在此边界内，拥有该目录读取权的可信调用方必须得到用户授权；这不是 OS 沙箱。
等待总计 30 秒，单次认证最多 1 秒，最多接收 16 次连接；attach 接入最多 5 秒，转发最多 10 分钟。
转发每方向仅持有 16 KiB，stdin EOF 在已排队数据发送后执行 SHUT_WR，反方向继续读取。
attach 的 0 只表示传输结束，业务成功仍看 Session 结果及原前台进程退出，不能从 EOF 推断。
正常退出 / 取消只移除本进程仍匹配 inode 的入口；替换文件保留并报清理失败。
SIGKILL 可能留下无效文件，进程退出即无可附着的 listener，不自动删残留、重启或复用。

实验 App 的插件页可明确选择已有前台实例、目录与公开标识，另行允许连接；修改主机 / 调用 / 实例配置会撤回允许。
macOS 与 Android 页支持先检查在线声明，再明确填入命令 / 参数 / 摘要 / RPC 表；填入要求匹配所选 Client
摘要与命令（Android 对应当前选中的命令），并清除此前连接、已有只读范围、业务进程与 Native 工作区批准。更改位置后旧声明不能再填入。
仍需选择已批准的 Client 包、预期 Executor 摘要和 RPC 方法，保持真实 Session consent。
前台的运行档、根目录和程序映射不能从此连接页重配；需要沿用前台已选只读文件范围或业务进程服务时，分别明确允许。
macOS 已有显式本机启动和附着。两端实验 App 现在可被动显示终端中的候选提示；
用户仍须自己选择实际执行位置，检查在线声明并重新批准。不会从输出猜测目标、自动 SSH 转发或恢复旧会话。复现：

```sh
python3 scripts/test-gterm-ipc.py --build tmp/gterm-build
python3 scripts/test-gterm-inspect.py --build tmp/gterm-build
python3 scripts/test-gterm-discovery.py --gterm tmp/gterm-build/gterm --observer core/build/test_ext_discovery --tmux /opt/homebrew/bin/tmux
python3 scripts/mutation-check-gterm-ipc.py --build tmp/gterm-build
python3 scripts/test-plugin-execution-ssh-app.py --macos --attach --build tmp/gterm-build --node 已安装Node路径 --sdk 已构建HostSDK目录
```

测试使用工程内隔离 endpoint 和真实 Core Host Session；变异只编译 CLI 副本，
ASan/UBSan 覆盖 CLI 对象，Core archive 与 native Host / 配置对照共享且不被修改。

### 执行端开发目录与普通命令（实验）

先创建本用户拥有的两个规范绝对路径目录，权限均为 0700：一个保存目录登记，另一个
承载本次前台 IPC。没有默认扫描 cwd / HOME、自动安装、常驻进程或终端字节流路由。
以下是登记的语法；占位路径 / 摘要需由调用方明确提供：

```text
gterm plugin list --catalog ABSOLUTE_CATALOG_DIRECTORY
gterm plugin register-development --catalog ABSOLUTE_CATALOG_DIRECTORY --expect-revision 0 --host-json HOST_JSON --trust-development-digest EXECUTOR_SHA256 -- ABSOLUTE_PACKAGE_DIRECTORY --client-digest CLIENT_SHA256 --root workspace @cwd
gterm [--catalog ABSOLUTE_CATALOG_DIRECTORY] [--endpoint ABSOLUTE_ENDPOINT_DIRECTORY] [--select PLUGIN_ID EXECUTOR_SHA256] [--command CANONICAL_PATH] COMMAND [ARG...]
gterm plugin unregister --catalog ABSOLUTE_CATALOG_DIRECTORY --expect-revision REVISION --id PLUGIN_ID --expect-digest EXECUTOR_SHA256
```

`register-development` 先经公共 Core 捕获包、完整性 / 兼容性检查并核对调用方提供的
开发摘要，只登记 Executor。它保存包 ID / 版本 / 摘要 / 原目录、调用方的 host 观察
以及目录之后的 stdio 启动配置；不执行模块，不把 publisher 声明当成身份认证，不
授予 Session 权限。host 观察不是已连接服务的证明，实际 stdio 仍按真实配置重新检查。
根路径可显式选 `@cwd`：每次调用使用本次前台进程的 cwd，由 fd 跨 IPC 等待保留，
不保存登记时 cwd，也不把任意插件字符串解释为路径。固定绝对根不改变原行为；未选
该配置时不自动授权 cwd。官方 Preview 可在所选根内处理 `./docs/file` / `docs/./file`，
仅移除 `.` 身份分量；绝对路径、`..`、空分量和目录目标仍不支持。原始调用参数不改写，
附件在同样处理后检查重复；完整执行位置与路径矩阵另验。
配置中的固定根目录、Node / SDK 与程序路径必须为规范绝对路径，固定参数按原始 argv
保存。只保存额外环境变量的名字，运行时仍取执行端显式选择的值。包 / 运行档 / 根 /
业务程序授权及每次 Session consent 各自保留，不能靠登记绕过。

登记完成后可为当前 shell 显式设置 `GTERM_CATALOG` / `GTERM_ENDPOINT`（与上述两个
全局选项同义），再直接运行 `gterm preview path` 或插件贡献的其它命令。CLI 将 argv
按声明 token 匹配命令，经 Core 注册表解析冲突并解码业务参数；不会自动选最长前缀、
按安装顺序抢占或解释 shell。重叠贡献要求 `--select ID DIGEST`；`--command` 可额外
固定完整规范命令，必须与所选贡献和实际 token 一致。空参数、Unicode 和字面引号保留。

每次分派重新捕获所有登记包并核对固定摘要 / ID / 版本，任一无效项整体拒绝；不会
静默跳过坏项后选另一个包。参数校验完成后才进入已有 `relay --listen` 的一次性
前台 IPC，端点目录仍由用户明确选择。终端给出公开实例就绪诊断；有控制终端且位于
前台进程组时，另尝试发送一条有界候选提示。用户在实验 App 中选择候选或明确填写
目录、公开实例、同一完整命令 / argv，经本机管道或已认证无 PTY SSH
attach，核对 Client / Executor 并决定本次 Session。业务字节不进入交互终端。
等待 30 秒、同用户准入、一次消费、拒绝 / 停止 / 关闭及清理沿用 IPC 规则。
这只自动发现不可信候选，不自动连接，也没有 TTY 双向能力。

### 被动终端候选（实验）

macOS 终端顶部和 Android 终端菜单提供手动打开候选页的入口，不弹窗或抢焦点。
提示只含公开实例、声称的 Client 摘要和目录。页面显示来源终端，外层 SSH 主机不等于
实际执行位置；使用候选只填写目录 / 实例并撤回旧批准，不选择主机或运行程序。
先批准匹配摘要的 Client，再自行选择实际位置、检查在线声明、确认范围和 Session。
列表最多八项、提示本机保存三十秒；原 listener 的三十秒期限不被延长。

私有 discovery v1 使用 `ESC ] 777;ghosterm-discovery;1; BASE64 ESC \\`。
Base64 原文依次为公开实例 16 字节、声称的 Client SHA256 32 字节、目录 UTF-8
长度 BEu16 和规范绝对目录（2–512 字节）；不包含私有 permit、hostname、程序或回包地址。
这不是已冻结的业务 wire v1。共享观察器只看有界输入，原始终端字节仍完整直送 Surface，
未知 OSC / APC / DCS 不会作为候选解析；业务数据继续走独立管道。

发送器单独非阻塞打开控制终端，只在当前前台进程组发送一次，不读取 TTY、不改变
共享 fd 标志或 termios。无 TTY / 后台作业静默保留手工入口；短写仅尝试 CAN 修复。
tmux 需用户显式配置 `allow-passthrough on`；检测到 `$TMUX` 默认包一层，嵌套时
显式设置 `GTERM_DISCOVERY_TMUX_LAYERS=0..4`，不会自动修改 tmux。真实单 / 双层有专项，
四层仅测帧包装；流控、短写 / SIGKILL、其它终端与完整拓扑矩阵留待集中验收。

目录更新要求 `list` 返回的当前十进制字符串修订号；首次无记录为 `0`。替换既有 ID
还须 `--expect-digest OLD_SHA256`，无选择覆盖、旧摘要或旧修订均拒绝。登记替换只影响
后续命令；运行选择在共享目录锁下固定，释放锁后保留本次固定配置，旧进程仍须自己
完成 Session 或显式停止。`unregister` 只改登记记录，不删除用户的开发目录 / 文件。

记录为未冻结的 CLI 私有二进制格式，与 App 的 Client 版本选择 JSON 无关。目录最多
32 项、文件最多 1 MiB；每项启动配置最多 256 个字符串 / 32 KiB，每字符串最多
4096 字节，host 观察最多 64 KiB。全部文本严格 UTF-8，拒绝 NUL、尾部数据、损坏 /
超限记录以及不满足 owner / 模式 / 普通文件 / 单链接约束的记录和锁；最终路径不跟
符号链接。不覆盖未知文件或自动修复损坏记录。同账户恶意 native 程序不在此边界内。

更新使用协作 `fcntl` 锁、原修订 / 文件身份复核、新建 0600 临时文件、文件 fsync、
原子 rename 与目录 fsync。失败不删替换文件；rename 后目录同步失败返回非零并保留
`committed=true, durabilityConfirmed=false`，此时先重新观察，不能盲目重放旧更新。
关闭失败不得先输出成功。SIGKILL 可能留下未知暂存，不自动删除 / 恢复。

基础退出码沿用 2/3/4/5/6/130，命令冲突 8、旧选择 / 修订 9、未找到 10、目录忙 11。
有序传输结束仍不等于业务成功；Session 和明确 RPC 回执是实际结果依据。

```sh
python3 -B scripts/test-gterm-catalog.py --build tmp/gterm-build
```

普通档默认跑 53 个场景。显式 `GHOSTERM_GTERM_TESTS=ON` 加
`GHOSTERM_GTERM_CATALOG_FAULTS=ON` 的专用测试构建另以 `--faults` 加三项文件同步 /
目录同步 / 消费式关闭故障；默认 CLI 不含这些注入入口。变异驱动
`scripts/mutation-check-gterm-catalog.py` 只允许工程 tmp 中的独立检出，真实编译后
逐组单条正常断言红、同 archive 配置绿、恢复绿和 SHA256。当前目录整链专项实际
运行 Portable 包与官方 Preview；三档普通命令与签名安装另见下一节，完整
文件系统 / 并发 / 物理设备矩阵另验，不以 headless C HOST 冒充 App 点击链。

`--trust-development-digest` 是调用方对这份固定开发包字节的显式来源选择，不是发布者
签名认证，也不代替 Host 的本次 Session 决定。包捕获、完整性/兼容检查、摘要和完整
命令贡献匹配均在启动协议前完成。Host 按实际 Client/Executor 摘要核对和授权；拒绝、
取消或不匹配不得激活插件。`--method` 与 `--host-method` 合计可重复至 16 项，分别
开放接收端为 Executor 与 Host、输入/输出根类型为 object、能力为 session.rpc 的方法；
必须由包声明 session.rpc。
具体业务 schema 仍由 SDK codec 检查，本入口没有实现任意 JSON Schema 服务。
当前 stdio Executor 提供 `session.rpc` 与有界 ResourceStore；只有显式配置 `--root ID DIRECTORY` 时才提供 `file.readScoped`。每个根以 `O_NOFOLLOW` 打开、验证为当前用户拥有的目录，最多 8 个、每个最多 16 MiB；插件只看到 ID 与相对路径，不看到宿主绝对路径。资源 Store 在 `$TMPDIR` 下创建随机 `0700` 子目录，总预算 16 MiB、单资源 1 MiB、8 个槽位，Session 结束时按 owner 顺序关闭并移除。需要 `resource.receive` 的 Executor 使用带资源 feature 的 Session；缺少 Store 或必需根时明确拒绝。`file.readScoped` 必须与 `resource.receive` 一起声明，普通 argv 仍通过显式启动参数传入。

Native 启动器可为 Executor 明确配置 `--process-program NAME ABSOLUTE_PATH VERSION SHA256`；该映射来自本次用户/Host 设置，不由插件包选择。只有同时存在至少一个显式 workspace root 和映射时，兼容性事实及 Session 才开放 `process.spawn` / `process.pipes`。Core 在 Runtime open 时核对绝对可执行文件 SHA-256，并在每次 spawn 前复核文件身份；程序名还必须出现在已审查包的 `process.spawn` manifest scope。`VERSION` 是此业务程序映射的 Native Host 版本声明，gterm/Core 不会为该映射自行运行 `--version` 探针，Codex adapter 会将它与锁定版本比较。子进程只收到 gterm 选择的 `HOME`、`PATH`、`XDG_CONFIG_HOME`、`CODEX_HOME`、`TMPDIR`（若存在），不会继承其余 SSH/gterm 环境。Host mapping 当前是显式命令参数；不要把摘要/版本当成来自插件自身的声明。

映射之后可重复 `--process-argument NAME VALUE` 固定该程序的启动参数；VALUE 即使以
`--` 开头也按字面值消费。每个映射最多 16 项、每项 4096 字节、合计 16384 字节（含 NUL）。
Core 复制全部字符串，在插件 argv[0] 后、插件 argv[1...] 前插入，不拼 shell，最终合并 argv
仍须满足原 64 项 / 16 KiB 限额。空参数保留，argv[0] 仍须非空。程序映射与固定值均由 Host
选择，插件不能替换固定项；若固定入口本身允许后续参数覆盖配置，Host 仍须按该入口的规则选择
参数。本功能不核验参数所指脚本 / profile / 依赖树的字节，也不提供 OS 沙箱。

`--process-environment NAME` 额外选择执行机器已有的环境变量，最多 27 个额外名称，
与原五个默认候选一起受 32 项上限约束；重复 / 非法名称拒绝，显式名称缺值或空值报不支持。
值不经 wire 传输，不打印到诊断；未选择的变量不继承。Portable 档须有包声明、Host 配置与本次 Session
共同允许的业务进程服务；Node / Exec 档也可将这些候选用于已明确允许的主 Executor，
不会因此开放业务进程服务。`TMPDIR` 是五个默认候选之一；没有自动搬运客户端凭据或改用身份。

例如，持有已审查 Node 后端与 profile 的可信启动器可将程序名映射到绝对 Node 可执行文件，
固定脚本、`--profile` 与 profile 路径，再显式选择后端所需的执行端环境。此为 Portable
Supervisor 调用外部 Node 进程的接缝；直接运行 `runtime: node` 包使用下面的独立运行档选择。
真实原生 HOST Session → gterm → Node → 锁版 Claude CLI 的无模型 conformance：

```sh
python3 -B scripts/test-gterm-node.py --build tmp/gterm-build --fetch
python3 -B scripts/mutation-check-process-prefix.py --sdk tmp/compiled-sdk --gterm-build tmp/gterm-build
```

第二条先在工程内隔离副本验证十一项 C 边界与十组 ASan/UBSan 变异，指定 gterm fixture build
后再验两组真实 Node 管道变异；不运行模型 / 工具，不代替 App 授权页面或 Node 包安装验收。

示例：

```sh
gterm relay --stdio --trust-development-digest PACKAGE_SHA256 --client-digest CLIENT_SHA256 \
  --root workspace ./project -- DIRECTORY COMMAND [ARG...]
```

调用方负责在 Executor 所在机器选择并授权该根；没有自动把当前目录、包目录或用户主目录加入资源范围。

实际 command/argv 不拼 shell，不把多 token 命令猜成某个包；COMMAND 参数是完整规范命令。
固定 checked 包的 stdio 调用会在 `activate()` 前按对应 `argsSchema` 的 v1
`x-ghosterm-cli` 映射解码，插件收到冻结的 `invocation.decodedArguments`，同时保留只读原始
`invocation.arguments`。缺少 CLI 映射却带有参数时拒绝激活；无参数且没有映射时
`decodedArguments` 为 undefined。Portable SDK `commandArguments()` 与宿主结果遵循相同语法，
stdio conformance 会逐字段对拍。跨包命令发现与关联之外，App 授权 UI 和安装信任仍待接。
不会自动下载、安装、连接其它执行环境或使用另一身份。

进程保持前台，收到 Host 关闭或 SIGINT/SIGTERM/SIGHUP 后释放 runtime/Session/包引用。stdio
模式退出码 0 只表示已激活实例随后被对端有序结束，**不表示业务操作成功**；业务结果
只能依据 RPC 回执。2 参数/命令/TTY 错误，3 包能力未知，4 不支持，5 引擎/校验/关闭错误，
6 来源摘要不符或 Host 拒绝，7 未建立完整实例或通道结果不明，130 取消。插件 dispose
异常也返回非零。Host 丢失、未确认的操作不自动重放。

```sh
cmake -S cli/gterm -B tmp/gterm-build -DGHOSTERM_GTERM_TESTS=ON \
  -DGHOSTERM_WITH_PLUGIN_JS=ON -DGHOSTERM_WITH_LIBSSH2=OFF
cmake --build tmp/gterm-build -j 4
python3 scripts/test-gterm-stdio.py --build tmp/gterm-build
python3 scripts/test-gterm-stdio.py --build tmp/gterm-build --case tty
python3 scripts/mutation-check-gterm.py --stdio
```

此进程测试以真实独立 child 与公共 native HOST Session 往返，不模拟 wire 成功；默认套件也用锁定 QuickJS 执行官方 Preview 包，通过真实 `file.readScoped` 和 C ResourceStore 收取并校验主文档与附件。该夹具使用本机隔离 stdio peer，不替代 App 原生授权页面、实际 SSH 启动矩阵、本机 IPC 或其它业务插件验收。

`plugin resolve` 对显式列出的开发包解析一条完整 Executor 命令：

```sh
gterm plugin resolve --host-json host.json -- 'ai codex' ./package-a DIGEST_A ./package-b DIGEST_B
# 冲突后，用户明确选定插件 ID 与当前精确摘要：
gterm plugin resolve --host-json host.json --select org.example.ai DIGEST_A -- 'ai codex' ./package-a DIGEST_A ./package-b DIGEST_B
```

每个目录后必须给其完整性清单的 SHA256；这表示本次显式信任该开发包字节，不证明发布者
身份。最多 32 个包，不扫描 cwd、环境变量或默认安装路径。所有包固定捕获、完整性及兼容
校验通过后才解析；没有按顺序优先、隐式替换或自动选择。返回选定 ID、版本、摘要、命令、
标题和参数 schema 路径；schema 尚未解码，解析不激活插件、不保存关联、不授予 Session 权限。
任何包失败或关闭失败均不输出部分选定结果。冲突退出 8，显式选择过期退出 9，命令缺失退出
10；未知能力 3、不支持 4、开发摘要不符 6，其余输入/校验/IO 沿用 2/5，取消 130。

```sh
python3 scripts/test-gterm-resolve.py --binary tmp/gterm-build/gterm
python3 scripts/mutation-check-gterm.py --resolve
```


通用 Node / Exec 开发运行档使用相同的固定包检查、命令参数解码、Host Session 批准和公开 runtime 门面。它们仍要求 JS-enabled 构建用于包校验；`doctor` 的引擎支持结果不能证明 Node 已安装或某个包可执行。必须显式选择运行档并批准本次 OS 执行，不自动探测、下载或切换后端：

```text
gterm relay --stdio --runtime node --allow-process-execution \
  --node-runtime ABSOLUTE_NODE_PATH VERSION NODE_SHA256 \
  --node-sdk HOST_SDK_DIRECTORY SDK_CATALOG_SHA256 \
  --trust-development-digest PACKAGE_SHA256 --client-digest CLIENT_SHA256 \
  --method METHOD -- DIRECTORY COMMAND [ARG...]

gterm relay --stdio --runtime exec --allow-process-execution \
  --trust-development-digest PACKAGE_SHA256 --client-digest CLIENT_SHA256 \
  --method METHOD -- DIRECTORY COMMAND [ARG...]
```

Node 的 VERSION 不带 `v`，由 Native Host 的显式配置提供；Core 会以空环境运行固定 Node 的 `--version`，必须与已检查包的版本观察精确一致。Node 路径须为规范绝对路径，Core 校验实际 SHA-256。Host SDK 不是插件自带模块；可用 `node scripts/build-plugin-node-sdk.mjs --tools 锁定开发工具目录 --out tmp/新目录` 构建，使用输出的 `catalog=` 摘要。Core 只读捕获该目录、核对独立摘要，再与固定包一起暂存；不重新读取原包入口。Exec 仅运行完整性已核验、与本机 OS / 架构匹配的包内二进制，不接受 Node 依赖参数。

`--allow-process-execution` 是本机可信启动器对主运行进程的明确选择，与包的 `process.spawn` 业务能力、命令关联和 Host Session 批准分别校验。缺少批准在握手前退出 6；缺少依赖参数 / SDK 目录或运行进程报告不支持时退出 4，配置存在但固定文件核验失败或激活失败沿用非零退出；不会运行 Portable 兜底。Node / Exec 同样支持 `--root`、`--process-program` / 固定参数、选定环境与 `ResourceBroker`；业务子进程仍受 manifest scope 和实时授权约束。主进程仅收到同一套明确选定的环境，未选变量不继承。

进程运行档要求显式绝对 `TMPDIR`，在其下建立当前用户私有 0700 暂存父目录；Core 完成实际 child reap 和资源关闭后删除。默认主进程激活期限 10 秒、总期限 60 秒、协议 stdout 16 MiB、stderr 64 KiB；业务子进程另外默认每个 60 秒、实例 stdout / stderr 合计 1 MiB、最多八个保留 child。管道每消息最多 3072 二进制字节，允许正常短读 / 短写；argv 受 8 KiB 私有消息和原参数预算共同约束。进程组信号错误与 leader 退出分开保留，相关 errno 输出为数字诊断；退出 0 仍只说明实例曾激活并由对端有序结束，不证明业务成功或后代树完整回收。这里没有 OS 沙箱或整进程 RSS / CPU 预算承诺。

验证只使用一次性本机目录与真实 Native HOST Session / Node / Exec，不连接账户、模型或用户项目：

```sh
python3 scripts/test-gterm-process.py --build tmp/gterm-build --node 已安装Node路径 --sdk 已构建HostSDK目录
python3 scripts/mutation-check-gterm-process.py --node 已安装Node路径 --sdk 已构建HostSDK目录
python3 scripts/test-plugin-process-runtime.py --node 已安装Node路径 --tools 锁定工具目录 --public --facade --mutate
```

Swift / JNI 已同步公共配置布局和进程事实观察。App 的远端运行档配置见下节；本机 IPC 激活与通用运行档的完整产品矩阵仍待完成，默认 App 插件开关保持原状态。运行档构件和 Host SDK 不加入 App 打包。

### 离线签名执行端安装（实验）

CLI 另外显式启用 `-DGHOSTERM_WITH_RELEASE_SIGNATURE=ON`，链接已安装的 OpenSSL Crypto
和 zlib，并保持 `GHOSTERM_WITH_PLUGIN_JS=ON`。签名构建把可信渠道策略校验器编入 CLI，
不依赖 Node / Python 做安装验证；这两个运行时只在所选插件或开发测试需要时使用。
未启用签名的构建在接触目录 / 安装根之前返回 `RELEASE_VERIFY_UNAVAILABLE`（退出 4）。
本批使用当前 macOS 主机的 OpenSSL；该本机依赖声明下限为 macOS 26，不能据此宣称已产出
兼容 macOS 13 的 CLI 分发物。普通 App 构建和随包组件不变。

用户选择独立可信的渠道公钥策略，以及发布方提供的 ZIP / 原始描述 / 64 字节签名。
目录登记根须已存在且属于当前用户、0700；安装根为另一个专用规范绝对目录，最多
3500 字节，可创建最后一层，不递归创建祖先。命令不会联网、下载依赖或运行安装脚本。

```text
gterm plugin install --catalog ABSOLUTE_CATALOG --expect-revision 0 \
  --store ABSOLUTE_INSTALL_ROOT --artifact RELEASE_ZIP --descriptor RELEASE_JSON \
  --signature RELEASE_SIG --policy ABSOLUTE_TRUSTED_POLICY --host-json HOST_JSON \
  -- --client-digest CLIENT_SHA256 --method plugin.method
```

先按同一候选 release / channel 契约严格检查 JSON、重复解码键、精确整数、NFC、
公钥 ID / canonical Base64、渠道 / 发布者 / 插件范围和撤销，再用共享 Core 验证
`GHOSTERM-PLUGIN-RELEASE-V1` + NUL + 原始描述的 Ed25519 签名。实际 ZIP 字节数 /
摘要须匹配签名声明。只接 ZIP32 stored / raw DEFLATE：无 extras、描述符、注释、
拼接 / 尾部、链接或特殊文件，核对中央 / 本地目录、连续布局、CRC 和解压限额。
最多 1024 文件 / 4096 总条目、单文件 16 MiB / 总 64 MiB、32 层路径；实际归档最多 72 MiB。
真实 Core 再核对完整 payload 及 ID / version / publisher / package D，检查兼容但不激活模块。

安装根使用私有单链接锁。先写并同步意图，再创建本次独占暂存；文件同步并冻结为
0400（可执行成员 0500）、目录 0500，一次不覆盖 rename 为 `INSTALL_ROOT/描述SHA256/`。
该版本内含 `payload/`、`release.json`、`release.sig`，不保存第二份 ZIP 或私钥。
已有版本冲突不覆盖；正常失败只清本次身份仍匹配的暂存。SIGKILL / 身份或同步未知的
残留保留，不自动恢复、收养或删除；已有版本 / 暂存 / 意图条目达到 64 后拒绝新安装，
完整崩溃和显式版本维护仍待验收。

成功为 `installed-and-registered`；结果分别给 `installed`、目录 `committed` 和
`durabilityConfirmed`。发布后登记失败会明确保留 `installed=true`，返回非零，不删除
已发布版本；同步不明也返回非零。此时先观察，正确配置后可只重新登记：

```text
gterm plugin register-signed --catalog ABSOLUTE_CATALOG --expect-revision CURRENT \
  --policy ABSOLUTE_TRUSTED_POLICY --host-json HOST_JSON \
  -- ABSOLUTE_RELEASE_DIRECTORY [stdio profile options...]
```

替换已有 ID 仍须 `--expect-digest OLD_PACKAGE_SHA256` 和当前修订；只改变后续选择，
不删除旧版本、不迁移正在运行的实例。列表中的 `signed-release-record` 仅说明记录来源，
`publisherAuthenticated=false`，不把列表当成当前认证。每次普通命令重新核对签名、
实际包和固定策略 SHA；等待 IPC 后、进入 Session 及激活前继续核对。策略文件变化
即拒绝旧记录，须显式重新登记；这是本机快照检查，没有在线策略新鲜度 / 防回滚保证。
签名不批准 Client、业务程序、工作区或本次 Host Session。

三运行档均可登记和签名安装；Node / Exec 继续明确配置 `--runtime` 和
`--allow-process-execution`、固定 Node / Host SDK 等依赖。同平台有多个候选 variant 时，
HOST_JSON 用 `variantId`、启动配置用 `--variant ID` 明确选择；不会按运行时可用性猜选。
登记与分派会核对配置运行档 / variant 和 Core 已选元数据；不匹配拒绝成为可选命令。

```sh
python3 -B scripts/test-gterm-release.py --build tmp/gterm-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
```

本批为 macOS / arm64 本机 C HOST 专项：33 场景，含三档、Node / Exec 开发目录、
多 variant、拒绝及 attacher 中断。attacher 的本地取消 130 不证明原前台取消；后者
保留断流结果不明 7。不是 App 点击、Linux 或完整 IO / 取消 / 崩溃 / 发行证据。



### 安装版本清单与显式清理

签名构建可观察用户明确指定的私有安装根与登记目录：

```text
gterm plugin versions --store ABSOLUTE_INSTALL_ROOT --catalog ABSOLUTE_CATALOG
gterm plugin review-installed --store ABSOLUTE_INSTALL_ROOT --catalog ABSOLUTE_CATALOG \
  --version RELEASE_DESCRIPTOR_SHA256
```

`versions` 列出磁盘条目、版本目录名、已观察引用与所选目录的登记引用数；未知条目
只列举、不收养。这里的 version 是安装槽的原始发布描述 SHA256，不是包版本号。
列表不认证发布者，目录名也不证明安装成功。`review-installed` 给出当前完整排序清单
（路径、类型、文件长度 / SHA256）、逻辑文件总字节、引用状态和精确 `reviewDigest`。
根目录不入清单。最多 4096 项、深度 34、单路径 4096 字节、路径总量 16 MiB、
单文件 16 MiB、文件总量 64 MiB＋64 KiB＋64 字节。该数字不是实际磁盘释放量。

初次清理要求同文件系统、当前用户的只读安装目录、固定的三项布局和匹配目录名的
release.json；文件为普通单链接、0400 / 0500，目录为 0500。不跟链接、不跨文件系统，
不 chmod 普通文件；这份目录检查不认证当前签名，不授予运行权限。
先用 unregister / disable-denied 取消所选目录的登记，再查看完整清单并明确执行：

```text
gterm plugin remove-installed --store ABSOLUTE_INSTALL_ROOT --catalog ABSOLUTE_CATALOG \
  --version RELEASE_DESCRIPTOR_SHA256 --expect-revision REVIEW_REVISION --review-digest REVIEW_SHA256
```

确认绑定目录身份、所选登记目录身份 / 修订、全部当前路径与文件字节、权限 / 时间、
删除意图及引用观察。提交时重新核对，不允许用旧确认删除变化后的内容。仍被所选目录
引用退出 8，持有引用退出 11，过期确认退出 9。该工具只检查所选登记目录，不枚举其它
目录：`otherCatalogsChecked=false`。其它目录可能仍留下失效登记，后续启动会明确失败。

新的 CLI 在普通命令选择释放目录锁前取得版本引用，待附着前台 / stdio 到 Runtime
释放期间保持；引用是目录 fd 的协作锁，不猜 PID。短暂的安装根锁关闭引用与隔离的
竞态，运行期间不占用整个安装根，故可安装 / 选择新版。它保护使用本版本协议的 CLI
owner，不是 OS 沙箱，也不追踪旧二进制、手工进程或任意配置路径的间接依赖。

删除先同步绑定安装根与版本 inode 的私有 `.remove-SHA.intent`，再不覆盖地将原目录
改名为 `.remove-SHA` 隔离。随后仅处理本次复核的对象，逐项重新核对、同步并删除；
最后删除空目录与意图。`removed`、`quarantined`、`intentRetained`、`durabilityConfirmed`
分别报告，失败不冒充回滚。原槽有待处理删除时，新的启动和同描述重新安装明确拒绝。

中断后的原目录、隔离目录或“目录已无、意图尚在”均须再次 review-installed，再使用
新的确认。复核展示全部当前剩余内容；新增内容会使旧确认失效，显式新确认才授权
该次完整清单。不会启动时自动继续删除。完整磁盘故障、挂载、并发和其它 OS 矩阵
仍待集中验收。

安装中断留下的条目须另外完整核对，再明确删除单个条目：

```text
gterm plugin review-residue --store ABSOLUTE_INSTALL_ROOT --catalog ABSOLUTE_CATALOG \
  --name ENTRY_NAME
gterm plugin remove-residue --store ABSOLUTE_INSTALL_ROOT --catalog ABSOLUTE_CATALOG \
  --name ENTRY_NAME --expect-revision REVIEW_REVISION --review-digest REVIEW_SHA256
```

只接受规范的小写十六进制 `.prepare-32HEX` 暂存目录、`.intent-32HEX` 安装意图、
`.remove-64HEX` 孤立隔离目录和 `.remove-64HEX.intent` 删除意图；其它名称保留。
名字不证明归属，始终报告 `ownershipKnown=false`。核对展示全部当前路径 / 文件摘要，
根文件另给 `rootSHA256`，确认绑定所选条目、登记修订、身份 / 权限 / 时间和所有字节。
沿用上述预算，不跟链接、不跨文件系统，不接受多链接普通文件，不修改普通文件权限。
暂存目录与文件额外允许私有可写权限 0700 / 0600。登记引用或实际协作锁阻止删除。

安装意图须等对应暂存目录不存在才可删除；孤立隔离目录须等原版本槽不存在。
删除意图须等隔离目录不存在。若原版本还在，核对同时展示它的完整清单并检查引用 / 锁；
`preservedVersion` 指出保留版本，提交只删意图，不改版本内容。关联版本发生变化同样使
旧确认失效。`fileBytes` 包含全部核对文件，`targetFileBytes` 只计所选删除条目的文件；
两者都不是实际可释放磁盘空间。残留维护不推断有效删除协议，`pendingRemoval`、
`versionAbsent`、`intentRetained` 为 null；伴随条目保留并须分别核对。

维护不重命名或自动重放残留任务。取消 / 进程死亡可能留下部分已删、部分仍在的目录，
旧确认随清单变化失效；再次完整核对后才能继续。不会将未知条目收养为成功安装，
不认证发布者或授予执行能力，不操作用户未指定的安装根 / 登记目录。

定向场景使用工程内签名夹具，不操作用户安装根、账号或外部主机：

```sh
python3 -B scripts/test-gterm-release.py --build tmp/gterm-build --node 已安装Node路径 \
  --sdk 已构建HostSDK目录 --case version-maintenance --case version-reference \
  --case version-active --case node-version-active --case exec-version-active
python3 -B scripts/test-gterm-release.py --build tmp/gterm-build --node 已安装Node路径 \
  --case residue-maintenance --case residue-reference --case residue-damaged-intent
# 仅显式夹具构建提供删除一次后的暂停；真实 SIGKILL 后重新复核继续。
cmake -S cli/gterm -B tmp/gterm-build -DGHOSTERM_GTERM_TESTS=ON -DGHOSTERM_GTERM_STORE_FAULTS=ON
cmake --build tmp/gterm-build --target gterm
python3 -B scripts/test-gterm-release.py --build tmp/gterm-build --node 已安装Node路径 --case version-resume
# 收尾恢复普通构建；普通二进制不识别测试暂停环境。
cmake -S cli/gterm -B tmp/gterm-build -DGHOSTERM_GTERM_STORE_FAULTS=OFF
cmake --build tmp/gterm-build --target gterm
```

### 执行端当前策略复核与显式批量禁用

签名构建可只读复核一个已登记目录与明确选定的独立策略：

```text
gterm plugin review-policy --catalog ABSOLUTE_CATALOG --policy ABSOLUTE_TRUSTED_POLICY
```

输出每条记录的状态、是否可禁用、旧策略绑定是否仍匹配，以及 `revision`、
`policyDigest` 和本次完整 `reviewDigest`。只复核记录中策略路径与选定路径完全相同的
签名来源；其它策略和开发来源分别保留。`allowed` 表示当前策略签名及实际 Core 包
检查通过，不授予运行权限；`policyBindingCurrent=false` 的允许项仍须显式重新登记，
不会自动采用新策略。

`denied` 才是当前策略明确拒绝，包含撤销、范围或签名验证失败。材料打不开、无效
策略 / 描述、包检查失败和记录不匹配独立显示，不当作明确拒绝。查看完整结果后，
带上这三项精确确认值执行：

```text
gterm plugin disable-denied --catalog ABSOLUTE_CATALOG --policy ABSOLUTE_TRUSTED_POLICY \
  --expect-revision REVIEW_REVISION --expect-policy-digest REVIEW_POLICY_SHA256 \
  --review-digest REVIEW_SHA256
```

确认摘要绑定目录身份 / 修订、所有登记字段、当前策略以及实际解析的描述 / 签名。
提交前在目录排他锁内重新复核，变化退出 9，不套用旧确认。仅将明确拒绝的登记从目录
移除，一次原子事务；其它记录原样保留。没有拒绝时返回 `unchanged`，不递增修订，
`committed=false` / `durabilityConfirmed=false` 表示没有新提交。提交后同步失败会保留
`committed=true` / `durabilityConfirmed=false` 并返回非零，不伪称回滚。

此操作禁止这些登记的后续普通命令选择，不删除安装版本、凭据或包文件，也不结束
正在运行的实例。恢复需用户明确选择可验证版本重新登记。不获取联网策略，不承诺
策略新鲜度 / 全局撤销传播、跨进程即时停止或版本 GC。复核摘要不是秘密或运行许可。

定向的工程内隔离复核 / 混合来源事务和普通调用冒烟：

```sh
python3 -B scripts/test-gterm-release.py --build tmp/gterm-build \
  --node 已安装Node路径 --case policy-review --case roundtrip
```

### App 中选择远端 Executor 运行档

实验 JS-enabled 的 macOS / Android 插件页面在来源审查批准后，提供显式的
「Executor 运行档」选择。Client 仍运行于内嵌 Portable JS；此选择只决定 SSH 另一端的
Executor，默认保持 Portable JS。

Node 需要填写远端已安装 Node 的规范绝对路径、不带 `v` 的版本、可执行文件 SHA-256，
以及预先准备好的 Host SDK 目录和独立 catalog SHA-256。Exec 使用已检查包内匹配
远端系统 / 架构的入口。两档均需填写远端已存在的绝对临时目录，并勾选本次进程执行允许；
运行档、依赖、主机或其它启动配置改变后需重新允许。目录通过 POSIX 引号构造的
`TMPDIR=... exec ...` 传给远端 gterm，不使用客户端的临时目录或凭据值。

之后仍有当前 Session 的独立能力确认；页面显示捕获的运行档、依赖与临时目录。
主运行档允许不授予业务 `process.spawn` / `process.pipes`，业务进程仍需要独立的
workspace root、Host 映射、包声明与 Session 授权。缺失依赖 / 摘要或版本不匹配会失败，
不自动安装、不改选其它运行档。远端规范路径、实际文件身份与版本由 gterm/Core 核验，
客户端的路径格式检查不等同于远端检查成功。完整启动命令仍受 SSH 路由的 1024 字节上限约束。

配置入口验证（无账号 / 模型，Android 使用工程内专用无窗口模拟器）：

```sh
python3 scripts/test-plugin-execution-app.py --macos --mutate
python3 scripts/test-plugin-execution-app.py --headless-api 36 --mutate
```

这些检查覆盖配置、授权撤回、依赖参数、原生表单及字面 shell 传递；它们不替代
通用 Node / Exec 包从两端 App 经真实 SSH / tmux 激活、业务运行和停止的产品矩阵。


### 两端 App 的真实 SSH 运行档验证

准备显式 JS / SSH-enabled 的 CLI 测试构件（同一静态 Core 也供隔离 macOS Host 使用），
以及上文的已构建 Node Host SDK；Node 必须是已安装的可执行文件，不自动安装依赖：

```sh
cmake -S cli/gterm -B tmp/execution-build -DGHOSTERM_WITH_PLUGIN_JS=ON \
  -DGHOSTERM_WITH_LIBSSH2=ON -DGHOSTERM_GTERM_TESTS=ON
cmake --build tmp/execution-build --target gterm test_gterm_exec_peer -j 2
python3 scripts/test-plugin-execution-ssh-app.py --macos --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
python3 scripts/test-plugin-execution-ssh-app.py --headless-api 36 --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
python3 scripts/mutation-check-plugin-execution-stop.py --headless-api 36 --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
python3 scripts/mutation-check-plugin-execution-pump.py --macos --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
python3 scripts/mutation-check-plugin-execution-pump.py --headless-api 36 --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
python3 scripts/mutation-check-plugin-execution-pump.py --macos --business-environment --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
python3 scripts/mutation-check-plugin-execution-pump.py --headless-api 36 --business-environment --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
# 每端分别用 --client-workspace / --client-directory / --client-instance 执行三组权限保护
python3 scripts/mutation-check-plugin-execution-pump.py --macos --client-workspace --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
python3 scripts/mutation-check-plugin-execution-pump.py --headless-api 36 --client-workspace --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
```

每端一百一十六个场景：Node / 原生 Exec × 直连 / 两跳 ProxyJump × 原有九种生命周期 / 资源情形、
九种业务进程情形，以及十一种 Native 本机资源发布情形。macOS 走生产 AppModel，Android 走生产 ViewModel 和原生
授权页；SSH 密钥只由服务在内存中交付。断流通过本轮拥有的透明 TCP 故障夹具切断
加密连接，不启动生产网关。模块 / 原生入口分别在第一条包代码处打点，未批准时不能进入；
正常 RPC 按实际收到的 JSON 内容回传，终止前等待处理器打点；取消先核对 cancelRequested，再停止 Client Runtime，结果必须 UNKNOWN 且未确认。
停止后检查本轮命令对应的远端 PID 消失及暂存目录清空，单独保存 gterm 的实际 wait 状态。

`--case node:jump:resource` 可单选远端已授权工作区文件的 256 KiB 二进制发布，
两端实际收件列表检查完整字节、SHA-256、大小 / MIME / 文件名，并独立检查远端发布确认。
`resource-root / resource-parent / resource-link` 分别验证未授权根目录、父目录越界、符号链接：
资源服务须给出明确错误、不产生收件或成功回执。Node / Exec 仍是显式允许的完整 OS
进程；这里验证 Ghosterm 资源服务的范围约束。worker 每轮最多 32 个原生 step，并在调用间
检查五毫秒墙钟预算；单次原生调用继续受原 CPU 保护，不按 UI 的 50 ms 周期逐帧限速。
独立 pump 变异恢复旧的单帧调用，保持十秒发布预算，要求普通单条 Native 断言红、同构件
配置绿、复原绿与源码 SHA256；另核对实际编译对象 / APK 的改变。

`--case node:jump:process` 经 App RPC 启动远端明确映射、SHA-256 固定的 `/bin/cat`，
在远端原生管道内双向验证 256 KiB，向 App 返回实测摘要 / 字节数；这部分数据不充当
SSH 上的资源流量。`process-root / process-program / process-cancel / process-stderr /
process-environment / process-cwd / process-stop / process-loss` 另护未授权工作区与程序拒绝、
业务取消、stderr 分离、完整选择环境、工作目录，以及子进程存在时的 Runtime 停止 / 物理断流。
每个夹具只配置一个名为 cat 的映射；环境 / cwd 场景分别将它明确映射到系统 env / pwd，
版本标签为 Host 的 fixture 声明，实际系统二进制另以 SHA-256 固定，不声称运行了版本查询。
正常场景核对 EOF / 实际退出 / release 后引用失效；取消核对 SIGTERM / 回收。停止 / 断流
先按唯一 argv 标记与进程启动身份观察实际业务 PID 存活，再检查其消失及未确认 UNKNOWN，
失败清理也只处理本轮已观察且身份仍匹配的进程。

`--business-environment` 变异在隔离宿主源码中漏传显式选择的 GT_SELECTED，保持其它启动
配置不变；实际 env 进程的测量结果必须触发单条普通 Native 断言红，同构件配置绿、复原绿
及源码 SHA256。它验证环境名称从 App 传到实际业务子进程的完整链，不把单纯 argv 字符串
相等当作效果证据。

`--case node:jump:client-resource` 另验证 Native Host → Executor → App 的双向资源：
本机明确允许的普通目录由 Native worker 在 Session consent 后打开，Native 发布按钮才读取
相对文件。Client manifest 仍禁止 file.*；Client JS 不得到目录、fd 或可用文件 root。
本机路径单独捕获，授权页展示，不加入远端启动命令。单文件 256 KiB、发布十秒，
Native Source 和 Runtime 由同一 worker 推进；成功必须收到实际对端确认，不能把 EOF 或进程
退出当作成功。Node / Exec 对本机上传的另一种二进制模式逐字节、完整 EOF / 摘要核对并释放
接收引用，再发布远端文件回 App，核对两方向不同的 SHA-256。Android 正向场景点击生产发布按钮，
macOS 通过按钮背后的 AppModel 动作；不把后者称完整 UI 点击。

`client-root / client-parent / client-link` 检查错误根、目录越界与文件链接拒绝；
`client-no-approval / client-undeclared` 必须在 SSH 前拒绝未允许的目录或缺少 Client resource.receive；
`client-directory-link / client-directory-file` 验证选中的本机目录入口不跟链接且必须为目录。
`client-stop / client-loss / client-cancel` 先观察真实部分发送，再停止 Runtime、断开 TCP 或显式取消。
停止或断流不能报告已发布，未完成 Source 可保留到显式释放以驱动 Session close，之后全部 owner 与
远端暂存必须释放；正常取消仍推进 Session 至本机 Source 释放。
每次发布带当前 Runtime UUID，旧实例动作不能向后来实例发布。三组独立 Native 变异分别去掉
显式允许、目录 O_NOFOLLOW 和 Runtime UUID 保护，要求真实编译后的单条普通断言红、
同构件配置绿、恢复与 SHA256。收据冻结两端 Native bindings / JNI 与测试输入，并保存实际 XCTest / APK 摘要。

可选普通终端 / tmux 共存矩阵（仍为独立插件 exec，未实现 TTY carrier）：

```sh
python3 scripts/test-plugin-execution-ssh-app.py --macos --tmux target --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
python3 scripts/test-plugin-execution-ssh-app.py --macos --tmux nested --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
# Android 将 --macos 换成 --headless-api 36；以下两类各跑 target / nested
python3 scripts/test-plugin-execution-ssh-app.py --macos --tmux nested --stop-tmux --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
```

每端 target / nested 各三十六个共存场景，--stop-tmux 各十二个，共九十六个。
旁路普通终端是自有 SSH PTY，target 为目标 tmux，nested 为 SSH A → tmux A → SSH B → tmux B；
第二个真实 SSH 客户端按键切 pane / window、detach / attach，外层另切 pane。惰性 peer 只记字节，
不会执行收到的输入；完整输入必须恰好为各步骤的固定字符，插件协议不能混入。Native App 在切换后
分别确认两次 RPC，再验业务管道 / 双向资源 / 中断，插件三标准流须实际无 TTY。
普通终端存活项要求插件停止后仍可输入；--stop-tmux 则先明确结束本轮普通终端和 tmux，
之后插件仍确认新 RPC 并完成普通请求 / 双向资源 / 业务进程。只关闭显式 socket 的 tmux，
检查其服务器、pane 和 SSH 客户端进程退出。跳板 fixture 仅 nested 档另允许普通终端 exec；
默认跳板仍 MaxSessions=0。未选择 --tmux 时保留原一百一十六个场景。

两端走生产 AppModel / ViewModel，但普通终端为旁路 SSH PTY 夹具；本项不是 App TerminalSurface
绘制 / IME / 焦点；不带 `--attach` 的上述矩阵也不证明 pane 内 gterm 的 IPC 附着。

显式前台附着可另与两种 tmux 拓扑组合：

```sh
python3 scripts/test-plugin-execution-ssh-app.py --macos --attach --tmux target --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
python3 scripts/test-plugin-execution-ssh-app.py --macos --attach --tmux nested --kill-front-pane --build tmp/execution-build \
  --node 已安装Node路径 --sdk 已构建HostSDK目录
```

每端分别跑 target / nested，Android 将 `--macos` 换成 `--headless-api 36`。普通附着各 48 项，
`--kill-front-pane` 各 12 项：前台 gterm 确实位于本轮 tmux 的独立窗口，App 经已认证目标的
独立 SSH 通道连接精确实例。前台启动时三个描述符为 TTY，实际 Node / Exec 插件描述符均非 TTY。
第二客户端按键选择前台窗口后，惰性包装器只记录固定输入，切回后再确认 RPC；插件结束后
包装器继续收取固定字符，核对其与其它 pane 的完整输入，绝不将输入交给 shell 执行。
故障档关闭该 pane，分别打断待确认 RPC、活动业务进程和文件部分发送，要求 UNKNOWN、
已观察进程身份消失和全部暂存释放，保留未观察到的实际退出码为 unknown。
gterm 的 HUP 处理走已有取消及有界释放，修复 pane 关闭直接终止后遗留运行副本的问题；
不增加宽限、不补发业务请求、不关闭其它窗口或用户 tmux。SIGKILL 仍无法保证执行清理。
这是自有 SSH PTY 的 tmux 档，不是 App TerminalSurface / 焦点 / IME、会话恢复或通用 TTY 回写证据。

这是无账号 / 模型的 conformance 包，并非官方业务插件。当前观察到成功回包后的 App
停止和连接断流使远端 gterm 返回 7（会话结果不明），拒绝返回 6；不把进程消失或一条
RPC 成功记成整个远端任务成功。这里只证明明确列出的资源方向和场景；SAF 来源发布、
App TerminalSurface 与 tmux 的完整交互、其它服务器 / 操作系统、完整资源 / 进程预算与异常矩阵或真实设备仍待验证；不纳入现有
macOS CI 二十份变异。

### Client 提供的标准原生视图服务

`relay --stdio --client-capability ui.table` 要求 Native Client 提供该服务。
支持 `ui.table`、`ui.diff` 与 `ui.task`，每种至多声明一次；未知名称和重复项拒绝，Executor 包必须
声明 `session.rpc`。这不是 gterm 本机
渲染能力或执行批准：该项加入共享 Session 的精确协商集合，Host 仍须实际提供能力并
显式 consent。缺少参数时不会猜测 Client 服务，旧 Host / 集合不匹配时拒绝握手。
包兼容性检查的 Executor profile 不因此声称具有这些 UI 服务；其它未接服务仍显式不支持。
已有 `--listen` 实例的配置在附着时不更改，所需服务须在其创建时声明。

macOS 实验插件页按准确批准命令的 `views: ["table"]` / `views: ["diff"]` 加入对应
标准分页方法及上述要求。同一官方 Executor 目录可用于本机进程通道或显式 SSH 路线。
Android 同样按命令声明接原生 table / diff，缺服务时明确不支持。普通构建仍关闭 JS；
插件安装、终端 TTY 回写、diff IPC 与正式分发不在本切片结论内。
