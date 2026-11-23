# 开发 Ghosterm Lua 插件

从仓库根目录执行本页命令。Ghosterm 使用锁定的 Lua 5.5.1 运行插件，开发工具使用 Python 3。生成包和本地试跑均使用同一份 Lua 源码。

## 写一个单文件插件

用脚手架生成带静态声明的入口。

```sh
python3 scripts/gterm-plugin init tmp/hello-plugin \
  --id org.example.hello --command hello --publisher Example
```

`tmp/hello-plugin/main.lua` 的结构如下。也可以将文件单独保存为 `hello.lua`，构建时工具会把它放入包内的 `main.lua`。

```lua
local manifest = {
  id = "org.example.hello",
  version = "0.1.0",
  api = "1",
  publisher = "Example",
  license = "MIT",
  client = { requires = {} },
  commands = {
    { path = { "hello" }, title = "Hello" },
  },
}

return {
  hello = function()
    return true
  end,
}
```

`manifest` 使用字面量 table。构建工具只读取声明，并从中生成 `manifest.json` 和覆盖文件字节的 `integrity.json`。命令处理器在宿主启动插件后执行。试跑时处理器返回 `true` 表示成功。

```sh
python3 scripts/gterm-plugin lua-manifest tmp/hello-plugin/main.lua
python3 scripts/gterm-plugin check tmp/hello-plugin
python3 scripts/gterm-plugin build tmp/hello-plugin/main.lua --out tmp/hello-package
python3 scripts/gterm-plugin pack tmp/hello-plugin/main.lua --out tmp/hello.zip
```

每次构建使用新的输出路径。同一份源码重复打包会生成相同的 ZIP。可用 `files` 声明额外文件，命令的 `argsSchema` 引用包内 JSON Schema。示例见 [Preview 插件](../../plugins/official/preview/main.lua)。

## 本地运行

先构建带 Lua 的 Core 和 testkit。将 `GHOSTERM_LUA_ARCHIVE` 设为本机 Lua 5.5.1 源码归档的绝对路径。CMake 会验证归档摘要。

```sh
cmake -S core -B tmp/lua-core -DGHOSTERM_BUILD_TESTS=ON \
  -DGHOSTERM_WITH_PLUGIN_LUA=ON "-DGHOSTERM_LUA_ARCHIVE=$GHOSTERM_LUA_ARCHIVE"
cmake --build tmp/lua-core --target test_lua_client
python3 scripts/gterm-plugin test tmp/hello-plugin/main.lua \
  --core-build tmp/lua-core --command hello
```

通过 `--arg` 逐项传入命令参数。客户端与执行端都有 Lua 入口时，使用 `--paired` 在同一本地 Session 启动两侧。

## 声明 Client 和 Executor

- **Client** 在 Ghosterm 宿主内运行，处理命令与原生视图，可使用已协商的 RPC 请求 Executor 执行工作。
- **Executor** 在选定的执行环境运行，可通过受控根目录和进程映射获取已授权的能力。
- 两侧分别声明 `requires` 和可接收的 `rpc`。处理器表必须与声明对应。只有一侧有代码时，可以只声明所需一侧的入口。

用 Lua table 表达对象和数组。空数组使用 `ctx.json.array()`，JSON null 使用 `ctx.json.null`。Session 协议在宿主和 gterm 间传输有界的 JSON RPC。用资源通道传输文件，并通过 RPC 传递资源引用。完整 Lua 双侧示例见 [Preview Executor](../../plugins/official/preview/executor.lua)。

## 校验和安装

`gterm plugin check` 检查构建后的包目录，可按调用方提供的 Host 事实核对兼容性。签名 ZIP 可通过原生插件页面审查并安装。发布工具提供 `gterm-plugin release sign`、`verify`、`sign-policy` 和 `verify-policy`，通过外部签名器签名。用各子命令的 `--help` 查看参数。

旧插件需要从 Lua 源码重新构建并安装。运行、签名安装和验证入口见 [gterm](../../cli/gterm/README.md)。架构和证据范围见 [Lua 插件迁移计划](../reference/plan-plugin-bridge.md)。

迁移前的开发指南和验证记录可在 [Git 历史](https://github.com/hanxi/ghosterm/blob/c579ed985b92c82260aa91500faf927d447aaf11/docs/how-to/plugin-development.md) 查阅。
