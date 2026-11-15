# 开发时验证可取消任务插件

本页在 macOS 开发环境构建官方 `task primes` 候选包，并通过真实 Native Host 验证同一
Executor 的本机、SSH 直连和 ProxyJump 路线。普通 App 默认仍关闭 JS；插件不自动安装
或发布。`gterm task primes` 尚非普通命令分发入口，下面使用显式的开发 testkit。

先准备仓库锁定的 C / libssh2、QuickJS submodule、原生终端产物、Xcode、Node / npm 和
SSH 测试工具链。相关入口见 [SSH 依赖构建](../reference/host-ssh2-build.md)、
[gterm 构建](../../cli/gterm/README.md)；SSH 测试工具链用
`bash scripts/bootstrap-ssh-channel-tests.sh` 准备。需要首次下载的准备动作与离线闸门分开。
命令均从工程根目录执行；以下输出路径应尚不存在，重复构建官方包时可用 `--check` 对拍。

## 构建同一份可移植包

```sh
mkdir -p tmp/native-task/tools
export TMPDIR="$PWD/tmp"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export GHOSTERM_AUTOMATION_OFFSCREEN=1
cp scripts/fixtures/plugin-sdk-tools/package.json scripts/fixtures/plugin-sdk-tools/package-lock.json tmp/native-task/tools/
npm ci --prefix tmp/native-task/tools --cache tmp/plugin-sdk-npm-cache --ignore-scripts --no-audit --no-fund
node tmp/native-task/tools/node_modules/typescript/bin/tsc \
  -p packages/plugin-sdk/tsconfig.json --outDir tmp/native-task/sdk
python3 scripts/build-plugin-sample.py --source plugins/official/task \
  --tools tmp/native-task/tools --out tmp/native-task/package-v2
```

构建器校验锁定 TS 5.9.3 工具、静态模块闭包、schema 和完整性成员，输出可供 review /
approve 的目录包。`plugin.json` 的 Client 要求 session.rpc / ui.task；Executor 只要求
session.rpc，无文件、进程、网络或账户依赖。Portable 源码不导入 Node、DOM 或端 UI。

## 验证本机与远端执行

```sh
cmake -S cli/gterm -B tmp/native-task/cli -DCMAKE_BUILD_TYPE=Debug \
  -DGHOSTERM_WITH_PLUGIN_JS=ON -DGHOSTERM_WITH_LIBSSH2=ON \
  -DGHOSTERM_BUILD_TESTS=ON -DGHOSTERM_GTERM_TESTS=ON \
  -DGHOSTERM_PLUGIN_SDK_DIR="$PWD/tmp/native-task/sdk"
cmake --build tmp/native-task/cli -j 4
build/ssh-channel-toolchain/bin/python scripts/test-plugin-task-native.py \
  --build tmp/native-task/cli --package tmp/native-task/package-v2 --mutate \
  --receipt tmp/native-task/native-final.json
```

testkit 复制 Native 源码并链接这份显式启用库，根工程 `core/build` 保持普通默认档。
它自行创建环回 SSH 端点、内存密钥、agent 与独立配置；不使用用户 SSH 服务、密钥或
已连接主机。本机 / 直连 / 两跳使用同一份包和 gterm，比较完成、空任务和实际按钮取消。
窗口保持后台 / 屏幕外；不省略 Native Session consent 或实际 RPC 来代替远端行为。

应用接线由准确批准命令的 views 和能力驱动。macOS Native Host 提供标准任务组件，
选择本机进程或 SSH 字节通道不会改变 Executor。业务参数通过同一 command schema
解码，不是拼入远端 shell 的素数命令。测试调用生产 review / approve / launch / consent
动作，仍不能据此称完整选包到启动的用户点击链已验收。

## 判断取消与结果不明

- 数字以精确 uint64 字符串传输；未知总量与已知零分开。
- 取消回执表示已受理；读取到 confirmed cancelled 才表示任务停止。
- 取消回包不明时保留请求 ID、禁用再次取消，并停止自动查询。用户可显式读取同一活动任务。
- RPC failed、RPC cancelled、unknown 与任务自身的阶段 / 结果分开；取消一个请求不等于取消任务。
- 关闭或失效保留最后观察，不凭 EOF / dispose 填造成功或取消。

原生字段 / 生命周期专项、位图 / 进程内 AX、实际路由与变异分别记账；真实 SSH 结果不
替代真实丢包、重连、系统 VoiceOver 或物理设备验收。Android 原生 ui.task 仍待接，缺
服务须显式不支持。范围与最新证据见[插件计划 P5](../reference/plan-plugin-bridge.md)。

结束后清理本次创建的 tmp 子目录；保留仍在使用的构建与非本次输入，不操作用户服务。
