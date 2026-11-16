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
编辑通知提交。默认 `--route both` 共六条链：Markdown + PNG、独立 PNG、HTML +
CSS / PNG 各走直连与 ProxyJump，核对实际主机密钥提示的端口顺序，拒绝只看配置
就当双跳已执行。`--route direct` / `jump` 各运行对应三条链，缺夹具或跳过项判失败。
每份真实收件核对字节 / SHA256；Markdown 与图片取原生绘制像素，HTML 则对页面实际
嵌入的 WKWebView 取快照，核对 CSS 蓝背景与 PNG 红像素，并点击附件图片后切回
HTML，最后停止并检查资源控件及嵌入 WebKit 移除。

窗口在显示前已透明、离屏并禁止应用激活。目录选择由显式测试依赖返回，默认产品
仍使用系统 `fileImporter`；本专项不证明系统文件面板、VoiceOver、用户键盘焦点、
其它设备或完整生命周期，也不把已有原生像素专项当作多格式 / 多服务器全矩阵。

资源组件另跑 14 项基线。HTML 的实际嵌入视图还必须采用每视图独立的非持久
WKWebsiteDataStore；切换到图片再返回时使用新 store。测试刻意持有已移除和停止后
的 WKWebView，检查 delegate 解绑，并通过原 scheme handler 发起附件请求确认拒绝。
这证明 Native owner 撤销未来读取，不等同于任意并发回包 / 物理内存擦除或系统缓存取证。

`--mutate` 默认 21 组：原九组摘要展示、来源批准、启动、会话批准、停止、Markdown
图片、独立图片、HTML 附件与资源选择，以及十二组 WebKit 存储 / 页面绕过 / 拆除 /
Coordinator 终态、幂等、stopLoading、delegate、附件撤销和 handler 关闭 / 晚更新保护。
`--mutation-case 名称` 仅选择本次组（可重复），必须同时指定 `--mutate`；基础六条
或对应三条链仍运行，收据逐项记录实际组及选择的测试，不能以选组称完整 21 组。
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
