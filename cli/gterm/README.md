# gterm 前台 CLI

独立构件，不加入 Ghosterm App 包、不安装远端 helper、不修改 shell 配置。
当前接入诊断、固定包兼容性检查及显式 Portable / Node / Exec stdio Executor；普通命令分发、本机 IPC、安装/升级仍未实现。
它不内置业务命令，未接分发的顶层命令返回 `COMMAND_ACTIVATION_UNAVAILABLE`。

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
关闭诊断写 stderr。SIGINT/SIGTERM 只设置取消标记，采集循环在下一步停止；同步元数据
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
Host 必须管理专属 stdio 的对端归属；本入口不实现本机 socket 认证、发现或后台服务。

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

进程保持前台，收到 Host 关闭或 SIGINT/SIGTERM 后释放 runtime/Session/包引用。stdio
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

Swift / JNI 已同步公共配置布局和进程事实观察，但 App 的依赖选择、启动参数和 Node / Exec 产品入口仍未接；默认 App 插件开关保持原状态。运行档构件和 Host SDK 不加入 App 打包。

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
