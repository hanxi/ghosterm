# 开发时验证可取消任务插件

在 macOS 开发环境构建官方 `task primes` 插件，通过原生宿主验证同一 Lua Executor 的本机、
SSH 直连和 ProxyJump 路线。普通 App 默认关闭插件运行时。下面的开发包供显式审阅与授权。

准备仓库锁定的 [libssh2](../reference/host-ssh2-build.md)、原生终端构件、Xcode 和
SSH 测试工具链。工具链可用 `bash scripts/bootstrap-ssh-channel-tests.sh` 准备。
Lua 源码归档由 CMake 按仓库锁定的 SHA-256 校验。

## 构建与验证

从工程根目录执行，`tmp/native-task/package` 使用新路径。

```sh
mkdir -p tmp/native-task
export TMPDIR="$PWD/tmp/native-task" TMP="$PWD/tmp/native-task" TEMP="$PWD/tmp/native-task"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
curl -fL https://www.lua.org/ftp/lua-5.5.1.tar.gz -o tmp/native-task/lua-5.5.1.tar.gz
python3 -B scripts/gterm-plugin build plugins/official/task --out tmp/native-task/package
cmake -S cli/gterm -B tmp/native-task/cli -DCMAKE_BUILD_TYPE=Debug \
  -DGHOSTERM_WITH_PLUGIN_LUA=ON -DGHOSTERM_WITH_LIBSSH2=ON \
  -DGHOSTERM_LUA_ARCHIVE="$PWD/tmp/native-task/lua-5.5.1.tar.gz"
cmake --build tmp/native-task/cli -j 4 --target gterm
python3 -B scripts/test-plugin-task-native.py \
  --build tmp/native-task/cli --package tmp/native-task/package --mutate \
  --receipt tmp/native-task/native-final.json
```

`main.lua` 声明 Client 的 `session.rpc` 和 `ui.task` 能力。Executor 使用
`session.rpc` 推进有界的质数计算和处理取消请求。构建工具静态生成 `manifest.json`
和完整性清单，保留 Lua 源码供宿主加载。

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
替代真实丢包、重连、系统辅助技术或物理设备验收。macOS / Android 已接原生 ui.task，
缺服务须显式不支持。范围与最新证据见[插件计划 P5](../reference/plan-plugin-bridge.md)。

## 验证 Android 原生任务视图

在已准备仓库锁定 Android JDK / SDK / NDK、Gradle 缓存及 API29 / API36 arm64 系统镜像的
macOS 开发机上，复用上面同一份包与 gterm：

```sh
build/ssh-channel-toolchain/bin/python scripts/test-plugin-task-android-native.py \
  --lua-archive tmp/native-task/lua-5.5.1.tar.gz \
  --build tmp/native-task/cli --package tmp/native-task/package --mutate \
  --receipt tmp/native-task/android-task.json
```

脚本复制 Android 源码，显式构建 Lua-enabled APK，创建自有 `-no-window` AVD，核对实际
安装 APK 摘要；不选择用户设备或现有模拟器。它对同一包执行直连 / 两跳、任务完成 /
空任务 / 真实按钮取消，以及原生 codec / 模型 / Compose 和隔离变异。`--api 29` 或
`--api 36` 可单独选档，`--case 名称` 只筛变异组，基线与实际 SSH 仍执行。签名发行、
物理设备、TalkBack / IME 和完整客户端验收须另记，不能从本专项推导。

结束后清理本次创建的 tmp 子目录；保留仍在使用的构建与非本次输入，不操作用户服务。

## 连接明确选择的前台 IPC 实例

同一 Lua Executor 也可由独立前台进程启动，然后经 App 的本机进程 / 已认证 SSH
通道连接。前台实例必须预先配置准确 Client / Executor 摘要、RPC 方法和 Client
服务；attach 沿用这些配置，不会重新授权或补装缺失服务。前台的公开实例 ID 与
256 位私有 permit 分开，App 只接收 ID，permit 留在前台 0600 文件中。

以下命令在准备好已构建的任务包后从工程根目录运行；目录须是新的私有目录：

```sh
ipc_task_package="$PWD/tmp/native-task/package"
ipc_task_endpoint="$PWD/tmp/native-task/front"
ipc_task_digest="$(shasum -a 256 "${ipc_task_package}/integrity.json" | cut -d ' ' -f 1)"
mkdir -m 700 "${ipc_task_endpoint}"
tmp/native-task/cli/gterm relay --listen "${ipc_task_endpoint}" \
  --trust-development-digest "${ipc_task_digest}" --client-digest "${ipc_task_digest}" \
  --client-capability ui.task --method view.task.cancel \
  --method view.task.describe --method view.task.read \
  -- "${ipc_task_package}" 'task primes' --limit 1000000
```

读取 stderr 的 `local endpoint ready instance=...` 公开 ID，在实验页选择连接已有
前台实例、准确目录 / ID / 完整调用并明确允许。本例调用为 `task primes` 与
`["--limit","1000000"]`。前台就绪后 30 秒内接入；过期需手动重启并重新核对新 ID，
不得由旧页面自动回退或接管同目录的新进程。选择 SSH 时目录属于明确选择的远端
端点，不从终端输出猜主机或代换身份。接入即撤销一次性入口；不能重连 / 重放旧
instance，不能据此认为任务可恢复。

手工 conformance 使用自有后台 PTY 与本机 / 直连 / 两跳服务，所有 App 窗口在后台：

```sh
build/ssh-channel-toolchain/bin/python scripts/test-plugin-task-native.py \
  --build tmp/native-task/cli --package tmp/native-task/package --attach \
  --receipt tmp/native-task/ipc-mac.json
build/ssh-channel-toolchain/bin/python scripts/test-plugin-task-android-native.py \
  --lua-archive tmp/native-task/lua-5.5.1.tar.gz \
  --build tmp/native-task/cli --package tmp/native-task/package --attach \
  --receipt tmp/native-task/ipc-android.json
```

attach 档另选百万上限的 unknown 场景：卸载 UI 轮询后，先确认自有前台进程已停止
调度，再在生产模型保留取消意图；未收到回包前杀死该自有进程，检查 Native 控件
失效、取消 ID 保留和不重放。不证明请求已到达业务 handler，也不以 EOF、SIGKILL
或进程退出推断业务取消成功；已确认取消由独立按钮场景的 read 回包证明。
前台退出码、端点撤销、旧 ID 重放拒绝与终端零协议字节分别记录。
