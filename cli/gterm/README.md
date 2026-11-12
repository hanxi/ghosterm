# gterm 前台 CLI

独立构件，不加入 Ghosterm App 包、不安装远端 helper、不修改 shell 配置。
当前接入诊断、固定包兼容性检查及显式 stdio Executor；普通命令分发、本机 IPC、安装/升级仍未实现。
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
gterm relay --stdio --trust-development-digest PACKAGE_SHA256 --client-digest CLIENT_SHA256 [--method NAME]... [--host-method NAME]... [--root ID DIRECTORY]... [--process-program NAME ABSOLUTE_PATH VERSION SHA256]... -- DIRECTORY COMMAND [ARG...]
```

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

Native 启动器可为 Executor 明确配置 `--process-program NAME ABSOLUTE_PATH VERSION SHA256`；该映射来自本次用户/Host 设置，不由插件包选择。只有同时存在至少一个显式 workspace root 和映射时，兼容性事实及 Session 才开放 `process.spawn` / `process.pipes`。Core 在 Runtime open 时核对绝对可执行文件 SHA-256，并在每次 spawn 前复核文件身份；程序名还必须出现在已审查包的 `process.spawn` manifest scope。`VERSION` 是 Native Host 提供的版本声明，gterm/Core 不会自行运行 `--version` 探针，Codex adapter 会将它与锁定版本比较。子进程只收到 gterm 选择的 `HOME`、`PATH`、`XDG_CONFIG_HOME`、`CODEX_HOME`（若存在），不会继承其余 SSH/gterm 环境。Host mapping 当前是显式命令参数；不要把摘要/版本当成来自插件自身的声明。

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
