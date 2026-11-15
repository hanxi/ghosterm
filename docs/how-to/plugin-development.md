# 开发 Portable 插件

`scripts/gterm-plugin` 提供候选 `init/check/build/pack/test` 命令，当前构建对象为
Portable Client 加单个 Portable Executor。命令读取固定副本，不执行项目的 npm
脚本、安装钩子或插件入口；只有 `test` 会在真实 Core / QuickJS 中激活 Executor。
Node / Exec 包构建、Client 激活、文件 / 进程 / UI 服务测试仍待接入。

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
strict / noImplicitAny、禁止显式 any 和跳过类型检查的注释；可用 ES2022，不能引用
DOM / Node 全局。导入限于包内相对静态闭包及 `@ghosterm/plugin-sdk`，不支持动态
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

`test` 需要显式启用 JS 的工程内 Core 构建，不复用默认 JS-off 的 `core/build`：

```sh
node tmp/plugin-dev/tools/node_modules/typescript/bin/tsc -p packages/plugin-sdk/tsconfig.json --outDir tmp/plugin-dev/sdk
cmake -S core -B tmp/plugin-dev/core -DGHOSTERM_WITH_PLUGIN_JS=ON -DGHOSTERM_WITH_LIBSSH2=OFF -DGHOSTERM_BUILD_TESTS=ON -DGHOSTERM_PLUGIN_SDK_DIR="$PWD/tmp/plugin-dev/sdk"
cmake --build tmp/plugin-dev/core --target ghosterm_core test_config -j 4
python3 scripts/gterm-plugin test --project tmp/plugin-dev/hello --tools tmp/plugin-dev/tools --contract tmp/plugin-dev/hello/plugin.test.json --core-build tmp/plugin-dev/core
python3 scripts/test-plugin-dev.py --tools tmp/plugin-dev/tools --core-build tmp/plugin-dev/core --mutate --receipt tmp/plugin-dev/receipt.json
```

macOS 显式设置 `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`。本片验收
工具链是 macOS arm64，Linux 原子发布分支未据此记为已验。

测试契约是脚手架生成的 `plugin.test.json`：命令、原始 argv、方法、输入 / 预期对象、
错误输入与预期 outcome kind。仅授予 `session.rpc`，需要其它服务会显式失败。
预期值及输入中的完整字符串 `${instanceId}` / `${epoch}` 可绑定本次 lease；字符串
内嵌片段不替换。夹具使用本次 epoch 的本机 socketpair、真实包检查 / SDK / RPC，
验证激活、成功值、声明错误类别、发送前排队取消、旧 epoch、撤权、配置预算拒绝、
引擎释放和 fd 数量平衡。脚手架的 `throw` 是明确错误夹具，结果为 unknown，不能
将其当作成功业务结果。

配置预算准入不等于 OOM / RSS 扫描；请求取消不等于业务任务取消。本片不覆盖 OS
沙箱、SSH、tmux、原生 UI、外部资源服务、整进程预算或完整插件生命周期矩阵。
六组隔离变异各要求正常断言红、独立 Core 配置绿、恢复绿及源码 SHA256；编译失败、
崩溃和超时不计目标红。新增手工脚本未加入 CI 二十份。

用完删除本次创建的 `tmp/plugin-dev/`，保留其它工作的临时目录。开发命令每次调用的
内部固定副本与探针会在正常退出、INT / TERM 中断时清理；TERM 同时收回受控工具进程组。
