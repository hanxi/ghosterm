# 注册标准插件视图

候选 `ctx.views` 是本地 Native 视图注册服务。它把 Client 的 table / diff / task
方法绑定交给原生 owner，不向终端或远端发送视图管理消息。注册成功表示宿主接受
了元数据，不表示画面已显示、用户已操作或 Executor 业务已完成。

当前已接 Core、Portable SDK、Swift 与 JNI / Kotlin 的启用、快照和关闭门面。
产品面板尚未消费该注册表，默认不启用；现有单 table / diff / task 的原生入口继续
使用其既有数据链。多视图的产品渲染、焦点、隐藏 / 恢复、原生关闭动作和用户点击
链须继续实现并验收，不能仅凭本片 API 测试记为完成。

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
