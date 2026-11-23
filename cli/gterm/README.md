# gterm 前台 CLI

`gterm` 是独立的插件执行端程序。它检查 Lua 插件包、管理执行端目录，并为原生宿主启动 Lua Executor。普通命令由已登记的包提供。

## 安装目录与更新

版本化 Release 附带 `install-gterm.sh`。安装器默认将入口放在
`~/.local/bin/gterm`，用 `--prefix DIRECTORY` 选择其他前缀。版本放在前缀下的
`lib/gterm/versions/`，入口由原子切换的符号链接指向当前版本。

`gterm upgrade` 校验独立稳定渠道的 Ed25519 签名索引，再核对本机平台的资产
SHA-256 和 `doctor`。`gterm rollback` 检查并切回上一版本。插件库位于独立的数据目录。
安装器和升级使用系统的 `curl`、`tar` 与 SHA-256 工具，开发子命令需要 Python 3。

发行工作流在 PR 上测试 macOS arm64、Linux x86_64 和 Linux arm64。Linux 以
Ubuntu 22.04（glibc 2.35）构建，macOS 设置 13.0 部署目标。生产发布使用独立
`gterm-vMAJOR.MINOR.PATCH` 标签与 `gterm-release` 环境。在发布前设置仓库变量
`GTERM_UPDATE_PUBLIC_KEY`（32 字节 Ed25519 公钥的小写十六进制）及同一环境中的
`GTERM_UPDATE_SIGNING_KEY`（对应 PEM 私钥）。首次发行前需推送 `gterm-stable` 渠道标签，发布工作流会在该标签上维护签名索引。
安装脚本的资产摘要随发布版本固定。

## 构建和检查

从仓库根目录执行。将 `GHOSTERM_LUA_ARCHIVE` 设为本机 Lua 5.5.1 源码归档的绝对路径。CMake 核对归档摘要。启用签名安装需要可用的 OpenSSL 和 zlib。

```sh
cmake -S cli/gterm -B tmp/gterm-build \
  -DGHOSTERM_WITH_PLUGIN_LUA=ON -DGHOSTERM_WITH_RELEASE_SIGNATURE=ON \
  "-DGHOSTERM_LUA_ARCHIVE=$GHOSTERM_LUA_ARCHIVE"
cmake --build tmp/gterm-build --target gterm
tmp/gterm-build/gterm doctor
```

先用开发工具从 Lua 声明生成包目录。将下列 Preview Executor 的示例事实保存为 `tmp/host.json`，并按目标宿主的能力调整。

```json
{"side":"executor","sdkVersion":"1.0.0","protocols":[1],"runtimes":["lua"],"capabilities":["session.rpc","resource.receive","file.readScoped"]}
```

```sh
tmp/gterm-build/gterm plugin build plugins/official/preview --out tmp/preview-package
tmp/gterm-build/gterm plugin check --host-json tmp/host.json -- tmp/preview-package
```

Preview 的实际 Executor 能力声明见 [Lua 入口](../../plugins/official/preview/main.lua)。校验读取固定快照的清单与完整性索引，输出包摘要、发布者声明、命令和兼容性结论。Host JSON 表示调用方观察到的事实。检查本身不执行插件、不确认发布者身份，也不授予授权。省略 Host JSON 时兼容性为未知。旧包需重新构建并安装。

开发子命令依赖 Python 3。`cmake --install tmp/gterm-build` 将辅助工具与 `gterm` 一起安装。原生执行端的 `relay` 和安装目录管理命令可独立部署。`gterm plugin check main.lua` 检查源码声明，`gterm plugin check -- PACKAGE` 检查已生成的包。

## 启动 Executor

原生 Host 经专属 stdin/stdout 启动已核对摘要的开发包。stdin 或 stdout 为 TTY 时拒绝启动。协议帧写到 stdout，诊断写到 stderr。

```text
gterm relay --stdio --trust-development-digest PACKAGE_SHA256 --client-digest CLIENT_SHA256 \
  --method preview.open --root workspace ABSOLUTE_WORKSPACE \
  -- ABSOLUTE_PACKAGE_DIRECTORY preview ./example.bin
```

Host 明确选择包和 Client 摘要、协商方法并批准能力，再启动 Lua 入口。文件访问经 `--root` 声明的范围，资源字节经单独的资源通道收取。已有原生 Host 可在同一专用连接上以 `relay --multiplex` 管理多个实例，或以 `relay --listen`、`--inspect` 和 `--attach` 操作本地前台端点。用 `gterm` 的用法输出核对当前参数。

需要外部 CLI 的 Lua 包通过 `--process-program NAME ABSOLUTE_PATH VERSION SHA256` 配置已安装程序。名称与 Lua 包声明一致。可用 `--process-argument NAME VALUE` 传入固定前缀参数，用 `--process-environment NAME` 选择额外环境变量名。程序映射、访问范围和本次 Session 授权分别校验。

## 登记和安装

Executor 默认在 `${XDG_DATA_HOME:-$HOME/.local/share}/gterm/` 下保存 `catalog/` 和
`packages/`。普通命令自动读取默认库，也可用 `--catalog` 或 `GTERM_CATALOG`
选定独立目录。安装与版本维护可通过 `--store` 选定独立包目录。

已确认完整性摘要的开发包可用 `plugin register-development` 登记。签名包可用 `plugin install` 安装并登记。

```text
gterm plugin install --catalog ABSOLUTE_CATALOG --expect-revision N --store ABSOLUTE_STORE \
  --artifact PACKAGE.zip --descriptor RELEASE.json --signature RELEASE.sig \
  --policy CHANNEL_POLICY.json --host-json HOST.json -- [EXECUTOR_OPTIONS]
```

首次空目录登记用预期版本 0。修改现有条目前先读取当前目录版本，替换已登记包时提供原包摘要。签名安装依照显式渠道策略校验发布签名、包和兼容性。登记后用 `gterm --catalog ABSOLUTE_CATALOG COMMAND [ARG...]` 启动普通前台命令，原生 Host 可挂接并确认本次 Session。

用 `plugin list` 查询登记项，`plugin versions` 查询已安装版本。清理版本或残留前先用相应的 `review-installed` 或 `review-residue` 取得审查摘要，删除时提供预期目录版本和对应摘要。渠道策略变化后用 `plugin review-policy` 核对当前目录，并用 `plugin disable-denied` 显式禁用选中的条目。

## 本地验证

```sh
bash scripts/gate-lua-plugin.sh
```

此闸门验证 Lua Core、CLI 开发与签名安装、实际 SSH 链路和 Preview 资源字节。原生应用与真实后端验证范围见 [Lua 迁移计划](../../docs/reference/plan-plugin-bridge.md)。插件源码和构建流程见 [Lua 插件开发](../../docs/how-to/plugin-development.md)。

迁移前的 CLI 行为和测试记录可在 [Git 历史](https://github.com/hanxi/ghosterm/blob/c579ed985b92c82260aa91500faf927d447aaf11/cli/gterm/README.md) 查阅。
