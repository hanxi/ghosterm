# 共用 runtime 的无 UI VT 状态适配

状态：**候选组件已接真实引擎；生产恢复未交付**。日期：2026-10-21。
实现为 `daemon/src/vt-state.{h,c}`，直接返回
`core/src/session/runtime.h` 的私有 state factory，不另建 session、PTY worker、
manager 或恢复状态机。本地 IPC 与 SSH 代理最终使用同一 runtime 和该工厂。
部署约束见[本地与远端架构](../architecture/remote-session.md)。

[共用宿主进程](session-host-process.md)另已组合本工厂，在 macOS/Linux arm64
原生运行真实 executable、私有 socket 和 PTY/VT 恢复；生产后台启动、SSH 与
原生 Surface 接线仍待交付，不能将其当成跨 OS 快照交换的证据。

## 依赖与 owner

daemon 组合目标同时链接 core 和无 UI libghostty-vt。core 仍不包含引擎头，
`terminal/session-state` 中的预算/冻结辅助仍只依赖引擎与 POSIX，不引用私有
core。组件不链接 AppKit、GTK、Metal 或 full surface，不增加公开 ABI。
所需 page allocator、独立 frozen 和资源扩展来自钉版本实验补丁；裸上游库
不能替代它。`daemon/CMakeLists.txt` 显式要求匹配的引擎头与库；当前不是
sessiond 可执行入口或自动安装器。

runtime 在唯一 session worker 上创建状态，传入 CREATE 的列数/行数及查询
应答 sink；OUTPUT、RESIZE、COLOR_SCHEME 使用既有 state ops。查询应答只进
worker 的有界回复暂存，由 worker 写回原 PTY，不经 GUI。sink 的 sticky
失败仍由 worker 以 REPLY 阶段报告。每次 feed/resize 检查预算及引擎 sticky
语义错误，失效后不能继续导出健康状态。

配色配置包含浅/深两档默认前景/背景，COLOR_SCHEME 改变当前档并支持查询。
cell_width/height 是明确配置的每格像素尺寸，不能从整个 PTY 的 px_width/height
直接冒充；像素度量协商和任意主题配置仍需产品接线。

## 冻结、编码和预算

每个状态、冻结对象、编码对象分别在稳定地址持有完整准入 scope；general、
OS page 和 scratch 使用已有预算分配器，共用同一工厂账本。工厂与记录对象
走调用方线程安全 allocator。线程栈、guard、内核对象与其余引擎系统开销仍
须由组合根准入，不能把本组件的预算说成完整 RSS 上限。

冻结由源 worker 完成，得到独立 immutable 对象。源状态后续变化或关闭不影响
该对象；encoder 可以在另一非 UI 线程调用 `vt_checkpoint_encode`。编码结果
独立持有 canonical、resources 两段及配色、尺寸、历史策略/已淘汰标记，借用
view 直到 encoded_release。活跃捕获按 runtime_release_capture 归还，终态
捕获仍归 runtime 引用/forget/close 生命周期，不能由客户端提前释放。

工厂关闭前须停止新调用；存在状态、冻结或编码对象时返回 BUSY，并保持可用。
快照准入失败只释放该次准备，不结束原会话；输出、查询仍可继续。底层 scope
若未成功回收，不释放仍被借用的记录地址，工厂继续拒绝关闭。

## 明确未完成的恢复能力

canonical、resources、continuation 和元数据已接入[候选快照容器](session-checkpoint.md)
与共用 DATA encoder，真实 PTY 到无 UI VT 副本的恢复组合已验；没有另建恢复
状态机。原生 surface 导入和产品客户端仍未接线，私有格式不作跨版本兼容声明。
该组合的接收端现已用[共用客户端](session-attachment-client.md) 替换手写 DATA
消费流程；HELLO/CREATE/ATTACH/BIND 也已使用[共用引导与管理客户端](session-client-bootstrap.md)；
生产原生绑定、引擎应用 adapter 和 SSH 入口仍待接线。

当前配置显式禁止文件、临时文件和共享内存图像装载；请求 external_resources
返回 UNSUPPORTED。内联图像的冻结/编码有真实 RGBA 像素验收。这项初始限制
不改变最终恢复范围：外部资源装载结果及失败需要有序事件，不能让客户端重读
源路径；动画时钟、宿主动作、程序/命令摘要、历史淘汰增量、通用配置、完整
资源账本和 full surface 导入/barrier 仍须在同一流水线中收口。

SSH stdio 代理、生产监听/启动/安装、公开 client ABI 和原生 GUI 尚未接线。
本组件通过不代表网络断线恢复、跨 OS/CPU 快照或所有客户端已经支持。

## 当前验证

macOS arm64 使用 pin `81598385d608dda4946093d6d6a234d204a2f225` 的已打实验
补丁 VT 动态库，七类基线覆盖：真实查询/配色/resize、源关闭后的独立冻结及
非 owner 编码、能力拒绝/失败输出清空、准入不足后源继续工作、四个外层分配点
失败回收、真实 runtime/PTY detach 后重新附着与终态编码、内联图像真实像素。
runtime 场景没有 GUI，子 shell 必须实际读到光标查询应答才继续；重新取得的是
同一个 session/attachment 控制器。其输入由可信测试宿主提交，不代表客户端
授权、DATA 或 SSH 链路已通过。PTY EOF 先于 waiter 观察退出码时保留 UNKNOWN。

上述首轮为七类基线/十二项变异；加入容器与真实 DATA 后，共十四类基线、37 项
隔离变异以及十四类 ASan/UBSan/TSan 通过，详见[容器参考](session-checkpoint.md)。
TSan 覆盖工厂/runtime
的 C 路径，借用的 VT 动态库未重编为 TSan，不能据此宣称引擎内部无竞态。
每个变异只接受普通 FAIL/exit 1，
同变异独立对照仍须绿，最后回滚复绿并核验起跑前 SHA256。日志在工程
首轮 `tmp/daemon-vt-{tests,mutations,sanitizers,tsan,harness,gate}.log`，新一轮日志见
容器参考；源/依赖哈希在 `tmp/daemon-vt-proof.json`。Linux 仅容器原生运行和
工厂/适配/夹具严格编译，尚无 Linux 或跨架构引擎组合证据。
遗留标记退出 2，真实编译器运行中的 INT/TERM 后代退出及十四组源码 SHA256
回滚均已核验；仓库核心 gate 322 项、57 个公共 ABI 符号保持通过。

下面使用本机已有的隔离引擎缓存；该路径不是生产构建依赖或自动回退策略：

```bash
export TMPDIR="$PWD/tmp"
cmake -S daemon -B tmp/daemon-vt-build \
  -DGHOSTERM_VT_INCLUDE_DIR="$PWD/tmp/worker-surface/engine/include" \
  -DGHOSTERM_VT_LIBRARY="$PWD/tmp/worker-surface/out/lib/libghostty-vt.dylib" \
  -DGHOSTERM_WITH_LIBSSH2=OFF
cmake --build tmp/daemon-vt-build --target test_daemon_vt test_checkpoint_wire -j 4
ctest --test-dir tmp/daemon-vt-build -R '^(daemon_vt_|checkpoint_wire_)' --output-on-failure
python3 scripts/mutation-check-daemon-vt.py \
  --engine-include tmp/worker-surface/engine/include \
  --engine-library tmp/worker-surface/out/lib/libghostty-vt.dylib
```
