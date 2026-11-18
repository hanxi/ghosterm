# 开发与验证插件

`scripts/gterm-plugin` 提供候选 `init/check/build/pack/test` 命令，当前构建对象为
Portable Client 加 Portable / Node / Exec Executor 运行档。命令读取固定副本，不执行项目的 npm
脚本、安装钩子或插件入口；只有 `test` 会在真实 Core / QuickJS 中激活选定的一侧。
默认测试 Executor，`--side client` 测试 Client，`--side paired` 同时激活两侧。
`test` 默认只激活 Portable；显式执行配置可用于单侧 Node / Exec Executor，或
Portable Client 与 Node / Exec 的双侧测试。可显式配置只读资源根、业务进程服务和 Client 视图注册服务；具体授权与验收范围见下文。

在仓库根目录准备锁定的开发工具。Node 是开发依赖，不进入 Portable SDK 或 C Core：

```sh
export TMPDIR="$PWD/tmp"
mkdir -p tmp/plugin-dev/tools
cp scripts/fixtures/plugin-dev-tools/package.json scripts/fixtures/plugin-dev-tools/package-lock.json tmp/plugin-dev/tools/
npm ci --prefix tmp/plugin-dev/tools --ignore-scripts --no-audit --no-fund --cache tmp/plugin-dev/npm-cache
python3 scripts/gterm-plugin init tmp/plugin-dev/hello --id org.example.hello --command hello --publisher Example
python3 scripts/gterm-plugin check --project tmp/plugin-dev/hello --tools tmp/plugin-dev/tools
```

没有 Host 事实时，最后一条返回 JSON `compatibility:"unknown"` 和退出码 **3**；这表示
结构检查完成，尚不能判断兼容。`--host-json` 可提供明确事实，例如：

```json
{"side":"executor","sdkVersion":"1.0.0","protocols":[1],"runtimes":["portable-js"],"capabilities":["session.rpc"]}
```

保存到 `tmp/plugin-dev/host.json` 后，加上 `--host-json tmp/plugin-dev/host.json`。
兼容返回 0，未知返回 3，不支持返回 4，拒绝返回 6；无效输入 / 工具失败返回 1 或 2。
兼容不会授予权限，也不代表发布者已经认证。依赖版本来自提供的 Host 事实，不会
自动启动程序、下载依赖或把缺失当成未知。可重复传入 `--registry-package 目录`
检查与这些目录 manifest 声明的命令前缀冲突；这不是读取应用安装注册表。

工程使用 `plugin.project.json` 显式列出 TypeScript 根文件和静态资源。构建强制
strict / noImplicitAny、禁止显式 any 和跳过类型检查的注释；可用 ES2022。Portable
不能引用 DOM / Node 全局。导入限于包内相对静态闭包及 `@ghosterm/plugin-sdk`，不支持动态
导入、require 或循环。相对运行时导入写 `.js`，构建时解析对应 `.ts`。实际 SDK 值
导出也计入每端 64 模块 / 1 MiB 限额，不把只有类型的导出计作运行时模块。

```sh
python3 scripts/gterm-plugin build --project tmp/plugin-dev/hello --tools tmp/plugin-dev/tools --out tmp/plugin-dev/hello-package
python3 scripts/gterm-plugin check --package tmp/plugin-dev/hello-package --tools tmp/plugin-dev/tools --host-json tmp/plugin-dev/host.json
python3 scripts/gterm-plugin pack --package tmp/plugin-dev/hello-package --tools tmp/plugin-dev/tools --out tmp/plugin-dev/hello.zip
```

输出必须是新路径，并在源目录之外。目录发布使用排他原子 rename，ZIP 用排他 link；
跨文件系统发布会失败，不逐个复制到公开目录。包必须含 README 与 LICENSES，
`integrity.json` 覆盖成员实际字节。ZIP 使用排序路径、固定时间和权限，无签名；同一
输入和工具链的工程直接打包与已构建目录打包可逐字节对拍。`build/pack` 未提供
Host 时仍可产生结构有效的产物，报告兼容未知，不安装到应用。

Node Executor 仍配 Portable Client。在 manifest 中把执行档改为例如：

```json
{"id":"node","runtime":"node","entry":"executor/index.js","runtimeVersionRange":"^22.0.0","requires":["session.rpc"]}
```

在 `plugin.project.json` 的 `sources` 保留 `client/index.ts`、`executor/index.ts`，并把
含 `{"type":"module"}` 的 `package.json` 加入 `assets`。Node TypeScript 可使用
`node:` 内建模块、`process`、`Buffer`；开发工具锁定 `@types/node` 22.19.7，实际
运行版本仍按 manifest / Host 事实判断，类型通过不证明运行版本兼容。每个运行档
分别编译；Portable Client 不会因为同包有 Node 运行档就获得 Node 全局或类型。
共享模块须在各自环境通过检查，不能靠共享代码绕过 Portable 限制。
不接受三斜线 `reference types/lib/path` 指令；额外类型、DOM 库和快照外文件不能
通过这些指令进入编译。这是构建闭包检查，不是 Node 进程的 OS 沙箱。

Node 的外部 npm 包不能作为裸导入交给此工具自动安装。可先在作者自己的构建链中
整理成包内相对 ESM 闭包，再把 `.mjs` 入口及依赖逐项放入 `assets`；这种已编译
资源只做静态闭包 / 完整性检查，不宣称重新验证了原 TypeScript。构建命令不执行
这些模块、`package.json` scripts 或安装钩子。`@ghosterm/plugin-sdk` 仍由可信宿主
提供，工具不把 Node 类型定义复制进插件或 Portable SDK。

Exec 运行档使用 `executable`，例如：

```json
{"id":"native","runtime":"exec","executable":"bin/tool","os":["macos"],"arch":["aarch64"],"requires":["session.rpc"]}
```

把 `bin/tool` 列入 `assets`，从 `sources` 移除不再使用的 Executor TypeScript 根。
构建要求文件非空且受完整性索引覆盖，原始字节随目录 / ZIP 保存；这不证明机器码
或协议兼容，也不运行二进制。运行时仍须有平台匹配、明确授权和执行配置。

同包多个运行档需在 Host 事实中显式给 `variantId`；工具不根据 Node 是否安装
悄悄换成另一个运行档。Node 的 `programs.node` 事实须明确 available / missing /
denied 及版本；没有事实为 unknown，不会执行 `node --version` 或安装 Node。
未提供执行配置时，兼容的 Node / Exec Executor 仍明确返回退出码 4 /
`TEST_RUNTIME_UNSUPPORTED`；也可以用 `--side client` 测试其 Portable Client。

Node / Exec 的实际执行使用 `--execution-json 文件 --allow-process-execution`，
且必须显式提供 `--host-json` 的事实。执行授权只限这一次开发测试。
Node 配置包含以下字段（路径和摘要须替换为自己独立核对的值）：

```json
{
  "runtime": "node",
  "nodeExecutable": "/absolute/physical/path/to/node",
  "nodeSHA256": "<64位SHA256>",
  "nodeSDKDirectory": "/absolute/path/to/trusted-node-sdk",
  "nodeSDKCatalogSHA256": "<可信SDK目录的catalog摘要>"
}
```

可信 SDK 可用锁定的 `scripts/fixtures/plugin-sdk-tools` 和
`scripts/build-plugin-node-sdk.mjs --tools 工具目录 --out 工程tmp下的新目录` 构建，
输出中的 `catalog=` 是目录摘要；它与插件自身的 integrity 摘要不同。Node 路径
必须是实际文件的绝对路径；Core 校验可执行字节、SDK 捕获快照，并实际核对 Node
版本与 Host 事实。SDK / Node 不会由测试命令自动安装或从 PATH 猜测。

Exec 的配置只有 `{"runtime":"exec"}`；选定的二进制仍来自已检查包的
`executable`，平台事实及摘要保持原校验。两种配置都不能添加环境或其它未知字段。
本片只向进程传内部私有暂存目录的 `TMPDIR`，不传 HOME、PATH 或 NODE_OPTIONS。

```sh
python3 scripts/gterm-plugin test --project tmp/plugin-dev/hello --tools tmp/plugin-dev/tools --contract tmp/plugin-dev/hello/plugin.test.json --core-build tmp/plugin-dev/core --host-json tmp/plugin-dev/executor-host.json --execution-json tmp/plugin-dev/execution.json --allow-process-execution
```

执行仍走公开 Core Runtime 门面、本机 socketpair 和 Native 对端；没有假业务回包。
记录真实激活、成功 / unknown、发送前取消 / 撤权、子进程退出、暂存目录为空及 fd
平衡。外部模式拒绝嵌入式 heap / CPU 预算，不能据此宣称限制了 Node / Exec 的
RSS；Core 的既有时间 / 输出限额仍生效。这不是同用户 OS 沙箱，不授予文件或
业务进程服务。`--side client` 拒绝外部执行配置，不静默改测另一个运行档。
`--side paired` 可使用同一执行配置，但 Host JSON 须提供两侧事实：Client 的
`runtimes` 为 `["portable-js"]`，Executor 为 `["node"]` 或 `["exec"]`，以及各自
的版本 / 平台 / 依赖事实。不能把 Executor 的运行档或 Node 类型套到 Client。

运行中 INT / TERM 会先请求受控 Native owner 清理，再在有限等待后停止仍存活的
自有工具进程组。退出 130 / 143 表示测试被中断，不是业务成功或业务取消回执。
手工全套可加 `scripts/test-plugin-dev.py --execute --node-executable 实际Node路径
--node-sdk 可信SDK目录`，该 verifier 的版本探针只针对明确选定的本机工具。

`test` 需要显式启用 JS 的工程内 Core 构建，不复用默认 JS-off 的 `core/build`：

```sh
node tmp/plugin-dev/tools/node_modules/typescript/bin/tsc -p packages/plugin-sdk/tsconfig.json --outDir tmp/plugin-dev/sdk
cmake -S core -B tmp/plugin-dev/core -DGHOSTERM_WITH_PLUGIN_JS=ON -DGHOSTERM_WITH_LIBSSH2=OFF -DGHOSTERM_BUILD_TESTS=ON -DGHOSTERM_PLUGIN_SDK_DIR="$PWD/tmp/plugin-dev/sdk"
cmake --build tmp/plugin-dev/core --target ghosterm_core test_config -j 4
python3 scripts/gterm-plugin test --project tmp/plugin-dev/hello --tools tmp/plugin-dev/tools --contract tmp/plugin-dev/hello/plugin.test.json --core-build tmp/plugin-dev/core
python3 scripts/gterm-plugin test --project tmp/plugin-dev/hello --tools tmp/plugin-dev/tools --contract tmp/plugin-dev/hello/plugin.test.json --core-build tmp/plugin-dev/core --side client
python3 scripts/gterm-plugin test --project tmp/plugin-dev/hello --tools tmp/plugin-dev/tools --contract tmp/plugin-dev/hello/plugin.pair.test.json --core-build tmp/plugin-dev/core --side paired
python3 scripts/test-plugin-dev.py --tools tmp/plugin-dev/tools --core-build tmp/plugin-dev/core --mutate --receipt tmp/plugin-dev/receipt.json
```

macOS 显式设置 `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`。本片验收
工具链是 macOS arm64，Linux 原子发布分支未据此记为已验。

脚手架两侧各注册 `dev.echo`，使同一契约可以分别验证 Client 与 Executor 的实际入口；
业务插件可为两侧准备不同契约。单侧测试只激活指定的一侧，另侧为自有 Native RPC peer；
测试 Client 时由 Executor peer 发请求，方法的 receiver 为 Host，使用 Executor 自己的
lease，不能把 Host token 复制过去。输出 `side` 记录实际测试侧。显式 `--host-json`
的 `side` 必须与 `--side` 相同；缺能力 / 拒绝 / 未知继续分别报告，不以测试选项授予权限。

`--side paired` 使用脚手架的 `plugin.pair.test.json`。它在普通契约上增加 `methods`
数组，每项为 `{"name":"dev.run","receiver":"client"}`；最多十六项，名字不可重复，
必须显式声明两个方向，根 `method` 也必须在表内。每个方法的输入 / 输出为 JSON
对象，具体业务 schema 由两侧 SDK 校验。脚手架的实际链路是 Client `dev.run` →
Executor `dev.exchange` → Client `dev.callback`，最后返回
`client:executor:callback:hello`。错误仍独立比对 outcome kind，不能把进程退出当业务回包。

paired 的显式 Host 文件必须提供各侧自己的事实，分别做 metadata / Native 包检查：

```json
{
  "client": {"side":"client","sdkVersion":"1.0.0","protocols":[1],"runtimes":["portable-js"],"capabilities":["session.rpc"]},
  "executor": {"side":"executor","sdkVersion":"1.0.0","protocols":[1],"runtimes":["portable-js"],"capabilities":["session.rpc"]}
}
```

两端分别捕获同一固定包并核对 digest，在同一非 UI worker 上按各自预算轮询。
Native 控制请求使用接收侧对端的原 lease，仅借用 Runtime 持有的 Session；两侧
Session 都由各自 Runtime 销毁。成功值必须来自真实 JS 往返，不安装假业务 handler。
两个入口都须激活，两个 Runtime 资源都须独立释放，整体 fd 数量回到起点；这不覆盖
跨进程内存 / RSS、SSH、Native UI 或平台生命周期。

双侧外部执行例如：

```sh
python3 scripts/gterm-plugin test --project tmp/plugin-dev/hello --tools tmp/plugin-dev/tools --contract tmp/plugin-dev/hello/plugin.pair.test.json --core-build tmp/plugin-dev/core --side paired --host-json tmp/plugin-dev/paired-host.json --execution-json tmp/plugin-dev/execution.json --allow-process-execution
```

Client 仍在真实 QuickJS，Node 或 Exec 通过既有进程协议运行。契约的根方法可以
属于任一侧；Native 控制请求只负责触发和观察，业务往返与回调必须由两侧实际
处理器完成。结果同时记录两个 owner 的激活 / 释放和外部子进程退出；任一回调
返回错误值、摘要 / 版本不符或清理失败都不能通过。INT / TERM 先停止两个 owner，
再在同一有限期限内关闭它们；不把进程退出改写成业务成功 / 取消。

测试契约是脚手架生成的 `plugin.test.json`：命令、原始 argv、方法、输入 / 预期对象、
错误输入与预期 outcome kind。可加可选整数 `timeoutMs`（1–10000 毫秒），缺省为
1000；它只配置本次 Native 控制请求的期限，输出 `requestTimeoutMs` 记账。
它不改变插件内嵌套请求、业务子进程生命期或 Core 的 CPU / 内存预算；插件内部
请求须按自己的业务策略选择期限。testkit 要求成功和声明错误都收到完整业务回包，
期限耗尽产生的未确认 unknown 必须判测试失败，不能冒充声明错误。
默认仅授予 `session.rpc`，需要其它服务会显式失败。
预期值及输入中的完整字符串 `${instanceId}` / `${epoch}` 可绑定本次 lease；字符串
内嵌片段不替换。夹具使用本次 epoch 的本机 socketpair、真实包检查 / SDK / RPC，
对选定侧验证激活、成功值、声明错误类别、发送前排队取消、旧 epoch、撤权、配置预算拒绝、
引擎释放和 fd 数量平衡。脚手架的 `throw` 是明确错误夹具，结果为 unknown，不能
将其当作成功业务结果。

单侧 Executor 和 `--side paired` 可显式接入只读资源根，支持 Portable、Node 与 Exec；Node / Exec
同时需要上述执行配置和执行授权。命令使用绝对目录，不从 cwd 或环境猜测根：

```sh
python3 scripts/gterm-plugin test --project tmp/plugin-dev/resource --tools tmp/plugin-dev/tools --contract tmp/plugin-dev/resource/plugin.test.json --core-build tmp/plugin-dev/core --resource-root "$PWD/tmp/plugin-dev/source" --allow-resource-access
```

本次授权将该目录以 `workspace` 提供给 Executor。所选 variant 必须显式 requires
`session.rpc`、`resource.receive`、`file.readScoped`，manifest 声明对应 Executor
权限；显式 Host JSON 的 capabilities 也须恰为这三项，已有 denied 事实仍拒绝。
未提供 Host JSON 时使用上述能力的 Portable Executor 测试 profile。缺授权拒绝，
`--side client` 不接受此根；默认 RPC profile 不因此获得文件服务。paired 的两份
Host 事实独立：Client 的 capabilities 恰为 `session.rpc / resource.receive`，
Executor 恰为上述三项；manifest 也分别声明。根只传给 Executor，Client 的
`resources.workspaces` 为空。未提供 Host JSON 时使用相应的双 Portable profile。

插件自行调用真实 `resources.publish`，Native 对端接收。契约可增加 `resource`
对象，预期恰好一次发布；不提供该对象则验证没有收件：

```json
{
  "sha256": "<源文件的64位小写SHA256>",
  "bytes": 65536,
  "displayName": "file.bin",
  "mediaType": "application/octet-stream"
}
```

该对象位于原契约的 `resource` 字段，不能替代原 RPC 输入 / 预期结果。当前每个
Store 上限 1 MiB、单资源 256 KiB、四个资源槽；测试根读取上限 256 KiB。对端
核对实际描述符、快照逐块读取的摘要和错误 lease 拒绝，Session 关闭后再次读取
原保留快照，核对 pin 阻止 Store 销毁、显式 release、预约 / 引用归零及 fd 平衡。
输出 `services: ["resources"]` 及实际收件观察；预期摘要不符或释放失败不能通过。
只读根下的越界路径、其它根名和符号链接均不能发布；没有文件写入服务。此档
不绘制资源，不代表 SSH、吞吐或完整文件变化 / 限额矩阵已验。

paired 保留原方法表和实际 Client → Executor → Client 回调，也支持根方法直接
指向 Executor。资源收件始终在 Client 一侧，独立使用 Client Session 的 lease；
根请求从哪侧发起不改变资源归属。默认仍由 Native 接收并核验上述保留快照。

要验证 Client 插件实际读取，在 paired 契约增加 `"resourceConsumer":"client"`。
此时 `resource` 必填，`expected.resource` 必须与之相同；业务成功结果必须包含
`resource` 回执，字段为 `sha256 / bytes / displayName / mediaType`，其中 `bytes`
是本次实际读取的整数计数。Client 通过真实 SDK `take / open / stream.read /
release` 读取，Executor 在发布确认后调用 Client 方法，Client 再把回执返回。
插件测试处理器应检查实际字节和 EOF；开发命令将业务回执与独立预期精确比对。

Native 同时要求 Store 曾实际持有资源、Runtime 关闭前没有待收件或保留 pin，防止
仅伪造相同 JSON 回执通过。两侧 owner 关闭后再验预约 / 引用归零及 fd 平衡；
该档不额外创建 Native 保留快照，也不把其字节摘要检查冒充 Client 读取证据。
输出资源观察带 `consumer: "client"`，报告 scope 明确区分消费方式。两个档不能
同时抢同一个收件队列。可复验的测试例见 `scripts/test-plugin-dev.py` 的
`resource-pair-client` 系列：64 KiB 全字节模式、EOF、精确回执及显式 release，
并有“匹配回执但从未发布”的负向夹具；多资源业务数量须另写契约断言。

配置预算准入不等于 OOM / RSS 扫描；请求取消不等于业务任务取消。本片不覆盖 OS
沙箱、SSH、tmux、原生 UI、整进程预算或完整插件生命周期矩阵。
隔离变异各要求正常断言红、独立 Core 配置绿、恢复绿及源码 SHA256；编译失败、
崩溃和超时不计目标红。新增手工脚本未加入 CI 二十份。

业务进程服务使用独立配置和独立授权；`--allow-process-execution` 只允许启动
Node / Exec Executor。要让插件通过 `context.process` 启动业务子进程，提供
`--process-json 文件 --allow-business-processes`，支持单侧 Executor 和 paired。
例如以下配置中的目录、程序、版本和摘要须替换为已独立核对的实际值：

```json
{
  "workspaceRoot": "/absolute/path/to/workspace",
  "programs": [{"name":"business","executable":"/absolute/physical/path/to/program","version":"1.0.0","sha256":"<64位小写SHA256>"}],
  "environment": [{"name":"GT_TEST_VALUE","value":"selected"}],
  "lifetimeMs": 3000,
  "outputBytes": 65536
}
```

programs 为 1–16 个不重名映射，environment 为 0–32 个不重名条目。配置要求
全部五个字段、绝对路径、规范摘要及有界整数；CLI 的 lifetimeMs 上限十分钟，
outputBytes 上限 1 MiB，Core 的既有验证和输出 / 期限限制仍生效。插件所选
Executor 必须 requires `session.rpc / process.spawn / process.pipes`，manifest 的
process.spawn 权限须列出程序名。Host JSON 仍独立提供程序依赖事实；配置中的
版本是原生 Host 已核对的版本声明，命令不自动执行任意程序的 `--version`。
Core 验证可执行字节摘要，启动前再核对文件身份；不从 PATH 找替代程序。

```sh
python3 scripts/gterm-plugin test --project tmp/plugin-dev/business --tools tmp/plugin-dev/tools --contract tmp/plugin-dev/business/plugin.test.json --core-build tmp/plugin-dev/core --host-json tmp/plugin-dev/business-host.json --process-json tmp/plugin-dev/process.json --allow-business-processes
```

paired 的 Client 不声明 process.*，也不接收 process 服务或 workspaceHandle；
Executor 才得到程序映射和 cwd。Node / Exec 另加已核对的 execution JSON 及其
执行授权。可以组合资源服务，但 resource-root 必须与 workspaceRoot 使用同一
绝对目录，两种权限仍分别检查。缺本次业务授权、侧别不符、声明或 Host 依赖不符
都失败，不静默改测另一运行档。配置只支持当前列出的字段，未知字段拒绝。

子进程只获得显式 environment，Executor 自己的 TMPDIR 配置不会自动传给业务
程序。cwd 以不含绝对路径的 workspaceHandle 提供；它选择工作目录，不限制原生
程序的文件系统访问，也不是同用户 OS 沙箱。插件自行核验 stdin / stdout / stderr、
EOF、实际 exit code / signal，再返回契约规定的业务回执；退出码不自动等于业务
成功。结果 `services` 包含 `processes`，Runtime / fd 释放与业务回执分别核验。
可复验的自有 C 子进程和三运行档处理器见 `plugin-dev-business.c` /
`plugin-dev-process.ts`；中断用例先观察自有子进程 PID，再发 INT / TERM 并核对
实际回收，不以父进程退出代替子进程结束。自有业务夹具的子进程生命期为 3000 ms，
对应成功契约显式使用 `timeoutMs:4000`，Client 内的业务转发也显式请求 4000 ms；
普通脚手架与其它 RPC 仍为 1000 ms。`business-window` 用 1200 ms 受控等待验证
1000 ms 失败、4000 ms 成功、再改回 1000 ms 仍失败；没有增加生产预算或自动延长
请求。完整输出 / 期限 / 进程树和平台矩阵仍待验。

Client 的视图注册使用 `--views-json 文件 --allow-native-views`，支持 `--side client`
和 `--side paired`。配置只接受如下 `kinds` 数组：一至三项、不重复，可选 table / diff / task。

```json
{"kinds":["table","diff","task"]}
```

Client 的 manifest `requires` / `permissions` 和 Host 事实必须声明配置中的
`ui.table` / `ui.diff` / `ui.task`；所选命令的 `views` 必须覆盖这些种类。
Host 可以另行声明其它已知 UI 种类，但 Native 只启用本次配置选择的种类。
Client 声明的所有 requires 仍须获得本次 Session 授权；不能声明依赖三种 UI，
却只授权一种并绕过 Core 的包准入。
Executor 不获得 UI 服务，单侧 Executor 不接受此选项。两侧默认 profile 可在
未提供 Host JSON 时使用；Node / Exec paired 仍须另行提供原执行配置与授权。

测试契约在原字段之外增加 `methods` 和 `views`：

```json
{
  "methods": [
    {"name":"dev.echo","receiver":"client"},
    {"name":"table.describe","receiver":"executor"},
    {"name":"table.page","receiver":"executor"}
  ],
  "views": [
    {"kind":"table","viewId":"table-a","methods":{"describe":"table.describe","read":"table.page"}}
  ]
}
```

这是对原 RPC 契约的补充，不是独立完整契约。`methods` 最多十六项、不重名，
必须含根 RPC 方法和两个方向；单侧 Client 的根方法必须指向 Client。
所有视图的 describe / read / cancel 方法必须显式指向 Executor。
`views` 为一至八个预期注册，viewId 唯一；task 额外需要 `cancel`，每项的方法名
必须互不相同。kind、viewId 与所有方法逐字段精确比较，不在预期中写随机 handle。

```sh
python3 scripts/gterm-plugin test --project tmp/plugin-dev/views --tools tmp/plugin-dev/tools --contract tmp/plugin-dev/views/plugin.test.json --core-build tmp/plugin-dev/core --side client --views-json tmp/plugin-dev/views.json --allow-native-views
```

插件在真实 Client Runtime 调用 `ctx.views.open`。testkit 用公开 Core 门面启用
服务，在原成功 / 错误 / 排队取消检查后读取实际快照，核对实例 / epoch / revision、
句柄格式 / 唯一性及预期注册；再由 Native 独立关闭首个视图，要求旧句柄与外来
句柄拒绝、revision 恰加一、其余注册和句柄逐字保持。停止 Runtime 后必须清空
快照输出，仍核对两个 engine、子进程及 fd 的既有释放条件。结果的 `services`
包含 `views`，`observations` 增加关闭前后快照，生命周期记录 `viewsRevoked`。

这证明本地注册服务和句柄生命周期；未自动调用每个视图业务方法，也未渲染原生
表格 / diff / task，不代表可访问性、大字号、主题或 UI E2E。业务方法和原生
呈现仍分别写契约或使用平台专项。缺省仍为 kind 注册检查。要让必需 `ui.views` 的包进入无窗口多视图容器 driver，
在配置中显式加 `"registry":true`；此时 Client 另须声明并获得 `ui.views`，Host
也须提供对应事实。没有该选项不会因为 Host 声明可用而自动授予；Executor 仍不能
获得注册或容器服务。容器 driver 支持读取注册、独立关闭与重开身份检查，不创建
产品窗口、不替代原生渲染或无障碍验收。输出 `viewServices` 记录实际选择的服务。
三服务组合与完整种类 / 数量 / 平台矩阵另验；注册首片见第二十三批，官方包顺序
场景见第二十四批。

paired 契约可增加 `steps`，在原成功 / 声明错误 / 排队取消和首次视图关闭检查后，
按顺序驱动两个真实 Runtime。根契约、每个方法的 receiver 及原来的完整回包检查
保持有效；每步使用实际调用方自己的 lease，不复用对侧 token。支持以下动作：

- `request`：指定 allowlist 中的 method、对象 input，以及 `expected` 或
  `expectedSchema` 二选一。expected 是完整 outcome，例如
  `{"kind":"success","value":{"opened":true}}`，不是单独的 value。
- `closeView`：指定 viewId，关闭当前 Native 注册；此动作不发送任务取消。
- `snapshot`：指定预期 views 数组，格式与根契约相同，可以为空。先检查所有已关闭
  句柄继续 NOT_FOUND，再比对当前注册、不可变绑定及新句柄的 nonce / 高水位。

例如给官方多视图包的原契约追加：

```json
{
  "steps": [
    {"action":"request","method":"views.reopen","input":{"viewId":"alpha"},"expected":{"kind":"success","value":{"opened":true}}},
    {"action":"closeView","viewId":"progress"}
  ]
}
```

这是契约补充：根 methods 必须同时列出 Executor 方法和 Client 的
`views.client.open`，根 views / command / expected / errorInput 等仍需保留。
根注册检查会先关闭首个视图，样例中的 alpha 重开因此应返回 opened=true。
会改变注册集合的 RPC 后，用显式 snapshot 观察新集合，再驱动后续 Native 关闭。

最多十六步，单请求输入 8192 B、全部步骤请求输入 32768 B，schema 总计 65536 B；
(2 + request 步数) × timeoutMs 不能超过 20000 ms，Native 整体仍有原 30 s
外层期限。不提高 Core 的 CPU / 内存 / 进程预算。步骤在同一个 Native owner 上
依次执行，Runtime 关闭后比对收集的值；断言不回滚已经调用的方法。

`expectedSchema` 接受自包含 JSON Schema 2020-12 对象，由锁定 AJV 8.17.1 同步
编译 / 校验。外部引用不自动获取，不加载插件模块。schema 先于 Native 入口编译；
拒绝 `$async` 校验器，关闭 coercion、defaults 和额外字段移除。每次验证有独立有界
工具期限。普通 expected 逐层比较 JSON 类型，false 不等于 0，数字 64 与 64.0
仍为相同 JSON 数字；完整字符串 `${instanceId}` / `${epoch}` 可以绑定本次身份。

实际官方包场景由以下脚本生成完整契约：

```sh
python3 scripts/test-plugin-dev-scenarios.py --tools tmp/plugin-dev/tools --core-build tmp/plugin-dev/core --mutate --receipt tmp/plugin-dev/scenario-receipt.json
```

它直接复制 `plugins/official/views` 的 Client / Executor 源，验证两张表、diff、
任务视图关闭后仍 running、重新打开、显式取消与终态、同 ID 取消幂等和新句柄。
加 `--execute --node-executable 物理路径 --node-sdk 可信目录` 可验同一 Executor
源的 Node 档，以及 invalid schema 不得进入 Node 入口、真实 INT / TERM 回收。
不测试网络、SSH、真实模型或 GUI。`--mutation-case 名称` 只缩小本次变异选择，
收据逐项记录实际执行组；默认 `--mutate` 执行全部本脚本保护，不计入 CI 二十份。

用完删除本次创建的 `tmp/plugin-dev/`，保留其它工作的临时目录。开发命令每次调用的
内部固定副本与探针会在正常退出、INT / TERM 中断时清理；TERM 同时收回受控工具进程组。

## macOS 实验插件页控件链

执行端也可先登记显式开发包，再用普通 `gterm COMMAND [ARG...]` 启动前台实例，
用户在插件页明确附着该目录 / 公开实例。登记、修订、精确摘要选择、`@cwd` 根、
参数与 IPC 限额见 [gterm 开发目录说明](../../cli/gterm/README.md#执行端开发目录与普通命令实验)。
目录信任与 Session consent 分开；此入口可显示被动候选，但不会自动连接 App，不能把普通
命令成功解析当成实际业务成功。验证范围见插件计划 P2 第六十四批。

macOS 的“连接已有前台 gterm 实例”可以在选定主机 / 本机程序、私有目录和公开实例标识后，
点击“检查此位置的前台实例”，再核对并填入命令、参数、摘要与 RPC 方法。检查通过独立通道，
不启动插件；填入会撤回旧批准，需要重新明确允许连接，之后仍有 Session consent。
声明只描述前台固定配置；不自动选主机、不授予工作区或业务进程权限，也不等于检查了当前包内容。
Android 对应页也支持通过当前 SSH 配置与认证流程检查，再明确填入当前所选命令的声明。
修改目标或关闭表单会取消未完成的检查；沿用已有只读工作区与业务进程需分别允许，填入不会保留旧批准。

前台 `gterm` 在控制终端可发出有界、不可信的发现提示。macOS 顶部的“查看候选”和
Android 终端菜单的“前台插件候选”只在用户点击后进入插件页；提示不打开窗口或建立连接。
先批准匹配摘要的 Client，再点击“使用此候选”，自行选择实际执行位置并执行上述检查。
外层 SSH 别名只是来源说明，手工嵌套 SSH / tmux 不会自动推断目标。列表上限八项、
本机提示有效期三十秒，来源停止后不能继续使用；原 listener 的等待期限不变。
tmux passthrough 配置、帧格式、专项范围和剩余边界见 [被动候选说明](../../cli/gterm/README.md#被动终端候选实验)。

官方 Preview 现在接受所选工作区内的 `./docs/README.md` / `docs/./README.md`，附件也支持
同样的 `.` 身份分量。处理只在 Preview Executor 内进行，不改写 CLI 原始 argv，不执行 shell
展开，不替其它插件处理路径；绝对路径、`..`、空分量与目录目标仍明确拒绝。附件按处理后的
路径去重，验证失败发生在发布之前。

已有补丁版 `build/GhosttyKit.xcframework` 与锁定的主机 SSH 依赖后，可以用以下手工
专项验证官方 Preview 的页面接线：

```sh
mkdir -p tmp/plugin-pane
export TMPDIR="$PWD/tmp"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export GHOSTERM_AUTOMATION_OFFSCREEN=1
cmake -S cli/gterm -B tmp/plugin-pane/gterm -DGHOSTERM_WITH_PLUGIN_JS=ON -DGHOSTERM_WITH_LIBSSH2=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cmake --build tmp/plugin-pane/gterm -j4
python3 -B scripts/test-plugin-pane.py --gterm tmp/plugin-pane/gterm/gterm --route both --mutate --out tmp/plugin-pane/receipt.json
```

脚本复制原生源码到 `tmp/`，由 Apple gate 启动本轮独占的 loopback SSH 服务。
实际操作生产页面的导入、摘要批准、启动表单、会话批准与停止按钮；表单经原生
编辑通知提交。默认 `--route both` 共十条链：Markdown + PNG、独立 PNG、HTML +
CSS / PNG、Unicode HTML、HTML + 动画 GIF 各走直连与 ProxyJump，核对实际主机密钥提示的端口顺序，拒绝只看配置
就当双跳已执行。`--route direct` / `jump` 各运行对应五条链，缺夹具或跳过项判失败。
每份真实收件核对字节 / SHA256；Markdown 与图片取原生绘制像素，HTML 则对页面实际
嵌入的 WKWebView 取快照，核对 CSS 蓝背景与 PNG 红像素，并点击附件图片后切回
HTML，最后停止并检查资源控件及嵌入 WebKit 移除。

窗口在显示前已透明、离屏并禁止应用激活。目录选择由显式测试依赖返回，默认产品
仍使用系统 `fileImporter`；本专项不证明系统文件面板、VoiceOver、用户键盘焦点、
其它设备或完整生命周期，也不把已有原生像素专项当作多格式 / 多服务器全矩阵。

资源组件另跑 28 项基线。HTML 的实际嵌入视图还必须采用每视图独立的非持久
WKWebsiteDataStore；切换到图片再返回时使用新 store。测试刻意持有已移除和停止后
的 WKWebView，检查 delegate 解绑，并通过原 scheme handler 发起附件请求确认拒绝。
这证明 Native owner 撤销未来读取，不等同于任意并发回包 / 物理内存擦除或系统缓存取证。

快照路径按 UTF-8 字节识别：NFC / NFD 不合并，斜杠不按 Swift 的字形簇拆分。
Unicode SSH 档核对分解重音目录、`#?%` 主文档名和中文 / emoji 附件的原始路径字节。
默认基线显式选择这十条，不把需要独立签名夹具的版本管理测试混入后跳过。

macOS HTML 图片在后台准备为静态首帧 PNG，WebKit 只读取准备好的快照。原始图片
仍最多 1 MiB；两轴各最多 2048，整包共享 4,194,304 个解码像素，实际容器须与声明
MIME 相符。派生的 8-bit sRGB PNG 单独限为 16,842,752 字节，输出消费者在增长前
检查上限；JPEG 转 PNG 变大不会占用或放宽原始收件 / SFTP 预算。CSS 的既有 1 MiB
上限不变。失败附件显示提示，不交回 WebKit 解码原件；附件变化重新准备并撤销旧
视图的读取。动画 SSH 档核对原始 GIF 两帧、实际 scheme 返回 PNG 单帧及红色首帧
像素。这些限制不等于 ImageIO / WebKit 的进程 RSS 硬配额。

`--mutate` 默认 36 组：原九组摘要展示、来源批准、启动、会话批准、停止、Markdown
图片、独立图片、HTML 附件与资源选择，以及十二组 WebKit 存储 / 页面绕过 / 拆除 /
Coordinator 终态、幂等、stopLoading、delegate、附件撤销和 handler 关闭 / 晚更新保护，
五组 UTF-8 路径键 / 范围 / 斜杠分段 / URL 编码 / 前导斜杠，以及十组图片两轴 /
总像素 / 首帧 / 容器 / 派生预算 / 输出增长与失败终态 / 重复路径 / 附件到达保护。
`--mutation-case 名称` 仅选择本次组（可重复），必须同时指定 `--mutate`；基础十条
或对应五条链仍运行，收据逐项记录实际组及选择的测试，不能以选组称完整 36 组。
每组要求生产对象实际重编译、单条正常断言红、同副本独立配置读取绿、原件恢复绿
及 SHA256 一致；编译失败、崩溃和 unexpected failure 不计目标红。收据记录指定
源码、实际 gterm / XCTest 与链接 Core / QuickJS / libssh2 摘要。gate 由仓库
`gt_timeout` 包装，INT / TERM 沿受控子进程、变异恢复和临时副本清理退出。
此为手工证据，不增加 CI 二十份。

Apple gate 会显式构建 JS-enabled Core。后续普通闸门先恢复默认配置：

```sh
cmake -S core -B core/build -DGHOSTERM_WITH_PLUGIN_JS=OFF -DGHOSTERM_BUILD_TESTS=ON
bash scripts/gate.sh
```

核对收据后清理本次创建的 `tmp/plugin-pane/`；脚本内部的原生副本在退出时清理。

## Android 插件页面返回终端

从终端菜单离开到主机列表 / 文件页时，保存该会话离开前的实际输入法可见性；从插件
设置返回后，继续同一终端会恢复原 PTY 和输入焦点。原本打开的键盘恢复，原本隐藏的
键盘保持隐藏。这是本次页面往返的一次性状态，不是重新启用「新连接自动显示键盘」；
不持久化到应用重启，不把另一会话的状态套到新会话，已经退出的 PTY 丢弃恢复请求。

设备专项须使用明确指定的测试设备；自动 AVD 需无窗口运行。先构建 JS-enabled App
和测试 APK，并准备 `doctor` 报告 Portable JS 可用的真实 `gterm`。以下每个 SSH
命令分别运行一条真实 SAF 选包 / 来源批准 / 双跳 Preview 链：

```sh
bash scripts/build-android.sh -PghostermPluginJs=true :app:assembleDebug :app:assembleDebugAndroidTest
python3 -B scripts/test-plugin-preview-android.py --android "$ANDROID_SERIAL" --gterm tmp/plugin-pane/gterm/gterm --test com.hanxi.ghosterm.PluginPreviewSettingsE2ETest#realTwoHopPreviewReturnsToSamePtyWithOpenIme
python3 -B scripts/test-plugin-preview-android.py --android "$ANDROID_SERIAL" --gterm tmp/plugin-pane/gterm/gterm --test com.hanxi.ghosterm.PluginPreviewSettingsE2ETest#realTwoHopPreviewReturnsToSamePtyWithoutSummoningHiddenIme
ANDROID_SERIAL="$ANDROID_SERIAL" python3 -B scripts/mutation-check-android-plugin-return.py
```

两条 Preview 链核对 Markdown / PNG 实际绘制，回到同一个会话 UUID / Surface、实际
窗口焦点及系统 IME；通过 OS 按键在原 shell 生成 PID 文件，与原 owner 记录对拍。
再次进入插件页时保留 Runtime 实例 UUID 和资源回执，随后真实停止、释放，并核对
远端退出和临时资源清理。Runtime 状态快照会持续更新，不把快照对象地址当实例身份。
Host 关闭长会话后远端结果仍为 unknown / 退出码 7，不能记成远端任务成功。

新增变异脚本默认九组，`--case 名称` 可重复选组；目标是捕获开 / 关状态、主机 / 文件
离开接线、返回恢复、隐藏状态、一次性消费、UUID 隔离和已退出 owner 的输入拒绝。
逐组真实重编译、单条指定正常断言红、同 APK Core 绿、恢复重编绿及源码 SHA256。
编译失败、跳过、崩溃及夹具错误不计目标红；INT / TERM 恢复生产源。该专项不替代
原会话面板 / 布局保护、原生 Release / lint 或完整手机 / 输入法 / 生命周期验收，也
不增加 macOS CI 二十份。临时内容均放工程 `tmp/`，核对收据后及时清理本轮内容。

## Android 静态 HTML 预览

收到的 `text/html` 默认进入原生 WebView，可显式切换查看源码。文档最多 256 KiB，
只映射它后面、下一份 HTML / Markdown 文档之前的前六个显式附件，且必须位于同一
文档目录范围；不扫描磁盘。文档与附件核对已知长度和 SHA256，CSS 最多 256 KiB，
单附件最多 1 MiB。PNG / JPEG / GIF / WebP 的首帧重编码成静态 PNG，单边最多
2048、整包最多 4,194,304 个解码像素；不接受 SVG、歧义同名或无效 UTF-8。

每个视图使用独立的合成 HTTPS origin，唯一主文档和固定 CSS / 图片由本机拦截器
返回，未映射请求一律拒绝。脚本、网络回退、文件 / content 读取、导航、表单提交、
下载、文件选择和权限请求不开放；内联样式受 `style-src 'self'` 限制。WebView 不设
JS 桥，不取终端焦点 / IME。切换源码 / 资源、附件变化和移除面板会销毁旧视图，
撤销后续附件读取；它不承诺擦除已在途回包或浏览器进程的物理内存。

沿用上节 JS-enabled APK 与 gterm，以下专项经真实 SAF 选包、来源批准、双跳 SSH、
会话批准，检查生产设置页的 HTML / CSS / PNG 屏幕像素、资源切换及停止 / 释放：

```sh
python3 -B scripts/test-plugin-preview-android.py --android "$ANDROID_SERIAL" --gterm tmp/plugin-pane/gterm/gterm --preview-format html
python3 -B scripts/test-plugin-preview-android.py --android "$ANDROID_SERIAL" --gterm tmp/plugin-pane/gterm/gterm --preview-format html-unicode
ANDROID_SERIAL="$ANDROID_SERIAL" python3 -B scripts/mutation-check-android-html.py
```

同类另有 `realTwoHopHtmlReturnsToSamePtyWithOpenIme` 和
`realTwoHopHtmlReturnsToSamePtyWithoutSummoningHiddenIme`，增加原 PTY / 实际 IME 往返。
`--test` 不指定时按 Markdown / HTML / Unicode HTML 选择相应生产设置页测试；
夹具只生成所选格式，默认 Markdown 不再指向使用 `fixture.txt` 的旧控制器测试。
Unicode 档使用分解重音目录、包含 `#?%` 的主文档及中文 / emoji 附件。URL 编码保留
原始路径码点；请求路径严格校验 UTF-8，百分号只解码一次，不把非法序列替换成
合法的 U+FFFD 文件名，也不把 NFC / NFD 当作另一条已授权路径。

HTML 变异脚本默认 24 组，`--case` 可重复选组；每组要求真实编译后单条正常断言红、
同 APK 独立 Core 绿、恢复重编绿与 SHA256。启动 / 编译 / 崩溃 / 日志传输异常不计红。
记录实际系统与 WebView 版本，专项不替代完整格式 / 生命周期 / 设备矩阵，不增加
macOS CI 二十份；核对收据后清理本轮 `tmp/`。

## 离线签名发布候选

`gterm-plugin release sign / verify` 是 P6 的开发工具首片，生成 / 检查候选发布包，
不安装、激活插件或保存来源批准。macOS 的插件包页面现可对已审查目录执行同格式的
签名核对；Core 的结构审查仍不认证发布者，页面另列本次可信策略快照下的签名证明。
当前没有内置官方公钥或开放第三方市场。

先用原 `pack` 命令得到 ZIP，再显式选择可信渠道策略与本机签名器：

```sh
python3 scripts/gterm-plugin pack --package build/plugins/preview --tools tmp/plugin-dev/tools --out tmp/preview.zip
python3 scripts/gterm-plugin release sign --artifact tmp/preview.zip --policy /可信位置/channel.json --key-id 公钥指纹 --signer-json /可信位置/signer.json --tools tmp/plugin-dev/tools --out tmp/preview-release
python3 scripts/gterm-plugin release verify --artifact tmp/preview-release/plugin.zip --descriptor tmp/preview-release/release.json --signature tmp/preview-release/release.sig --policy /可信位置/channel.json --tools tmp/plugin-dev/tools
```

官方候选包可以用一次性测试渠道统一核对。先准备前文的锁定开发工具
`tmp/plugin-dev/tools`，以及匹配 `scripts/fixtures/plugin-sdk-tools` 锁文件的 SDK 工具
`build/plugin-sdk-tools`。在工程根目录执行：

```sh
export TMPDIR="$PWD/tmp"
mkdir -p tmp/official-release
python3 scripts/build-plugin-codex.py --tools build/plugin-sdk-tools --out tmp/official-release/ai-codex
python3 scripts/build-plugin-claude.py --tools build/plugin-sdk-tools --out tmp/official-release/ai-claude
python3 -B scripts/test-plugin-release.py \
  --tools tmp/plugin-dev/tools --sdk-tools build/plugin-sdk-tools \
  --ai-packages tmp/official-release/ai-codex tmp/official-release/ai-claude \
  --receipt tmp/official-release/receipt.json
```

测试通过后，收据应包含 44 项基线，其中七项分别核对 Preview、Table、Diff、Task、
Views、Codex 和 Claude 的签名归档。每个测试策略限定一个插件 ID，验签后逐成员
比对归档与构建目录。Claude 的 `--fetch` 允许下载锁定的构建依赖。完成核对后可清理
本次 `tmp/official-release/` 产物。

在启用 Portable JS 的 macOS 开发构建中，先选择并审查目录包，再展开「目录包发布签名」。
填写 `release.json`、`release.sig` 与渠道公钥策略的绝对路径；从独立可信来源核对策略后，
勾选信任确认并点击「核对目录包签名」。包附带的策略不能自行建立信任。成功时显示渠道、
公钥 / 策略 / 已捕获目录包的 SHA-256；修改材料路径或撤回策略信任会清除旧证明，晚到结果也会丢弃。

「目录包发布签名」只把描述中的身份及包摘要与 Core 已捕获的不可变目录快照绑定，不导入或验证 ZIP，
也不把描述声明的 ZIP 摘要当作已读取的文件证据。ZIP 的完整传输及内容验证可使用上面的 CLI
`release verify`，或下面的原生发布包安装入口。目录签名核对不会批准来源、授予 Session 能力或安装插件。
所有信任只对应当前策略文件快照，没有联网撤销刷新或防回滚保证，退出页面不保存这份证明。

原生跨层手工验证（公钥材料由一次性内存 agent 生成，窗口在后台）：

```sh
GHOSTERM_BUILD_PLUGIN_JS=1 bash scripts/build-core.sh
GHOSTERM_CORE_TEST_PLUGIN_JS=1 python3 -B scripts/test-plugin-native-signature.py --tools tmp/plugin-dev/tools --mutate --out tmp/native-signature/receipt.json
```

需要完整 Xcode 的 Swift 工具链。脚本只变异工程 `tmp/` 内的 Apple 副本，退出时清理；
结果收据留在显式输出路径。验收后恢复 Core 的 `GHOSTERM_WITH_PLUGIN_JS=OFF` /
`GHOSTERM_BUILD_TESTS=ON` 配置并运行默认离线闸门，核对收据后清理本轮输出目录。

## macOS 签名发布包的检查与安装候选

仅启用 Portable JS 的开发构建提供此入口。插件包页面点击「安装签名发布包…」，填写
ZIP、发布描述、签名及独立可信渠道策略的绝对路径，确认策略后点击「检查发布包」。
App 使用 CryptoKit、系统 libz 和 Core 完成本地检查，不调用 Node、Python 或 unzip，
不联网获取策略或依赖，也不沿 SSH 部署文件。

检查先验证签名及 ZIP 的实际字节摘要，再核对 ZIP32 目录 / 本地头、连续范围、路径、CRC、
stored / raw DEFLATE 流和预算。私有暂存内容由 Core 校验完整性、SDK / 协议、模块与本机
Client 兼容性；页面展示发布者、精确摘要、运行时、权限目的 / 程序声明，以及执行端与依赖
声明。逻辑包完整保存于本机；这不表示已经部署或验证了远端执行端的实际依赖环境。

确认这些信息后点击「安装此发布包」，将候选保存为独立只读版本。默认存放在
`~/Library/Application Support/Ghosterm/PluginPackages/<发布描述 SHA-256>/payload`；
同级保存公开的描述、签名与当次策略材料，策略副本只是证据，不会自动变成下次的可信锚。
文件写入后同步，目录冻结后以禁止覆盖的原子改名发布，再同步父目录；父目录同步失败时
明确保留“已发布、持久性未确认”，不伪装未发生或重复安装。已有目标即使为空目录也不会覆盖。

修改输入、取消或离开页面会释放本次暂存。安装前检查成员集合、文件身份 / 模式 / 大小和
修改时间 / 状态变化时间；检查后改动、删除或加入成员须重新检查。清理失败会显示保留位置，
保留原 owner，并提供显式重试；不按路径前缀猜测归属。安装根目录为当前用户私有目录，
失败信息中的位置是原登记位置，目录身份变化时不表示它当前仍存在；恢复后仍须核对同一 owner。
准备期间持有互斥锁，最多保留 64 个版本；当前没有自动删除旧版本或崩溃遗留的维护入口。

安装后先在「管理已安装版本…」核对并选择活动版本，再单独批准精确来源与本次 Session；
安装不执行入口或 lifecycle hook。旧目录与已捕获的 Core 包不因新版本发布而替换。
版本选择、手动升级 / 回滚、禁用和显式旧版本回收见下节；自动更新、安装中崩溃遗留维护
与断电恢复仍未完成，不将当前候选称为完整安装管理器。

原生版本发布手工验收：

```sh
GHOSTERM_BUILD_PLUGIN_JS=1 bash scripts/build-core.sh
GHOSTERM_CORE_TEST_PLUGIN_JS=1 python3 -B scripts/test-plugin-native-signature.py --tools tmp/plugin-dev/tools --install --mutate --out tmp/native-install/receipt.json
```

仅使用工程 tmp 内的签名夹具、暂存和离屏窗口。结束后按上节恢复 Core 默认档并核对、清理本轮目录。

## macOS 活动版本、手动升级与禁用

点击「管理已安装版本…」会只读刷新本机版本目录；重启后仍可列出版本及本机选择。
列表里的发布者 / 版本属于包内声明，标为「当前策略待核对」；损坏、缺失和读取失败
单独显示，不把它们当成“没有安装”。未知目录条目只计数，不按名称自动清理。

选择版本前填写当前独立可信策略的绝对路径并明确确认，然后点击「核对并设为活动版本」；
已选版本使用「重新核对并载入」。宿主重新读取原始描述 / 签名与真实 payload，通过当前
策略验签及 Core 兼容 / 完整性检查，不从已安装的 `policy.json` 自动继承信任。
新包安装后选择其版本即手动升级；选择保留的旧版本即显式回滚，过程不删除原包，
不迁移来源批准、Session 能力或旧请求。重启或重新选择后，仍需单独批准精确来源。

「禁用新建」只修改本机选择，阻止新建 Session / Runtime，不需要重新取得发布者策略。
已运行实例保留原固定包与权限；需要结束它时另点停止。所选版本缺失或不可读时，
界面仍显示原选择并提供禁用记录入口。已知受管理包缺选择、指向缺失版本、记录损坏或
不能读取时，新建失败，不退回开发目录的同名包。

选择存于安装根的 `selection-v1.json`，只含 schema、随机 nonce 与各包的 release ID /
enabled / 选择代次；不保存来源批准或 Session 授权。完整原始文件 SHA-256 是 CAS revision，
写前及改名前核对，文件 fsync 后原子替换并同步父目录。旧窗口 / 另一进程的陈旧 revision
须刷新重试；父同步失败仍明确记录“已写入、持久性未确认”。各包独立选择代次使另一
进程切换或重新启用后，旧进程的历史批准不能自动复活。该机制不提供网络策略新鲜度或
自动撤销所有既有批准的服务，选择时以本次提供的策略快照为准。

完整手工版本管理专项需要显式 gterm，并只连接脚本自建的 localhost SSH：

```sh
GHOSTERM_CORE_TEST_PLUGIN_JS=1 python3 -B scripts/test-plugin-native-signature.py --tools tmp/plugin-dev/tools --versions --gterm tmp/plugin-dev/gterm/gterm --mutate --out tmp/native-versions/receipt.json
```

`--versions` 包含原签名 / 安装专项；额外运行真实 SSH 中的活跃 Preview，在选择另一
固定版本及禁用后继续由旧 Runtime 发布 Native 资源并核对实际对端回执，最后正常停止。
此为受控手工证据，不代表所有插件 / tmux / 多设备 / 丢包恢复矩阵或云端发行。

## macOS 显式版本回收

在「管理已安装版本…」中，对仍启用的版本先点「禁用新建」；需要结束旧运行时，
另点「停止并释放插件」。然后点「预览回收此版本」，核对绝对目录、全部成员路径、
文件大小与 SHA-256，选择「保留并取消」或「确认删除以上目录与文件」。
当前发布者策略不是删除的前提，撤销或不兼容的版本仍能维护；工作区、远端执行端和
业务文件不在此操作范围内。已禁用选择继续保留，不借回收恢复开发目录来源或历史批准。

预览不改文件；它持有目标目录的独占引用，阻止此版本开始新的 Session。Session /
Runtime 持有各自版本的共享引用，直到正常关闭实际完成；列表中的占用属于一次观察，
「查不出来」不等于无人使用。确认时重新取得根锁，核对选择 revision、目录身份与
完整文件清单；变化、链接、特殊文件、跨设备条目或仍被引用的版本均保留。
每树最多 4096 成员、目录深度 34、单文件 16 MiB、合计 65 MiB，长路径和删除记录另有上限。
超限明确失败，不临时放宽预算或跳过文件。

确认后先原子写入并同步 `removal-v1.jsonl`，再将精确目录改名、同步父目录，最后按
记录逐项移除。启动和刷新不会自动执行删除；若中途退出，下次展开可查看原删除清单，
显式点「继续处理此已确认删除」。所有尚存文件必须仍属于原清单及目录身份，新文件或
替换文件会阻止清理；同一 release ID 的新目录不会被旧记录覆盖或递归删除。
「本次删除」和「观察到已不存在」分别报告，父同步失败另报持久性未确认。
已确认记录待处理期间，安装和活动版本选择暂停，禁用仍可操作。

尚未改名、完整原树仍匹配时，可点「撤销尚未开始的删除」解除待处理记录，原版本保留；
已经进入移除阶段则拒绝撤销。撤销不重新启用版本、不恢复来源 / Session。
不完整或被替换的记录不按目录前缀采纳。未登记目录的独立核对见后节；恶意同用户
进程持续改文件、断电和完整进程死亡 / 文件系统矩阵仍未验；后节补明确列出的实际本机进程死亡，
不把检查点故障注入当作实际断电证据。

包含纯原生目录维护、真实页面按钮和受管 SSH Runtime 引用的手工专项：

```sh
GHOSTERM_CORE_TEST_PLUGIN_JS=1 python3 -B scripts/test-plugin-native-signature.py --tools tmp/plugin-dev/tools --remove --gterm tmp/plugin-dev/gterm/gterm --mutate --out tmp/native-removal/receipt.json
```

`--remove` 包含签名、安装和版本专项，只操作脚本生成的私有夹具与独占 localhost SSH。
它不会清理用户安装目录；默认 App 仍 JS-off。结束后按前节恢复默认 Core，核对收据并
清理本轮 tmp 产物，不增加 macOS CI 二十份变异脚本。
调试单组可在 `--mutate` 后重复传入 `--case 保护组名称`；完整所选基线与真实 SSH 链仍执行，
收据只登记本次实际选跑的变异，不能将子集通过写成全部组通过。

## macOS 安装暂存归属与遗留维护

新安装在私有根的 `install-journal-v1/` 中先持久化精确目录名预约，再创建暂存并记录
目录 dev / inode / uid / birth time；数据写入后仍持有目录锁，发布前另记目标 release ID。
这些记录只描述本机文件归属，不认证发布者、不保存来源批准或 Session，也不恢复安装。
用户仍须重新提供 ZIP / 当前可信策略、验签和 Core 内容检查后才能发起新安装。

在「管理已安装版本…」刷新后可核对安装记录：

- 「仍在处理」保留；另一进程持有全局安装锁时读取会明确报忙，不当作没有记录。
- 「目录身份已匹配，未观察到活动引用」可预览完整清单，保留取消或确认回收。执行前
  重核记录摘要和目录 / 文件身份，后续修改不会沿用旧预览。先写删除意图再移除，途中
  再次退出时仍使用既有的显式继续入口；完成后清理原归属记录。
- 同一目录已在发布目标出现时，只能移除该条元数据，保留已发布版本；不会因记录还在
  就重装、覆盖或把它当暂存删除。源位置已不存在时也只移除记录，不据此判断安装成功。
- 只有预约、缺目录身份、目录已替换或记录损坏时保留并显示未知 / 错误。旧的无记录
  `.prepare-*` 和未登记目录须走后节独立核对；顶层文件仍保留，不按前缀采纳或自动清理。

安装目录和记录各有 64 条保留预算，未知元数据占预算，不通过隐藏损坏记录绕开上限。
正常发布 / 取消后可留下空的元数据目录；目录存在不表示有未完成安装。发布效果、父目录
持久性与元数据清理失败分别报告，不能在改名成功后把记录清理失败写成“没有发布”。

手工维护、原生按钮和实际进程死亡专项（默认 JS-off Core / 本机 GhosttyKit）：

```sh
TMPDIR="$PWD/tmp" python3 -B scripts/test-plugin-install-journal.py --mutate --out tmp/install-journal/receipt.json
```

脚本只用自有声明夹具，不安装 / 认证 / 运行插件。九个明确检查点的 SIGKILL 独立记账，
新进程只读观察后明确维护；包含预约未落目录、目录已建但身份未确认、部分文件、发布
前后和暂存回收再次中断。未声明断电 / 掉盘、完整磁盘故障 / 并发矩阵或旧无记录孤儿
已可自动清理。实际签名 ZIP 安装与活跃 Runtime 仍须运行前节 `--remove` 专项。

## macOS 升级／回滚前的活动选择预览

在已安装版本管理页，选择版本和重新载入现在先进入预览。仍须提供独立可信策略并
明确勾选信任；预览会验证候选签名、完整 Core 包内容与宿主兼容性，展示 SDK /
协议、所需和未提供的可选能力、权限目的 / 范围 / 程序、命令描述及执行端声明。
执行端声明仅作展示，本操作不安装或运行执行端。

若有旧活动选择，尝试以本次策略完整核验旧版并展示完整声明、能力增删和权限声明
差异。目的、范围或程序列表改变都视为声明改变；这些差异不推断代码行为或实际
授权效果。没有旧选择与旧版无法读取 / 验签是两种状态：后者明确显示查不出来，
差异保留未知，不写成「没有新增权限」。缺失或已撤销的旧版不会阻止用户审查一个
有效候选。旧选择的启用状态仍显示；确认选候选会将其设为启用的活动选择。

不按版本字符串大小自动决定升级或回滚；选择保留的旧版走同一预览与确认流程。
显示用 manifest 原始字节上限 64 KiB，其摘要必须与 Core 捕获的 manifestDigest
一致，不凭后来读到的同名文件展示权限。旧版未知时仍展示完整候选声明，用户须
自行判断是否继续。

预览持有候选目录的共享引用，阻止另一 Controller 回收它，但不会占着安装根锁
等待用户。取消、修改路径 / 信任、折叠或关闭页面会取消票据并归还引用；后台
取消未完成时保持页面忙碌，防止刚隐藏预览就启动下一项维护。重开不恢复票据。

只有勾选「我已核对候选声明与旧版本状态」后才能确认。确认重新读取所选策略并
比对原始 SHA256，检查原根 / 候选对象及选择 revision，再重新捕获、验签和检查
候选内容后原子更新选择。更换策略、其它 Controller 的选择变化或篡改候选都拒绝
旧预览；不把 UI 上旧的成功标志当成新授权。父同步失败仍单独报告持久性未知。

选择／重新载入／回滚都不继承来源批准或 Session 权限，不启动候选入口，也不迁移
已经运行的实例。旧实例仍持有原快照和目录引用；停止后才能回收旧版本。Native
编程调用的显式选择接口保留，但同样只更新选择、不授予运行权限。

```sh
# 默认 Core：真实 fd 身份 / 引用与注入 metadata 的数据专项
TMPDIR="$PWD/tmp" python3 -B scripts/test-plugin-selection-review.py --mutate --out tmp/selection/receipt.json
# JS-enabled Core、锁定工具与 gterm：实际签名包 / 后台控件 / 旧实例业务
TMPDIR="$PWD/tmp" python3 -B scripts/test-plugin-native-signature.py --tools tmp/tools \
  --selection-review --gterm tmp/gterm/gterm --out tmp/selection-enabled/receipt.json
```

此为 macOS 手工验收档；不提供自动更新、联网策略新鲜度、发布顺序 / 防回滚或
其它端兼容性保证。普通 App 仍默认关闭插件 JS 运行时。

## macOS 用当前策略复核与批量禁用

在「管理已安装版本…」提供独立可信策略的绝对路径并勾选信任，然后选择
「按此策略复核全部已启用选择」。这是用户选择的单个渠道策略快照，不联网查询
策略新鲜度，也不替代官方信任根或防回滚设计。其它渠道的版本可能因范围不符而
被此策略拒绝；应先看每项原因，再决定是否禁用。

结果分开显示：

- 「此策略接受」：当前签名、描述身份与 Core 捕获的包内容均匹配；不加载来源、
  不授予来源批准或 Session 权限，也不改变活动选择。
- 「此策略拒绝」：已撤销 / 未知密钥、策略范围、签名或已验证身份不匹配。
- 「查不出来」：缺少版本、读文件或包捕获 / 校验失败；不据此断言密钥已撤销。

所有勾选默认关闭。用户可以逐项选择禁用被拒绝或无法核验的当前选择，再确认
「禁用以上勾选项」。只改动这些插件的新建开关；接受项、未勾选项和已禁用项保持
原记录，不删除任何包，也不停止现有运行。现有运行若需撤销业务权限，应另外停止。
只读复核本身不会撤销或恢复已有来源批准。

确认前重核私有根身份、策略原始字节摘要及整个选择记录 revision，一次原子写入
所选禁用项并更新其授权代次。另一 Controller 的新建入口也会读取到禁用；重新
选择版本仍需重新核对策略并重新批准来源。策略文件 / 选择变化、根替换或旧票据
会拒绝确认，不悄悄扩展批次。写入效果与父目录持久性未知分开报告。

取消、折叠页面、修改路径 / 信任或重新复核会清空页面勾选；票据仅在当前 Controller
内有效，不跨重启保存，也不自动执行。损坏选择记录 / 策略整体拒绝，不以空列表
假装全部受信任。本入口不自动分发 / 持久采纳渠道策略、自动升级或撤销运行实例。

手工分开验证数据 / 页面与实际 Core / SSH Runtime：

```sh
TMPDIR="$PWD/tmp" python3 -B scripts/test-plugin-policy-review.py --mutate --out tmp/policy/receipt.json
# 已构建 JS-enabled Core、锁定开发工具与 gterm 后：
TMPDIR="$PWD/tmp" python3 -B scripts/test-plugin-native-signature.py --tools tmp/tools \
  --policy-review --gterm tmp/gterm/gterm --out tmp/policy-enabled/receipt.json
```

前者使用自有公钥签名声明和注入的 metadata 消费者，不据此称 Core 内容验证通过；
后者包含真实签名 ZIP、Core 捕获、跨 Controller 准入及运行中自有 SSH 夹具。

## macOS 归属未知目录的显式核对

「管理已安装版本…」另列私有安装根内的未知条目。旧 `.prepare-*`、未登记目录或
身份不匹配的同名目录始终标为「归属未知」，名称和本次观察都不能证明它来自安装。
已登记且身份匹配的活跃 / 遗留目录仍走前节流程，三类私有元数据名称不提供此入口。
顶层链接、文件、其它设备或归属的对象仅显示原因并保留。

用户可选择「逐项核对此目录的完整内容」。预览绑定本次根与目录身份，列出完整有界
路径、字节和文件摘要；链接、特殊文件、硬链接或超限内容拒绝整棵预览。取消保持内容；
再次预览须重新勾选「我已核对完整清单，并选择删除这个归属未知的目录」。确认前仍
检查启用选择、共享引用、目录身份与完整清单；内容变化需重新核对，不能沿用旧预览。

这次确认只建立针对当前对象的新删除决定，不补写旧安装归属、不认证来源、不发布
或运行插件。删除使用独立格式 3 意图，格式 1 已安装版本与格式 2 已登记暂存继续
可读。中断后只读观察不重放；尚未改名且原树完整时可显式撤销，开始移除后只能核对
同一意图再明确继续。新文件 / 替换对象保留，删除数量与已不存在观察分开。

旧的未确认归属记录不随手动删除而退役；目录消失后，该条记录仍需单独选择移除
元数据。损坏 ledger、保留的顶层非目录 / 私有元数据、完整磁盘故障、恶意同用户
并发写入与其它平台维护仍未完成，不据此宣称全部孤儿可回收。

```sh
TMPDIR="$PWD/tmp" python3 -B scripts/test-plugin-unclassified.py --mutate --out tmp/unclassified/receipt.json
```

脚本使用工程 tmp 内自有目录、后台原生控件及独立原生子进程，正常断言红与主动
SIGKILL 分开记账；不接触用户安装目录、用户插件或远端业务文件。

## macOS 实际进程死亡后的回收专项

已构建默认 JS-off Core 与本机 GhosttyKit 后，可运行：

```sh
TMPDIR="$PWD/tmp" python3 -B scripts/test-plugin-removal-crash.py --mutate --out tmp/plugin-removal-crash/receipt.json
```

脚本复制 Apple 包并编译，完全使用工程 tmp 内的声明夹具，不导入、批准或运行插件，
不需要 Node、签名私钥或 SSH 服务。夹具的发布者声明及占位签名不认证任何发布者。
原生子进程必须在指定检查点发回随机令牌、PID 和目录身份且仍存活，驱动才向该进程
发送 SIGKILL；另一个新进程通过真实 Controller 只读列表 / 待处理记录，并确认完整
树未被自动改写，然后明确继续或重新确认删除。原子记录前、记录后、改名后、首项
unlink 后及目录删除 / 记录清除前分别验证；进程死亡不当作业务执行成功或远端退出。

额外检查替换文件、未知成员、新的同名版本目录、已启用选择、安装根替换和损坏记录；
跨进程共享引用 / 回收预览在存活时阻止删除，进程被杀后内核释放锁，目录仍原样保留。
驱动自身的 SIGINT / SIGTERM 在已观察到原生子进程持有引用后测试，必须确认子进程
退出及其私有目录已清理。注入死亡事件、正常断言红和基础设施异常分别记账，不能
把预期 SIGKILL 计作变异红。`--case` 可选择普通场景，`--mutate` 仍执行六组保护验证。

本专项只证明本机 macOS / APFS 上的已登记版本文件维护与进程死亡，不证明断电、
恶意同用户并发写入、安装暂存孤儿、真实运行中跨进程切换、远端任务终止、GUI 重启
或完整文件系统 / 其它端 / 真机矩阵。旧受管 Runtime 与页面控件证据仍见前节独立专项。

## 共享渠道策略与 CLI 签名器

CLI 发布输出目录包含捕获的 `plugin.zip`、精确 UTF-8 `release.json` 和 64 字节原始
`release.sig`。Ed25519 签署 ASCII `GHOSTERM-PLUGIN-RELEASE-V1`、一个 NUL，再拼接
完整描述字节（含空白 / 末尾换行）；不把重新编码后的 JSON 当原签名消息。
描述绑定渠道、keyId、插件 ID / 版本 / 发布者、原始 integrity.json SHA256 和整个
ZIP 的 SHA256 / 字节数。CLI 通过 [Node crypto](https://nodejs.org/api/crypto.html#cryptoverifyalgorithm-data-key-signature-callback)
验证；不自写密码算法。schema 位于 `release-v1.candidate.json` / `channel-v1.candidate.json`，
由锁定 AJV 与宿主语义检查共同验证，仍是候选格式。

策略由调用者提供，不能从包内选择或导入。形状为
`{schemaVersion:1, channel, keys:[{id,algorithm:"ed25519",publicKey,publisher,pluginIds:[]}], revokedKeyIds:[]}`。
`publicKey` 是原始 32 字节公钥的规范 Base64，`id` 是这 32 字节的 SHA256，
不是 PEM / SSH 容器的摘要。每个 key 的发布者和 pluginIds 精确匹配，无通配或 ID
前缀授权。未知 / 已撤销 key、跨渠道、改描述、改 ZIP 或包内身份不一致均拒绝。
返回 `policySHA256` 和 `trustSource=explicit-policy-snapshot`；这只证明本次提供的
策略快照，不证明策略最新或具有防回滚能力，也不自动取得该发布者的联网身份。

签名器配置是 `{executable,sha256,prefixArguments:[],environmentNames:[]}`：绝对物理
可执行路径，执行前后核对字节摘要；只继承显式列出的环境名，并固定工程 TMPDIR。
它从 stdin 读取上述公开签名消息，只向 stdout 返回 64 字节签名，不加换行；15 秒
期限、64 字节输出和 4096 字节诊断上限，失败诊断不转发到日志。生产签名器须从
平台安全存储 / 已选 agent 使用私钥，本工具不接受 privateKey / 私钥文件参数，也不
读取 / 导出私钥。程序及其参数、脚本和动态依赖仍属于调用者的信任范围；前后摘要
不等于进程沙箱或完整执行依赖闭包。仓库只提供一次性、内存 KeyObject 的测试 agent，
没有生产 Keychain / Keystore signer。

CLI 验证先固定 regular-file 快照，再检查完整 ZIP32 布局：stored / raw DEFLATE、连续
成员范围、中央 / 本地头、CRC、压缩 EOF、条目数、单文件 16 MiB / 总量 64 MiB。
目录 / 链接 / 设备、路径越界、重复 / 大小写冲突、ZIP64 / extras / data descriptor /
注释 / 隐藏前后缀等不属于本候选容器。插件 metadata、完整性与兼容性复用原检查器；
包入口与 lifecycle hook 不执行。可附 `--host-json`，不兼容即失败；未给事实仍为
`compatibility=unknown`。成功始终 `installation=false` / `authorization=not-granted`。
输出目录原子、不覆盖地发布；这不是崩溃持久性或安装 / 升级 / 回滚验收。

手工专项：`python3 -B scripts/test-plugin-release.py --tools tmp/plugin-dev/tools --mutate --receipt tmp/release-receipt.json`。
只用自有目录 / agent / 隔离源码，真实签名与公钥不写私钥文件；变异先编译 Python，再
要求行为断言失败、独立 Core 配置绿、恢复绿及摘要。拒绝类型改变和错误接受分别记账，
不把编译 / 崩溃 / 超时当目标红。测试后清理本轮 tmp，不增加 CI 二十份。

## Android 发布签名核对后端

P6 第十一批提供 `PluginReleaseSignature` 和
`ExtensionController.verifyDirectorySignature` 的内部数据入口。调用者显式传入独立的
渠道公钥策略、原始描述及 64 字节签名；Controller 在自己的 worker 上读取已经
Core 检查的固定包 metadata，精确绑定插件 ID、版本、发布者和包摘要。证明记录
策略 / 描述的原始 SHA256，原目录后来被编辑不会改变已捕获的包身份。
核对不修改来源批准或 Session 权限。P6 第十二批已接 Android 原生签名材料选择与
策略确认页面；签名 ZIP 导入 / 安装、活动版本选择、显式回收及已登记安装暂存维护见后文；未知目录独立核对见后文，损坏归属元数据继续保留。

在显式启用插件 JS 的构建中，进入设置 → 插件包来源，选择文件夹完成审查后展开
「核对目录包发布签名…」。分别通过系统文件选择器选择原始 `release.json`、64 字节
`release.sig` 和独立可信的渠道策略。页面显示每份已捕获材料的实际字节数 / SHA256；
核对策略快照摘要并勾选确认后，才可请求验签。策略的新鲜度由用户所选快照决定。
同 URI 的提供方文件后来变化不会偷换已确认的快照；重新选择会读取新字节，策略
重选须再次确认。取消选择清除此槽位；更换任何材料、撤回确认、收起或离开页面
都会清除证明，旧异步结果不能恢复它。材料和确认只留在当前展开页面内存中。

读取只接受 `content://` 提供的只读普通文件 / 管道，使用 API29 兼容的已有描述符
适配器；检查 CLOEXEC，短 poll / 非阻塞读取后恢复状态标志，并关闭自有 fd。
按真实 EOF 检查上限与 64 字节签名，不用提供方声明的大小代替完整字节；可靠管道
报告错误时拒收前缀，普通文件读取前后核对 fd 状态戳。10 秒为合作提供方的读取 /
取消预算；忽略取消的外部 provider 不能保证硬期限。进程内共用的两个读取名额保留至
实际退出，避免关闭页面后未释放的读取继续累加。平台读取失败只显示固定错误，
不转发提供方任意异常文本；不持久化 URI 权限，也不在启动时重新读取这些材料。

共享 C ABI `gt_ext_release_verify` 只认证原始字节，输入公钥 32 字节、签名 64 字节、
描述 1..65536 字节；消息缓冲纳入既有插件共享内存预算，返回前擦除并归还。
它不解析策略或包，返回 `GT_OK` 也不授予执行。
`GT_ERR_AUTH` 表示签名不匹配，`GT_ERR_INTERNAL` 表示验证未完成；未启用的构建对
合法长度明确返回 `GT_ERR_UNSUPPORTED`。Swift / Kotlin / JNI 已同步绑定。
Android 通过现有锁定的 `android_crypto` 显式开启
`GHOSTERM_WITH_RELEASE_SIGNATURE`；交叉编译不得查找宿主 OpenSSL。
普通 host Core 默认关闭此开关，macOS 产品仍使用现有 CryptoKit 验证。
显式本机 conformance 构建可开启并链接 OpenSSL Crypto；新增后端使用
[OpenSSL 的 Ed25519 单次验证接口](https://docs.openssl.org/3.0/man7/EVP_SIGNATURE-ED25519/)，不自行实现签名算法。

Android v1 解码拒绝重复 / 转义重复 / NFC 同名字段、prototype 字段、额外字段、
非规范 Base64、错误公钥摘要、BOM、畸形 UTF-8 / surrogate、未转义控制字符及数字
舍入。限制策略 128 KiB、描述 64 KiB、8192 节点、16 层；JSON 精确整数语义与
macOS 相同，`1.0` 可表示版本 1，舍入后才等于 1 的小数不接受。
未知密钥、已撤销密钥、范围拒绝、签名拒绝、包身份不符和验证能力未知分别报告。

手工复验使用本地已安装 API 29 / 36 arm64 镜像，不下载系统镜像：

```bash
python3 -B scripts/test-plugin-release-android.py --headless-api 29 --signature-ui
python3 -B scripts/test-plugin-release-android.py --headless-api 36 --signature-ui --mutate
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  python3 -B scripts/mutation-check-release-signature.py
```

前两条明确构建 JS-enabled 测试 APK；`--signature-ui` 组合在既有 42 项真实 JNI /
策略 / Core 包 / Controller / 配置上，加入八项真实提供方读取、七项草稿调度、一项
MainActivity + 系统 DocumentsUI 整链及四项既有描述符适配器。`--mutate` 配合此选项
运行页面 / 材料读取保护；不带 `--signature-ui` 时仍运行原 42 项和旧后端变异。启动器关闭窗口及崩溃报告，显式
设置工程内 Android 临时 / AVD / 发现目录，确认日志后才测试，等待所属进程退出再
删除镜像。已有明确授权的设备可改用 `--android SERIAL`，不自动选择用户设备。
Core 变异脚本要求显式可用的 `pkg-config libcrypto`，只在工程 tmp 编译隔离生产副本，
不改变 host 默认开关。测试资产只有公开描述、公钥、签名及合成目录包，无私钥。

### Android 签名 ZIP 预检后端

P6 第十三批提供内部 `ExtensionController.inspectSignedArchive`。输入为调用方已捕获的
可定位只读普通文件句柄及三份明确选定的签名材料；本入口不读取 URI / 网络。
Controller 拥有 fd 副本，先认证描述 / 策略，再检查签名描述所绑定的归档长度和
SHA256。原始 `verifyDirectorySignature` 仍在 Core 已检查目录身份上核对。

`zip-v1` 与 macOS 使用同一有界档：仅单磁盘 ZIP32、stored / raw DEFLATE、普通
文件成员；拒绝目录 / 链接 / 设备、加密 / data descriptor、ZIP64、extra / 注释、
未声明范围及中央 / 本地头不一致。归档至多 72 MiB、1024 文件、单文件 16 MiB /
总解压 64 MiB，路径为 NFC UTF-8、4096 字节 / 32 层；拒绝越界、重复、大小写或
文件 / 父目录冲突。用 `pread` 分块访问，保持借用 fd 的位置，每次只解压一个成员，
核对 CRC、流终点与准确输入 / 输出字节，检查期间变动即拒绝。取消检查位于读块 /
解压 / 写入循环，复制和解压完成不代表 Core 身份已接受。

完整内容仅写入本次新建的应用私有缓存目录，Core 捕获 / 检查后，再比较描述中的
插件 ID、版本、发布者及包摘要。成功返回审查数据与证明；临时树、Core owner 和
自有 fd 在退出时释放，失败清理会明确报错。本入口不写来源注册表，不保留安装，
不创建 Session 或执行包 / 安装 hook。该缓存处理以本应用私有目录内的单次 owner
为边界，不能替代未来持久安装的身份 / 原子发布 / 归属日志或同 uid 恶意并发防护。

手工复验在上面的设备命令增加 `--archive`；它增加十八项 ZIP / 生命周期与五项
真实 Controller / Core 用例，可与 `--signature-ui` 同时运行。`--archive --mutate`
执行归档 / 身份 / 生命周期变异；旧页面与旧签名后端变异仍分别用原选项调用。
`scripts/fixtures/plugin-release-archive.mjs` 从已有合成目录生成公开的 stored /
DEFLATE、身份不符、完整性不符及 CRC 错误向量；一次性 Node Ed25519 私钥只留在
生成进程的 KeyObject，输出仅有归档、公钥策略、描述和签名。重新生成须显式运行并
审查资产差异；生成器不参与产品构建或运行。

### Android ZIP 导入与原生预检

在显式启用插件 JS 的构建中，设置 → 插件包来源 →「检查签名 ZIP…」分别选择 ZIP、
`release.json`、`release.sig` 和独立取得的渠道策略。页面展示实际捕获字节数 / SHA256，
确认具体策略快照后才可检查。结果展示签名发布者 / 渠道、包与清单摘要、许可证、
SDK 声明 / 本机选择、协议声明 / 可用交集、全部权限目的 / 程序，以及完整 Executor
与依赖声明。后者是包声明，实际远端执行环境与依赖仍须另行核对。

ZIP 从只读 content fd 流式复制到应用私有缓存，每块 16 KiB、上限 72 MiB，按
实际 EOF / 可靠管道错误 / 文件状态戳检查，不采用提供方声明大小。60 秒为合作
提供方的读取 / 取消预算，忽略取消的 provider 没有硬期限保证。ZIP 与三份小材料
共用进程内两个读取名额，直到实际 IO 退出才归还。成功捕获后只保留无文件名的
只读 fd；取消返回结果的调度同样关闭未交付的 owner，borrow 副本各自持有生命周期。
页面上的重新选择、收起、离开会清理材料 / 证明与 ZIP owner，不持久化 URI 授权。

检查使用独立 Controller worker 及原主机能力快照，不占用正在运行的 Client pump。
权限和 Executor 展示只取本次归档的原始 `plugin.json`，先匹配 Core 给出的原始
manifest SHA256，再做有界严格 JSON / 类型解码；不会从提供方重新读同名清单补值。
单独检查只生成本次预检回执，不登记来源或创建 Session。保存到本机另需下节的独立确认。

设备专项：`python3 -B scripts/test-plugin-release-android.py --headless-api 36 --archive-ui --mutate`。
`--archive-ui` 包含原 `--archive --signature-ui` 组合，并增加 ZIP 捕获、草稿、声明、
独立 Inspector 与真实 MainActivity / DocumentsUI 用例；旧后端 / 页面变异仍用原选项。
生成完整声明的公开测试向量可显式运行
`node scripts/fixtures/plugin-release-archive.mjs --declarations` 并将 stdout 保存到测试资产。
该开关沿用只在进程内持有私钥的生成器，不参与产品运行或放宽原向量。

### Android 签名版本安装

完成 ZIP 预检并审阅发布者、完整权限 / 程序与依赖后，勾选独立的「我已审阅上述
发布者、权限和依赖，确认保存此版本到本机」，再点「安装此版本」。策略信任和安装
确认是两次不同决定。重选材料、改变策略或重新检查都会清除安装确认；失败后重试
也须重新确认。安装期间材料选择与两项确认冻结，收起页面会取消尚未发布的工作。

安装使用已捕获的只读 ZIP fd 与三份材料快照，再次验证签名 / 策略和完整预检。
目标为应用私有 `files/plugin-releases/<release.json 原始字节 SHA256>/`：`payload/`
保存 Core 再次核对过的完整包，另存公开 `release.json`、`release.sig`、`policy.json`
证据。后者不自动成为未来的信任根。普通文件 0400、可执行文件与目录 0500；同一
发布槽位已有文件、空目录或链接时明确拒绝，不覆盖，也不预先删除。

目标根目录持独立非阻塞锁；同步其父目录后，先同步 `.install-<UUID>.json` 预约，
再建同文件系统的 `.prepare-<UUID>`。预约登记当前根目录及暂存目录的 dev / inode /
uid。每次核对持有的 fd 与命名对象，完整检查已知树后冻结，只以一次
`renameat2(RENAME_NOREPLACE)` 原子发布。平台 / 文件系统不支持时显式失败，不复制
或改用覆盖 rename。开始安装时根目录已有对象须少于 64 项，未知对象也计数；本次
意图 / 暂存及记录原子替换最多另占 3 个根条目，不因名称前缀而删除。

发布前的取消 / 失败仅清理本次仍能确认身份及完整内容的暂存和意图；发现替换、
缺失、未知文件或记录变化时保留待核对。发布后不回滚：目录同步失败仍显示「版本
已发布 / 持久性未确认」，并保留意图；清理意图失败单独展示。回执仅描述同步调用
结果，不等于断电 / 磁盘故障证明。进程重启不会自动收养、安装、重放或清理残留。
这些运行时 fd 身份和意图不是跨 inode 复用的永久归属证明，也不是恶意同 uid 的
操作系统隔离。残留列表 / 显式维护与更强的持久归属仍需后续切片。

安装只保存本机版本，不登记来源、不授予 Session、不启动 Runtime、不部署远端，
也不选择活动版本。本机版本观察、显式选择 / 禁用 / 回滚、旧实例引用、显式版本回收及已登记安装暂存维护见后文；未知目录可独立核对，损坏归属元数据继续保留。
默认发行 App 仍关闭插件 JS，本入口仅用于显式启用的开发构建。

```bash
# 两档本机已有镜像；均为自有无窗口 AVD
python3 -B scripts/test-plugin-release-android.py --headless-api 29 --install
python3 -B scripts/test-plugin-release-android.py --headless-api 36 --install --mutate --death
```

`--install` 包含原 ZIP / 签名正常基线，增加目标文件系统事务、JNI 原子目录发布、
实际 Core 安装、确认草稿与真实 SAF 页面；旧归档 / 提供方变异仍用原脚本。
`--death` 仅允许驱动自有 AVD，分别在六个实际检查点结束目标进程，随后实际启动
MainActivity 检查没有自动重放，并以新策略输入核对已发布树；进程死亡单独记账，
不充当正常变异断言红。精确通过范围、前置失败与输入摘要见插件计划第十五批。


### Android 已保存版本观察与选择记录

在启用插件 JS 的开发构建中，设置 → 插件包来源 →「查看已保存的本机版本…」
读取安装根目录；「刷新版本列表」重新读取，收起会取消尚未交付的观察。该操作
不创建安装根目录、不采集或加载包、不采纳来源批准，也不回放保存的选择记录。
观察在独立 worker 上进行，当前 Client 的 pump 和固定包 owner 保持独立。

列表显示发布描述 SHA256、声明的插件 ID / 版本 / 发布者及目录包摘要。这里只
核对只读文件、描述字节与目录名的一致和有界结构，尚未用当前可信策略和 Core
重新核验；完整签名 / 权限核验仍是实际运行前必须完成的独立步骤。已保存策略
只作为公开证据读取，不自动成为新信任根。损坏版本单列问题，非摘要条目只计为
暂存 / 未识别对象，不按前缀删除、收养或恢复。

活动选择显示为「记录为启用 / 禁用」；这不代表当前进程已加载、来源已批准或
Session 已授权。已知无记录与读取失败分开：损坏 / 链接 / 可写的选择文件显示
「查不出来」，不会冒充没有选择；引用缺失版本的记录也保留可见。观察和刷新本身
仍为只读；显式切换与禁用按下节的独立确认流程操作。

底层选择存储使用 `selection-v1.json`，严格格式 1 / UUID nonce / 最多 64 项，
每项为 releaseID / enabled / generation。根与安装共用短期非阻塞排它锁；写入
合并其它插件选择，并为本次插件产生新 generation / 根 nonce。以原始文件 SHA
作为修订号，创建临时文件前和提交前均核对期望修订；旧视图及合法 A→B→A 不能
重新使用原代次。原子替换前同步根入口和已关闭 writer 的 0400 临时文件，精确
核对根与候选身份 / 状态戳；变化的候选保留供核对。发布后同步失败返回已发布且
持久性未确认，不回滚。这是存储事务，不是包认证或运行准入消费者。

版本目录另有显式共享引用组件，独立 fd 读者可共存，借用副本保持同一共享锁直到
最后一个 fd 关闭。组件不按路径名称撤销引用；预览 / Session / Runtime 消费见下节，
显式版本回收见后文。当前没有自动维护、恶意同 uid 隔离或真实断电保证。

```bash
python3 -B scripts/test-plugin-release-android.py --headless-api 29 --catalog
python3 -B scripts/test-plugin-release-android.py --headless-api 36 --catalog --mutate
```

`--catalog` 包含原 `--install` 正常组合，增加记录事务、实际内核共享引用、观察
草稿、真实 Core 安装重开及实际设置页；新变异只验证本批保护，不泛称旧变异全部
重跑。精确平台范围、输入 / APK 摘要与其余限制见插件计划第十六批。

### Android 活动版本选择、禁用和回滚

展开本机版本列表后，先选择本次独立可信的渠道公钥策略，并勾选「我已独立核对并
信任此策略快照」，然后点击目标版本的「核验并选择此版本…」。安装时保存的策略
只是证据，不会替代本次信任决定。操作重新验签并用真实 Core 核对实际包，预览中
显示完整候选声明、命令、参数与 Executor / 依赖，以及能用本次策略核验的旧版和
权限差异。旧版无法核验时明确显示未知，不能据此认定没有新增权限。

核对后单独勾选本次切换确认，再点击「确认切换活动选择」。选择保留的旧版即显式
回滚，使用相同的核验和确认流程；不按版本号大小自动决定方向。取消、换策略、
撤回信任或离开页面会作废预览。另一窗口 / 控制器已改变选择时，旧预览不能覆盖
新选择，需要刷新后重新确认。

切换成功会回到「尚未批准」：仍须独立批准准确来源摘要、完成 Session 授权，才可
运行新实例。即使再次选择同一版本，也会产生新的批准代次。已安装 ID 没有有效
选择、被禁用或代次 / 文件身份已变化时，新实例不能退回同名开发目录绕过检查。
损坏、不可读及根目录缺失与明确禁用分开处理。

「禁用此插件的后续新实例…」有独立确认，不要求重新信任或执行旧包；该入口也可
用于阻止记录所引用的版本已丢失时继续新建。已有 Session 与 Runtime 保留创建时
的固定包和版本共享引用，切换或禁用不会把它们换成新包，也不会代替用户停止它们。
实际结束并释放实例后才释放对应引用；本机版本回收与已登记安装暂存维护见下节，未知目录的独立核对见后文，损坏归属元数据继续保留。

写入后的目录同步失败会显示「持久性未确认」，已写入的选择保持原状；回执取不到
时请刷新核对，不以没有收到回执推断操作没有发生。应用启动和列表刷新仍不会
自动载入、授权、运行、远端部署或重放保存的选择。默认发行构建仍关闭插件 JS。

```bash
# 使用本机已安装镜像和已构建的 core/build/test_ext_rpc；只启动自有无窗口 AVD
python3 -B scripts/test-plugin-release-android.py --headless-api 29 --selection-channel
python3 -B scripts/test-plugin-release-android.py --headless-api 36 --selection-channel --mutate
```

`--selection` 在既有安装 / 观察组合上增加当前策略、真实 Core、升级 / 禁用 / 回滚、
来源与批准代次、取消和实际设置页；`--selection-channel` 另验两个控制器及同控制器
注册替换时，旧 Session / Runtime 通过两跳隔离 SSH 完成双向 RPC、取得独立远端
完成回执，核对目录引用的保留和释放。具体执行范围及前置失败见插件计划第十七批；
不以此代替真机、16 KiB、完整 provider / 辅助技术或全客户端验收。


### Android 本机版本删除与中断后继续

展开本机版本后，确保目标版本未被启用；已有启用选择需先禁用，运行中的实例仍需显式停止并实际
释放。点击「核对并删除此本机版本…」会读取完整目录清单，包括相对路径、文件
字节数及 SHA-256；预览不会写删除记录或删除文件。另一项预览或运行引用仍在
使用目标版本时，会拒绝预览删除。取消或离开页面会释放预览引用。

核对清单后，单独勾选本次删除确认，再点击「确认删除此本机版本」。删除前会再次
核对目录和活动选择，并先保存、同步删除意图，再原子移开原目录并逐项删除已核对
的成员。其它未识别目录和禁用选择记录保留；删除不会批准来源或新 Session，
也不会自动改选同名开发目录。回执分别显示本次实际移除和观察到此前已不在原位
的项数，目录同步失败仍显示持久性未确认。

若中途停止、磁盘写入失败或回执丢失，请刷新核对。打开页面和刷新只读取保留的
删除意图；核对保存的完整清单并另行确认后，才能点击「明确继续此删除意图」。
剩余目录身份、文件内容或成员无法核对时，会保留剩余内容。尚未进入移除阶段的
意图可另行确认撤销；已移开目录后会明确拒绝撤销，需继续核对处理。待处理意图
存在时，暂停新的版本安装与启用，仍可显式禁用。

跨重启核对使用文件系统提供的持久对象身份：Android 10 使用 inode generation，
较新系统使用创建时间；不支持必要元数据或不覆盖原子移动的文件系统会明确拒绝。
该机制不授权按目录名前缀收养或删除安装暂存、未知孤儿及其它文件。默认发行构建
仍关闭插件 JS，不自动联网、运行插件或安装远端环境。

```bash
# 只使用本机已有镜像；包含完整原生页、真实文件与显式继续/撤销组合
python3 -B scripts/test-plugin-release-android.py --headless-api 29 --removal
python3 -B scripts/test-plugin-release-android.py --headless-api 36 --removal --mutate
# 自有无窗口 AVD 内另造实际硬链接；不在用户设备上执行 su
python3 -B scripts/test-plugin-release-android.py --headless-api 29 --removal-hardlink
# 在 --removal 基础上另验真实两跳 SSH 及旧开发实例关联安装版本的引用
python3 -B scripts/test-plugin-release-android.py --headless-api 36 --removal --selection-channel
```

进程内重开 Controller 与实际杀进程、断电、所有文件系统和物理设备矩阵分别记账；
具体范围见插件计划第十八批，不以本机专项替代完整客户端验收。

### Android 安装暂存归属维护

展开「已保存的本机版本」后，「安装归属维护」显示安装留下的公开元数据。只读刷新
区分活动引用、已登记遗留暂存、同一对象已在目标位置、登记对象已不在原位、未确认
归属和已变化 / 无法读取的记录。已在目标位置只表示对象位置核对，不等于包认证、
安装结果已确认或来源授权。活动引用未释放时不提供维护；未确认与已变化的对象
保留，不能凭 `.prepare-` / `.install-` 前缀采纳。

已确认的遗留暂存可点击「核对并删除此安装暂存…」，先查看完整文件清单、字节数及
SHA-256，再单独确认。部分写入的私有文件也按当前完整快照核对；该允许仅用于已
登记暂存，不放开已安装版本的只读要求。确认前 / 继续时重核对原始归属记录摘要和
目录持久身份。删除意图使用格式 2，保留准确的归属引用；先同步意图，再不覆盖地
移开目录、逐项核对删除，最后退役原归属记录。已存在的冲突目标保持原样。

同一对象已在目标位置、或登记对象已不在两处记录位置时，可另行选择「只退役这条
归属记录…」，独立确认后仅移除准确的元数据文件。它不删除版本或未知对象，不加载
来源、授予 Session 或改选活动版本。途中回执缺失或同步失败请刷新核对；打开页面和
重开应用不会重放、继续安装或自动删除。目录已移除但归属 / 删除意图清理未完成时，
仍需明确继续，不据未收到回执推断没有发生副作用。

新安装先保存格式 2 预约，创建目录后登记 dev / inode / uid 及创建时间或 inode
generation，并持有该暂存自己的独占引用；未登记身份前不写包数据。预约与登记之间
中断留下的同名目录仍为未确认，旧弱身份记录也不自动升级。安装暂存自身和元数据
归属核对，不替代当前可信渠道策略、真实 Core 包核验和运行授权。

```bash
# 本机已有镜像，自有 no-window AVD；仅安装/维护应用的私有测试夹具
python3 -B scripts/test-plugin-release-android.py --headless-api 29 --maintenance --death
python3 -B scripts/test-plugin-release-android.py --headless-api 36 --maintenance --mutate
```

实际进程死亡专项包括预约、mkdir 后尚未登记、登记、文件创建 / 写完、改名前 / 后、
发布同步后及暂存首项删除后；未知归属场景只证明保留，夹具退出时的测试清理不能算
下节独立未知目录删除已通过；下节的新决定另行验收。范围、源码 / 构件摘要与变异证据见插件计划第十九批。

### Android 归属未知目录的独立核对

「归属未知的目录与条目」列出私有安装根中未进入正常版本或已登记暂存流程的对象。
名称或存在不代表安装来源。已能读取声明的版本、身份匹配的已登记暂存继续走各自
入口；选择、删除意图、安装归属及其临时元数据名称不提供这个入口。顶层文件、
链接或非本应用私有目录只显示原因并保留；活动选择或安装归属损坏时报告查不出来，
不借错误取得删除权限。旧弱归属元数据仍需独立维护，不自动迁移或退役。目录名称须为 NFC、单个不超过
255 字节的安全路径分量；保留名称及不符合名称条件的对象不提供本入口。

可预览目录先绑定当前父目录身份、准确名称与目录 stat 的观察令牌，再持有独占 fd
读取完整有界清单。完整清单包含子目录、每个普通文件的字节与 SHA-256；不跟链接、
不跨设备，不接纳特殊文件、多重硬链接或超限内容。目录和部分写入文件必须仍属
本应用私有对象。目录已变化或仍有引用时拒绝旧预览，请重新核对。

确认前须同时勾选完整清单确认和额外的「我了解归属未知」确认。取消后重新预览、
或明确继续旧意图时，两项都要重新确认。已启用但声明损坏的目录仍须先明确禁用，
不能从未知入口绕过；删除途中再次发现启用也会保留剩余内容。

本次确认建立对当前完整对象的新删除决定，格式 3 意图绑定原名称及名称 SHA-256，
先持久化，再不覆盖地移开并逐项核对删除。它不补写安装来源、不加载或运行内容，
不退役旧安装记录，也不改选活动版本或清空版本来源缓存。尚未移除且清单完整时可
明确撤销；部分删除后只能核对同一意图并明确继续。已不存在观察、本次实际删除和
同步未确认分开报告，打开、刷新与应用重启均不自动重放。

旧的未确认安装记录在目录被手动删除后可只观察到已不存在；记录摘要保持，随后
退役仍是另一项独立决定。顶层非目录、损坏私有元数据、旧弱归属和完整恶意同 uid /
断电 / 文件系统矩阵不在这个目录入口内。

```bash
# 自有 no-window 模拟器；组合含原有签名/版本/维护回归
python3 -B scripts/test-plugin-release-android.py --headless-api 29 --unclassified --removal-hardlink --selection-channel --death
python3 -B scripts/test-plugin-release-android.py --headless-api 36 --unclassified --removal-hardlink --selection-channel --mutate
```

具体设备、构件和断言 / 实际进程死亡证据见插件计划第二十批；不以本机专项代替
正式发行、真机或完整客户端验收。

### Android 用当前策略复核与批量禁用

在实验插件的已保存版本区域独立选择渠道策略，核对材料摘要并明确确认信任，再点
「按此策略复核全部已启用选择」。复核使用本次从文档提供方读取并保留的字节快照；
确认时重核同一快照摘要，不重新读取 URI，不从版本内 `policy.json` 自动继承信任。
无启用选择时也校验完整策略。单个策略不代表所有渠道，接受也不授予来源或运行权限。

结果分别显示此策略接受、此策略拒绝和查不出来。每个版本临时持有引用，重新验签并
交真实 Core 核对包内容和身份；无法完整读取 / 核验不记作已撤销。检查在独立 worker
执行，活动 Runtime 的 worker 继续处理原会话。复核不注册包、不自动运行或修改选择。

默认全部不勾选，只有拒绝 / 未知行可显式选择。确认只禁用本次勾选的后续新建，
一次原子替换活动选择记录，并为这些选择生成新代次；接受、未勾选和原已禁用项保持。
确认前重核安装根身份、策略原始摘要和整个选择修订；其它 Controller 的变更也使
旧决定失效。更换材料、撤回信任、取消、关闭或晚到结果不能保留旧勾选和确认票据。

已有 Session / Runtime 继续使用已批准快照并持有版本引用，需另行停止后才能回收。
未取得完整回执或同步未确认时请刷新核对，不盲目重复提交。此入口没有联网新鲜度、
生产信任根分发、防回滚或运行中全局撤权保证。

```bash
# 自有 no-window AVD，启用 JS 的测试 APK；复核不触及用户远端主机
python3 -B scripts/test-plugin-release-android.py --headless-api 29 --policy-review --removal-hardlink --selection-channel
python3 -B scripts/test-plugin-release-android.py --headless-api 36 --policy-review --removal-hardlink --selection-channel --mutate
```

本批具体验证范围和原始收据摘要见插件计划第二十一批；不代替发行、真机、16 KiB 页
或完整客户端验收。

### Android 选择版本前的能力声明差异

已保存版本的切换预览现在单独列出 Client 必需能力声明的新增与移除，保留声明顺序，共同
能力不会出现在增删列表。首次选择时全部候选必需能力列为新增；旧选择即使已禁用，
只要能经本次策略和真实 Core 核验，仍参与完整比较。回滚按声明比较，不按版本字符串
自动判断先后，也不把变少解释为更安全。

旧版缺失、验签失败或无法完成核验时，能力与权限差异明确显示查不出来；不把未知
当空集合或「没有新增」。仍展示完整候选权限用途 / 范围 / 程序、SDK / 协议、必需 /
未提供的可选能力、命令与执行端 / 依赖声明。可经独立确认选择有效候选替代坏旧版，
来源批准与新 Session 授权继续分开。

```bash
# 公共签名测试向量；私钥仅在生成进程内，输出不要覆盖既有 versions.json
TMPDIR="$PWD/tmp" node scripts/fixtures/plugin-release-archive.mjs --capability-versions > tmp/capability-versions.json
# 自有 no-window AVD；包含原签名/安装/选择/维护/当前策略组合
python3 -B scripts/test-plugin-release-android.py --headless-api 29 --version-changes --removal-hardlink --selection-channel
python3 -B scripts/test-plugin-release-android.py --headless-api 36 --version-changes --removal-hardlink --selection-channel --mutate
```

默认仍为 JS-off。具体模拟器 / 构件、声明比较与真实 Core 的不同证据范围见插件计划
第二十二批；本入口不审计代码行为，也不安装执行端依赖或迁移已有运行实例。

## 执行端离线签名安装

独立 `gterm plugin install` 可在用户选择的执行端安装并登记已签名 ZIP；
`register-signed` 可重新核对、登记已有发布副本。需显式启用签名构建、选择可信渠道策略，
并分别配置 Portable / Node / Exec 的本次启动范围；不会安装 Node 或业务程序。
副本发布、目录登记与同步结果分开，失败后先观察再操作。命令、策略变化后的拒绝、
多 variant 选择和当前验证边界见 [CLI 安装说明](../../cli/gterm/README.md#离线签名执行端安装实验)。


## 从原生启动表单核验执行端签名版本

macOS / Android 的实验插件启动表单可选择「核验执行端已安装版本的签名」。填写
执行机器上已有的版本目录（含 `release.json` / `release.sig`）、信任策略文件绝对路径
和独立核对的策略 SHA-256；Executor 包目录填写对应的 `payload/`。再明确允许本次使用
该策略。SSH 主机认证、Client 来源批准、文件 / 进程范围和 Session consent 各自独立。
更改执行目标、证据、包摘要或启动配置后，须重新允许使用策略；新字段不读取本机
Client 安装策略，也不复制或安装远端文件。

普通启动、同连接启动组和在线添加都携带每个实例自己的证据。连接组的「查看配置」
会展示版本目录、策略路径 / 摘要与本次允许状态；不同实例可以选择不同发布版本或策略。
执行端复用现有 `gterm --release-evidence` 检查，固定包摘要仍单独传入；会在进入会话和
激活前核对签名、发布者、插件版本、包摘要与策略快照，并保留版本引用。签名构建不可用、
材料缺失、策略变化或签名不匹配均报错，不自动退回开发目录或切换执行环境。

这只适用于创建新 Executor。已有前台附着继续沿用原进程启动配置，不能在附着时添加
签名证据或更换策略。普通构建仍 JS-off；完整远端签名 / 混合运行档交互、迟到 / 断线、
设备与发行矩阵继续按插件计划集中验收，不能把表单编译或参数对拍当成这些验证。


## 固定原生启动的 Executor variant

两端新建 Executor 的启动表单提供「Executor variant ID（可选）」。填写执行端 manifest
中准确的 `executor.variants[].id`，例如 `native_linux-2.1`；ID 为 1–128 位 ASCII 字母、
数字、点、下划线或连字符，不修剪或改写用户输入。留空表示没有固定选择，执行端仍会
检查候选是否唯一；即使只安装了一种运行时，多候选也会拒绝，不能据 Node 是否存在
或声明顺序自动选择。

此 ID 只选择 Executor 变体，不替代 Portable / Node / Exec 运行档、平台、依赖、签名、
文件 / 进程范围或 Session consent 的检查。修改选择须重新确认相关范围；启动组的
配置审查展示每项选择，在线添加保留新实例自己的选择，已有实例保持原快照。
已有前台附着不能在连接时修改 variant，切到附着模式会清除这份新建选择。

CLI 的相同选项为 `--variant ID`，可与 `--release-evidence` 组合。开发期可在独立启用
JS / 签名的 CLI 构建上执行以下专项（测试全部使用工程内一次性包和内存签名夹具）：

```sh
python3 scripts/test-gterm-release.py --build 工程tmp下的CLI构建 --node 明确的Node绝对路径 \
  --sdk 工程tmp下的可信NodeSDK --case exec-same-multi --case node-same-multi
```

两场景各放入两个同运行档 / 同平台候选；没有选择时应以明确错误拒绝，指定选择后
通过真实签名安装、前台 IPC、Host Session / consent / RPC 和释放。未选 Node 入口会
抛错，未选 Exec 入口会以 83 退出，不用成功的占位处理器代替所选业务。
这不代表原生 App 的系统表单点击、远端 SSH 或混合多实例矩阵已完成。


## 验证 macOS 同连接混合运行与资源隔离

`scripts/test-plugin-mixed-native.py` 创建带 Portable / Node / Exec 三个 variant 的一次性
RPC / 文件资源夹具，通过实际 AppModel 的连接组消费；不操作用户包、主机、账号或设备。
CLI 构建须显式启用 JS、SSH 和 release signature，SDK 使用锁定工具构建：

```sh
mkdir -p tmp/plugin-mixed
export TMPDIR="$PWD/tmp/plugin-mixed"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
node build/plugin-sdk-tools/node_modules/typescript/bin/tsc \
  -p packages/plugin-sdk/tsconfig.json --outDir tmp/plugin-mixed/sdk
cmake -S cli/gterm -B tmp/plugin-mixed/cli -DCMAKE_BUILD_TYPE=Debug \
  -DGHOSTERM_WITH_PLUGIN_JS=ON -DGHOSTERM_WITH_LIBSSH2=ON \
  -DGHOSTERM_WITH_RELEASE_SIGNATURE=ON -DGHOSTERM_BUILD_TESTS=OFF
cmake --build tmp/plugin-mixed/cli --target gterm -j 4
node scripts/build-plugin-node-sdk.mjs --tools build/plugin-sdk-tools --out tmp/plugin-mixed/node-sdk
build/ssh-channel-toolchain/bin/python scripts/test-plugin-mixed-native.py \
  --build tmp/plugin-mixed/cli --node 明确的Node绝对路径 --sdk tmp/plugin-mixed/node-sdk \
  --out tmp/plugin-mixed/native --mutate
```

`--out` 必须是工程 tmp 下的新目录；省略 `--route` 运行本机、直连和两跳，也可重复指定
所需路线。`--mutate` 在隔离 Swift 副本撤回批处理内的父连接推进，要求真实编译后单条
正常断言红、独立 HostList 配置绿、恢复绿与对象 / 源码 SHA256。失败会保留本次日志和
副本供诊断；任务结束后核对活动引用并清理本次目录。重复开发可用 `--project` 明确选择
本工具带标记的复制工程缓存，源码仍从根工程刷新并比对，不覆盖未标记工程。

每路线有八个独立审批的实际实例，交替使用开发目录与已签名安装版本。各实例返回
自己的 RPC 回包和 256 KiB 资源，核对完整字节 / 摘要；关闭一项后注入错误策略摘要，
其它七项继续 RPC，再在线添加已签名 Node，全关后复用原连接创建已签名 Exec，最后
确认 finish / EOF / 本机 reap 与暂存为空。发布操作使用显式 10 秒期限；测试 Exec 的
`GT_DEV_RESOURCE_TIMEOUT_MS=10000` 只配置本次夹具，原默认仍为 1000，不调整生产
CPU / 内存 / 流控上限。源码 / 实际构件 / 日志收据留在 `--out/receipt.json`。

本专项不启动 Android 或系统选包器，也不代表官方业务插件、并发资源压力、完整
断线 / OOM / 性能 / 设备矩阵通过。普通 App 的 JS / 签名开关保持原配置。
