# Portable SDK 候选

当前实现类型化请求边界，不是完整插件运行时，也没有发布 npm 包或冻结 v1 API。
生产源在 `portable/`；它只使用 ES2020，没有 Node、DOM、定时器、文件或网络入口。
`schema/bridge-v1.candidate.json` 是 wire 错误定义的事实来源；
`python3 scripts/generate-plugin-sdk.py --check` 对拍生成的 `wire-contract.ts`。

```typescript
import { object, text, integer } from "./portable/index.js";
import type { Method, SessionChannel } from "./portable/index.js";

const message = object({ text: text(40), count: integer(0, 100) });
type Message = ReturnType<typeof message.parse>;
const echo: Method<Message, Message> = {
  name: "example.echo", input: message, output: message,
};

// session 由已批准的 native runtime 提供，不由插件构造 NativePort。
async function example(session: SessionChannel) {
  const call = session.request(echo, { text: "你好", count: 1 }, 1000);
  const outcome = await call.outcome;
  if (outcome.kind === "success") return outcome.value;
  // failed / cancelled / unknown 分开；取消请求本身不是取消完成。
  return outcome;
}
```

`object` 拒绝额外字段，`text` 按 Unicode scalar 计 schema 长度，整数必须在显式范围。
codec 同时提供 TypeScript 推断、运行时解析与可独立验证的 JSON Schema。这是当前对象 / 文本 /
整数子集，不是任意 JSON Schema 解释器。参数在入队前复制，接收参数与成功结果再验业务 codec。
字节上限按 UTF-8 计算；非法 UTF-16、非有限数、不安全整数、访问器、toJSON、环、稀疏数组及
原型污染键拒绝。uint64 以规范十进制字符串保持精确，不能经过 Number 四舍五入。

受信 native adapter 绑定唯一 Session / 原 lease / 串行 owner；它必须继续使用 Core 对方向、能力、
实例、代次和期限的校验。JS 不持有 lease、fd 或私钥。`NativePort` 是宿主契约；Core 的显式可选 QuickJS-NG 后端已接真实 Session，
SDK conformance C host 另作独立对照。公共 runtime C ABI 与 Swift / Kotlin / JNI worker 已接，默认后端关闭；
App 激活入口仍待接。SDK 不是授权源或恶意插件沙箱。

native owner 先推进 C IO，再调用 `session.step()`；runtime 另有独立 CPU / Promise job 预算。
SDK 每次最多 take 8 个请求、保留 64 个发送观察与 64 个处理中请求，最多注册 16 个 handler。
没有自动排队重试、网络重连或批准重放。处理函数每次 await 后、执行副作用前须调用
`context.check()`；真正的文件 / 进程能力在 native 边界还要再检查，不能只依赖 JS。

取消由 Core 判断排队未发或已发结果不明。只有已确认且匹配成功状态的结果才能返回 success；
处理异常、异步中断或不合法结果不推断为“没有副作用”。`dispose()` 立即撤销 native 权限、取消
发送并清空接收账目，不等待插件配合；owner 仍须继续推进 terminal result / forget 和 native 关闭。
未完成 Promise 由受控 runtime 销毁 context 强制回收；SDK 本身不能终止任意 JS 或原生阻塞调用。

验证：`python3 scripts/test-plugin-sdk.py --mutate`，默认离线；缺锁定工具缓存时显式加 `--fetch`。
工具 TypeScript / Ajv 以 lock integrity 固定，临时文件只在仓库 `tmp/`。同一 TS 产物在无 Node / DOM /
quickjs-libc 的 QuickJS-NG 0.18.0 中，调用实际公开 C Session / socketpair；并独立对拍 codec schema。
`confirmation` 单独注入不一致 native adapter 状态，不能算真实网络成功。

runtime 已通过 native adapter 接 Session 请求与受限 activate / dispose；尚未实现完整 PluginContext、
事件 / UI / 资源 / 进程服务、模块安装器、App 页面 / 激活入口、gterm IPC 或多实例调度。
运行时门面验证见 `python3 scripts/mutation-check-plugin-runtime-api.py`；Android 显式测试档另见
`scripts/mutation-check-plugin-runtime-api-jni.py --device 设备`。未来网关只预留，不在当前实现范围。
