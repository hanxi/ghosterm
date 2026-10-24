# 共用 POSIX 会话宿主进程

状态：**macOS / Linux arm64 宿主、后台启动、stdio 代理与真实 SSH 接线已运行；Linux x86_64 为 QEMU 用户态证据**。
日期：2026-10-21。`daemon/src/host.{h,c}` 把私有端点、共用 server/runtime、
attachment owner、DATA router 和 checkpoint executor 组合起来；`main.c` 注入
真实无 UI VT 工厂与 encoder。没有第二套本地/远端管理或恢复状态机。

## 入口与部署边界

```bash
export TMPDIR="$PWD/tmp"
cmake -S daemon -B tmp/sessiond-build \
  -DGHOSTERM_VT_INCLUDE_DIR=/absolute/path/to/patched-engine/include \
  -DGHOSTERM_VT_LIBRARY=/absolute/path/to/matching/libghostty-vt
cmake --build tmp/sessiond-build --target ghosterm-sessiond
./tmp/sessiond-build/ghosterm-sessiond serve --runtime-dir /absolute/private/runtime
```

当前构建仍要求精确 pin 和实验补丁的配套引擎，不能用系统 Ghostty 冒充。
缺头/库直接使配置失败；Windows 宿主明确不支持当前 POSIX adapter。
`--help`/`--version` 是操作入口；serve 的 stdout 不承载日志或终端字节。
诊断只写 stderr 的错误类别，不记录会话 argv、env、输入或 token。

serve 在前台运行，SIGINT/SIGTERM 表示**显式关闭宿主及其全部会话**。
[共用后台启动与 stdio 代理](session-activation.md)另已提供按需独立 session 与标准流
隔离，连接同一私有端点。安装发现、随包清单与 SSH exec 接线见 [原生安装](session-native-setup.md)；
远端使用现有 SSH 端口，不新增公网监听。

原生平台绑定仍只能经 `ghosterm.h` 与 Transport 消费最终公开接口。
这里是 daemon 私有组合层，不能从 UI 直接调用 `host.h` 或 core 私有头。

## 服务与回收所有权

一个非 UI I/O owner 串行驱动服务轮次。每轮最多接受一个连接，再 poll 固定
连接表；仅有 TX 积压时监听 POLLOUT。固定 10 ms timer 同时驱动期限和异步
DATA 清理，当前尚非最终的通知与空闲 CPU/延迟验收实现。

每条连接的 socket adapter 存储稳定地放在 host 的固定槽位中，活连接退休后
才复用；server 拥有 fd 关闭，host 只借用 fd 做 poll。连接容量满时关闭新 fd，
不动已有会话。端点在各组件及 reaper 准备后才发布；重复启动不能移除仍存活
实例的 socket 或取得其 lifetime lock。

独立 reaper 定期读取 manager 元数据，只对已发布结果的 worker 执行 join。
join、上下文析构和出错会话回收均不在 I/O 路径中。已结束的会话只在仍有客户端
引用时保留 frozen 终态；attachment 和 encoder 释放最后一个引用后，reaper 立即
回收该会话并归还 Source 槽，不设保留计时器。不提前释放借出的 frozen。

客户端关闭、SIGKILL、坏帧或认证拒绝只退休相应连接。没有“连接数为零便停止
会话”的路径。显式结束单个 session 不结束其他 session；显式宿主关闭先撤
监听、断连接并停止 reaper，再等待 router 任务实际回收，最后停止/join runtime、
关闭 executor 和引擎工厂。BUSY 保留所有权，不能把仍在运行的 callback 当成
已回收。完成关闭清空 host 指针；端点清理错误仍显式报告。

## accept 错误的作用域

macOS 上，对端可能在 accept 成功后、fd 配置前已关闭，此时配置 socket 会失败。
端点现在区分以下结果，避免一个短连接导致整个宿主退出：

| 结果 | 含义 | host 行为 |
|---|---|---|
| WAIT | 暂无连接或被信号中断 | 下轮继续 |
| REJECTED | 已 accept 的 peer 配置或凭据校验失败，fd 已关 | 保持 listener 和现有会话 |
| BUSY | OS fd/内存容量暂时不足 | 暂不监听新连接的就绪，继续已有连接及定时工作 |
| AUTH / IO | endpoint 身份或监听失败 | 明确报告宿主错误 |
| UNSUPPORTED | 平台缺少凭据能力 | 保留 `GT_ERR_UNSUPPORTED`，不伪装成认证成功 |

每次 accept 均由内核验证 UID；principal 使用该宿主用户的非零映射。
三条 syscall shim 测试只替换测试对象，不引入生产故障开关；验证配置失败、
凭据查询失败及 OS 容量错误，拒绝 fd 必须关闭，随后正常连接必须仍可使用。

## 容量、状态与协议身份

当前入口最多 4 个 session、8 条连接、2 个 checkpoint 槽/1 个 encoder 线程。
每会话增量队列为 1 MiB/1024 事件，快照上限 8 MiB；已结束会话在最后一个客户端
引用释放后立即回收，创建 nonce 记录最多 64 条/一分钟。host 配置可以缩小容量。

C 动态分配有独立的 64 MiB 原子准入域，按原始大小退款且释放前擦除；VT 有
独立的 512 MiB 映射预算。worker/backend/reaper/encoder 使用显式 2 MiB 栈和
一页 guard。两类预算不覆盖全部原生栈、静态数据和 OS/框架记账，不能据此
宣称完整总内存账本已经通过 P0。

初始 VT 使用固定两档配色、10×20 cell metrics、4 MiB/10000 行历史和内联图像
profile。外部文件/临时文件/共享内存资源仍被禁止；尚无完整资源装载结果、
动画与宿主动作事件流或用户终端配置接线，恢复范围没有缩成“只保活 shell”。

兼容指纹由协议/container schema、相关编解码及状态 adapter、固定上游 pin、
配套引擎头和补丁集合生成。它不含绝对构建路径、OS/CPU 或平台二进制 hash；
依赖内容变化会重新生成。Mac/Linux 原生构建已核对同一指纹。它识别当前
候选契约，不是安全认证或跨版本迁移方案；配套库/头仍由构建输入提供并留存
实际 artifact hash。每次启动的 instance 则来自新的 OS 安全随机值。

PTY EOF 可能先于 waiter 的退出状态发布。结果保留显式未知，不能因为 worker
已回收就编造 exit 0；已知退出码仍核对真实程序的值。

## 早期进程切片证据与尚未交付项

两平台均有 7 类 host 场景及 version 基线、16 项普通 FAIL 变异与独立绿对照。
覆盖真实 executable / 私有 socket / 共用客户端 / PTY / VT 导入；客户端进程
SIGKILL 后恢复同一 PID 和会话，随后输入仍生效；两会话终止隔离、显式关闭、
保留与引用阻止过期、槽位饱和后复用、构造失败退款、重复启动和坏帧均已运行。
这不是 GUI SIGKILL、完整 full surface 或真实 SSH 断网的证据。

端点的 17 类基线/24 项变异通过。核心 gate 为 348 项，57 个公共符号无漂移；
两平台启用现有 SSH 后端的完整 daemon 构建另有 369 项测试通过。
两平台另运行 47 类相关组件/真实 VT/宿主组合及 ASan/UBSan；macOS 同组 TSan
通过。Apple 未启用不受支持的 leak detector，Linux 启用；借用的引擎库未重编
sanitizer，不代表引擎内部无缺陷。遗留标记退出 2、INT/TERM 编译器中断、后代
清理及源码 SHA256 回滚另验。

```bash
ctest --test-dir tmp/sessiond-build -R '^(host_process_|endpoint_peer_)' --output-on-failure
python3 scripts/mutation-check-session-host.py --build-dir tmp/sessiond-build
python3 scripts/mutation-check-unix-endpoint.py
bash scripts/gate.sh
```

源码/库 hash、编译选项与日志保存在工程 `tmp/session-host-*`。Linux 使用原生
内核与 C 编译器运行 ELF/真实 PTY，临时资源放在工程路径下的原生临时文件系统。

SSH owner/exec、租约与断网矩阵、公开 client/session ABI、原生 GUI 与安装发现
已有后续切片证据，见 [产品协调器](session-product-coordinator.md)。其余客户端/
宿主与完整系统/GPU/第三方资源预算仍待完成，P0 与完整方案保持未完成。

## 默认入口容量切片（2026-10-21）

命令行宿主配置上限改为 32 个 Source、64 条连接；已结束会话在最后一个客户端引用
释放后立即回收、归还 Source 槽（不再按保留期占槽）。这不是 32 个完整 GUI 终端的
资源验收。已验证六个真实 PTY/VT Source 独立创建、列举与进程标识，以及 64 条连接
饱和后拒绝新连接、释放后复用。原四会话/八连接上限各有定向变异，host 共 18 项变异、
9 类基线通过。SSH 关闭的独立后端构建 448 项非 SSH 用例通过，不能替代启用 SSH 的
核心 gate。Linux aarch64 原生另通过六 Source 与 64 连接两个真实进程用例。当前启用
SSH 的核心 gate 为 414 项/82 ABI，完整 P0 仍未通过。
