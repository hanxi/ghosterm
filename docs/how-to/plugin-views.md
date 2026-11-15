# 注册标准插件视图

候选 `ctx.views` 是本地 Native 视图注册服务。它把 Client 的 table / diff / task
方法绑定交给原生 owner，不向终端或远端发送视图管理消息。注册成功表示宿主接受
了元数据，不表示画面已显示、用户已操作或 Executor 业务已完成。

当前已接 Core、Portable SDK、Swift 与 JNI / Kotlin 的启用、快照和关闭门面。
macOS 与 Android 的 JS-enabled 面板按所选命令的 `ui.views` 必需能力消费注册表，
可同时显示多个标准组件。缺少已实现服务的宿主对此类包显式报告不支持。
没有声明该能力的旧单 table / diff / task 入口继续使用既有数据链。完整键盘焦点、
系统辅助技术、隐藏 / 恢复与用户选包点击整链仍待验收。

多视图 Client 需要声明 `session.rpc`、`ui.views` 和使用的 `ui.table` / `ui.diff` /
`ui.task`；所选命令的 `views` 列出对应 kind。每个绑定方法仍须由用户纳入本次
RPC allowlist。`gterm relay --client-capability ui.views` 仅协商 Client 服务，
不会给 Executor 增加渲染能力。缺少 RPC、重复或未知服务均显式拒绝。

Native owner 在同一非 UI Runtime worker、第一次 `step` 之前显式调用：

```c
/* table=1, diff=2, task=4；按实际支持的标准组件选择，不扩大权限。 */
gt_status_t status = gt_ext_runtime_views_enable(runtime, 1);
```

仅 Host Runtime 可启用。包模式必须有准确的 invocation，所选命令声明对应 views，
Client requires 对应 `ui.*`，且 Session 的 `session.rpc` 和该能力均已授权。选择
另一个没有 views 的命令不会沿用其它命令的声明。Node / Exec、Executor 和 JS-off
不提供此服务；重复启用或启动后启用被拒绝。失败不接管或销毁 Runtime，原 owner
仍须完成关闭和释放。

Swift 门面是 `enableViews(kinds:)`、`viewsSnapshot()`、`closeView(handle:)`；Kotlin
为同名方法（关闭参数名为 `viewHandle`）。两端先在原 worker 复制 C 的借用快照，
再将不可变值交给 UI。这里没有任何自动系统窗口或视图创建。

Client 可声明绑定：

```ts
import type { PluginContext } from "@ghosterm/plugin-sdk";

export function activate(ctx: PluginContext) {
  if (ctx.side !== "client" || !ctx.views) throw new Error("UNSUPPORTED");
  const table = ctx.views.open({
    kind: "table", viewId: "metrics",
    methods: { describe: "metrics.describe", read: "metrics.page" }
  });
  return { dispose() { table.close(); } };
}
```

方法必须已在本次 Session allowlist 中，接收者为 Executor，能力为 `session.rpc`；
方法名称不同，不允许把 Host 方向方法或不存在的方法拿来当绑定。kind 为 task 时
methods 必须另含 cancel；其它 kind 不接受 cancel。各方法的参数 / 回包沿用对应
标准 table / diff / task codec，viewId 应与实际 describe 返回值一致。宿主消费
快照时还须校验描述符身份、修订和本次请求，不能把注册视为对远端内容的认证。

同一 Runtime 最多八个活动注册，viewId 在活动集合中唯一。可注册多个同类型视图，
但不扩大 Session 既有十六方法上限。每个句柄独立关闭；关闭不是业务取消，task 的
取消回执与确认停止仍走既有任务协议。Native 也可通过 `gt_ext_runtime_view_close`
关闭注册，此后 JS 句柄 state 为 closed。同 ID 重开会获得新句柄，旧句柄的迟到
close 不会删除新注册。

两端以 Runtime / epoch / handle 保存独立模型，描述符 viewId 须与注册一致；相同
快照保持分页状态。关闭按钮调用 Native owner，移除后停用旧模型，旧页和旧快照不
能复活已关闭句柄。关闭失败保留视图及显式重试入口。同 Runtime 的任务观察另有
最多八个业务 scope 记录；关闭 / 重开保留取消 ID、未知结果及已确认终态，不自动
重放取消，记录满时显式报错。停止并释放插件后才销毁该账本。

`handle.state` 保留 ok / closed / unknown / denied / revoked / unsupported 等
观察，不把未知当拒绝。`ctx.views.dispose()` 使本地句柄失效，逐个尝试 Native
关闭；Runtime 的原生释放仍独立验证。停止 / 撤权后快照拒绝返回旧注册，关闭
Runtime 时擦除注册数据并归还计费内存。注册表内存纳入同一 Runtime heap / Core
分配预算，不新增无界队列或逐 token UI 通知。

Native 快照为最多 8192 字节 UTF-8 JSON：

```json
{"instanceId":"...","epoch":"1","revision":"4","views":[{"handle":"...","viewId":"metrics","kind":"table","methods":{"describe":"metrics.describe","read":"metrics.page"}}]}
```

epoch / revision 使用规范 uint64 十进制字符串；handle 是不透明的本地标识，不是
可移交的权限凭据。快照借用至下次可变调用 / step / close，失败清空输出。UI 必须
绑定本次 Native Runtime / epoch，旧 owner 的异步结果不能应用到新实例。

本片手工验证命令（工具准备见[开发指南](plugin-development.md)）：

```sh
python3 scripts/test-plugin-views.py --tools tmp/plugin-views/tools --mutate --receipt tmp/plugin-views/proof.json
```

十五组真实公开 Runtime / Session / SDK 用例、四组包命令约束和四十四项 Portable
codec / adapter 检查分别记账。八组 Core 变异与一组 SDK 迟到关闭变异要求目标正常
断言红、独立配置绿、恢复绿和 SHA256；编译失败、崩溃、sanitizer 报错不计红。
sanitizer 档为 RelWithDebInfo + ASan / UBSan，LeakSanitizer 未启用。没有 Native UI、
设备、真实 SSH / tmux 或完整客户端验收结论，也未增加 CI 二十份。

macOS 产品消费的独立验证使用官方 `plugins/official/views` 样例（两张表、diff、
可取消的内存计数任务），构建样例后执行：

```sh
python3 scripts/build-plugin-sample.py --source plugins/official/views --tools tmp/plugin-views/tools --out tmp/plugin-views/package
build/ssh-channel-toolchain/bin/python scripts/test-plugin-views-native.py --build tmp/plugin-views/cli --package tmp/plugin-views/package --mutate --receipt tmp/plugin-views/native.json
```

`cli` 须预先按开发指南构建为 JS-enabled / SSH-enabled 并带 CLI 测试夹具。脚本仅使用
项目 tmp 内隔离副本、自有本机 / 直连 / 两跳测试服务和内存密钥，所有窗口放到屏幕
外且透明。它验证生产 AppModel 的审阅 / 授权 / 启动门面、真实 Runtime 注册、独立
分页、原生关闭 / 重开及取消按钮；不代表用户文件选择器整链、系统 VoiceOver 或
Android / 真机验收。具体分轮结果见插件计划 P5 第十二批。

Android 产品消费的独立验证使用相同官方样例：

```sh
build/ssh-channel-toolchain/bin/python scripts/test-plugin-views-android-native.py --build tmp/plugin-views/cli --package tmp/plugin-views/package --mutate --receipt tmp/plugin-views/android.json
```

脚本创建并销毁自有 API 29 / 36 arm64、4 KiB 页模拟器，强制 `-no-window`；不选择
已有用户设备。使用 APK 及实际安装包摘要对拍、正常单条断言红 / 同 APK Core
配置对照 / 恢复绿与源码摘要验证回归保护。真实 Settings 面板经独立 SSH 直连 /
两跳，按 Native 按钮分页、关闭、重开及取消，运行时释放与暂存目录一并回收。
只在设备自身测试沙箱保存包夹具，内存身份经有 token 的一次性本机服务传入；
不安装外部插件或访问用户主机。完整 TalkBack、键盘 / IME、16 KiB / 真机、真实
断网重连及完整选包点击链仍待验收。证据见插件计划 P5 第十三批。

## 前台 IPC 连接

两端 Native 多视图也可使用原 Attachment 门面连接已明确选择的 Portable 前台实例。
先在前台 gterm 的 `--listen` 配置四个 Client 服务 `ui.table` / `ui.diff` / `ui.task` /
`ui.views`，Executor 方法为 `alpha.describe / alpha.page / beta.describe / beta.page /
changes.describe / changes.page / work.describe / work.read / work.cancel / views.reopen`，
Host 方法为 `views.client.open`。调用必须准确为 `views demo`、空 argv，两个包摘要
均为本样例完整性清单的 SHA256。私有目录、公开 ID、30 秒期限和一次性连接语义
见[任务 IPC 指南](plugin-task.md#连接明确选择的前台-ipc-实例)。attach 不新增服务、
根目录或进程映射，前台配置有缺失时仍须显式失败，不能自动另起 Executor。

```sh
build/ssh-channel-toolchain/bin/python scripts/test-plugin-views-native.py --build tmp/plugin-views/cli --package tmp/plugin-views/package --attach --receipt tmp/plugin-views/ipc-mac.json
build/ssh-channel-toolchain/bin/python scripts/test-plugin-views-android-native.py --build tmp/plugin-views/cli --package tmp/plugin-views/package --attach --receipt tmp/plugin-views/ipc-android.json
```

同一前台 gterm 与 attacher 是不同 PID；先完成原生 Session 授权，Client 视图仍在
Native owner 中注册。测试复用原分页、独立关闭 / 重开和任务取消按钮验证，另核对
一次性入口回收、前台退出、旧实例重放拒绝和原 PTY 零协议字节。后台 PTY 夹具不是
新的 TerminalSurface / tmux pane / 焦点或完整用户点击链证据；分轮见计划 P5 第十四批。
