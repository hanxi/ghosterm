# 开发与验证插件

`scripts/gterm-plugin` 提供候选 `init/check/build/pack/test` 命令，当前构建对象为
Portable Client 加 Portable / Node / Exec Executor 运行档。命令读取固定副本，不执行项目的 npm
脚本、安装钩子或插件入口；只有 `test` 会在真实 Core / QuickJS 中激活选定的一侧。
默认测试 Executor，`--side client` 测试 Client，`--side paired` 同时激活两侧。
`test` 默认只激活 Portable；显式执行配置可用于单侧 Node / Exec Executor，或
Portable Client 与 Node / Exec 的双侧测试。文件 / 进程 / UI 服务测试仍待接入。

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
错误输入与预期 outcome kind。默认仅授予 `session.rpc`，需要其它服务会显式失败。
预期值及输入中的完整字符串 `${instanceId}` / `${epoch}` 可绑定本次 lease；字符串
内嵌片段不替换。夹具使用本次 epoch 的本机 socketpair、真实包检查 / SDK / RPC，
对选定侧验证激活、成功值、声明错误类别、发送前排队取消、旧 epoch、撤权、配置预算拒绝、
引擎释放和 fd 数量平衡。脚手架的 `throw` 是明确错误夹具，结果为 unknown，不能
将其当作成功业务结果。

单侧 Executor 可显式接入只读资源根，支持 Portable、Node 与 Exec；Node / Exec
同时需要上述执行配置和执行授权。命令使用绝对目录，不从 cwd 或环境猜测根：

```sh
python3 scripts/gterm-plugin test --project tmp/plugin-dev/resource --tools tmp/plugin-dev/tools --contract tmp/plugin-dev/resource/plugin.test.json --core-build tmp/plugin-dev/core --resource-root "$PWD/tmp/plugin-dev/source" --allow-resource-access
```

本次授权将该目录以 `workspace` 提供给 Executor。所选 variant 必须显式 requires
`session.rpc`、`resource.receive`、`file.readScoped`，manifest 声明对应 Executor
权限；显式 Host JSON 的 capabilities 也须恰为这三项，已有 denied 事实仍拒绝。
未提供 Host JSON 时使用上述能力的 Portable Executor 测试 profile。缺授权拒绝，
`--side client` / `paired` 不接受此根；默认 RPC profile 不因此获得文件服务。

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
不绘制资源，不代表双侧 JS 资源、SSH、吞吐或完整文件变化 / 限额矩阵已验。

配置预算准入不等于 OOM / RSS 扫描；请求取消不等于业务任务取消。本片不覆盖 OS
沙箱、SSH、tmux、原生 UI、业务进程服务、整进程预算或完整插件生命周期矩阵。
隔离变异各要求正常断言红、独立 Core 配置绿、恢复绿及源码 SHA256；编译失败、
崩溃和超时不计目标红。新增手工脚本未加入 CI 二十份。

用完删除本次创建的 `tmp/plugin-dev/`，保留其它工作的临时目录。开发命令每次调用的
内部固定副本与探针会在正常退出、INT / TERM 中断时清理；TERM 同时收回受控工具进程组。
