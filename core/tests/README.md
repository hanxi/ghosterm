# core/tests — 测试与黄金夹具

## 公共头依赖闸门

`public_headers` 经 `ctest` 纳入 `scripts/gate.sh`：所有公开头分别作为第一个 include，
重复包含后以 C11 / `-Wall -Wextra -Werror` 独立编译。依赖图不得有环，
子头不得反向包含聚合入口 `ghosterm.h`，也不得引用 include 根以外的本地头。
共享错误类型定义于 `ghosterm/types.h`；平台绑定继续只从 `ghosterm.h` 进入，
现有 Apple module map 的符号链接自动消费新布局，不另复制类型。

```bash
python3 scripts/check-public-headers.py
python3 scripts/mutation-check-public-headers.py
```

变异分别引入子头→聚合头、隐藏依赖聚合头类型、两个子头互相包含，
要求目标检查红而同一变异下独立 config 头仍绿。副本位于工程 `tmp/`，
起跑前检查残留标记，INT/TERM 清理，回滚核验起跑前 SHA256 并复绿。

## PTY 描述符隔离回归

`pty_inheritance` 纳入核心 ctest。先开一个 PTY，再真 exec 一个检测子进程，
验证前一条会话的 master、两条 wakeup fd 和调用方故意未设置 CLOEXEC 的管道
都未泄漏进去；父进程所有 transport fd 也必须设置 CLOEXEC。第二个 PTY 保持
运行时，第一个 PTY 必须在两秒内关闭，避免继承 master 导致 hangup/收尸卡死。
测试失败时先关检测子进程，避免回归用例自己永久挂起；CTest 另有十五秒硬超时。

```bash
python3 scripts/mutation-check-pty-inheritance.py
```

脚本在工程 `tmp/` 的独立 core 副本编译，分别清掉 CLOEXEC、跳过子进程 fd 隔离，
要求目标红而 API 冒烟对照仍绿，随后 SHA256 校验回滚并复绿。
生产 fork/exec 间只使用 async-signal-safe 调用，描述符枚举上限在父进程确定。

## 本地与远端共用 Unix 私有端点

`unix_endpoint_*` 经 ctest 运行十四类真实命名 socket/锁/凭据/fork-exec 用例。
路径夹具在 CMake 中规范化为工程 tmp 的绝对路径；生产接口仍拒绝点组件、
symlink、非私有 leaf 和不可信祖先。`locks` 的子进程在有界等待内返回 BUSY，
阻塞锁变异会被父进程结束并明确断言红，而非等外部 watchdog 冒充有效失败。

```bash
python3 scripts/mutation-check-unix-endpoint.py
```

二十组隔离变异要求目标红、同一变异独立对照绿、起跑标记检查、INT/TERM 后代
清理和 SHA256 回滚。当前运行证据为 macOS arm64，详见
[端点说明](../../docs/reference/session-endpoints.md)。不把独立端点基础当作已接入
生产协议/session/SSH 的 listener，也不据此声明 Linux/Windows 原生支持。

## 本地与远端共用帧连接

`ipc_connection_*` 十七类基线组合共同 decoder/writer 与 Unix socket 适配，包含
真实命名端点请求/响应、实际 socket 发送背压下继续读取、整帧单调 deadline、
回调迟到、双缓冲、取消/关闭寿命、分配回收、EOF/HUP/SIGPIPE。fake stream 只
用于确定性控制分块/clock/IO 错误，不替代上述真实 socket 用例。

```bash
python3 scripts/mutation-check-ipc-connection.py
```

三十组隔离变异要求普通断言红/同变异独立对照绿，不接受编译失败或信号退出
充数；信号测试临时捕获 SIGPIPE 再断言零信号，生产代码不改全局 signal 策略。
连接清除 RX/TX/scratch 和最终整块内存另有 `erasure` 断言及五组变异。
INT/TERM 会回收编译/测试进程组并恢复十三份源码 SHA256。当前证据为 macOS
arm64，仍未覆盖服务循环/session/SSH 接线，见[连接说明](../../docs/reference/session-ipc.md)。

## 本地与远端共用引导握手和 token

`ipc_tokens_*` 十一类与 `ipc_handshake_*` 十八类基线通过 ctest 执行。握手使用
真实 socketpair、共同 connection 与实际 attachment controller；`native` 另走
OS 随机源/单调时钟。包括固定字段/消息状态、能力/兼容/实例拒绝、单次 token、
过期/撤销/预约释放、ACK、旧代隔离、交接后仍未开放输入、整体 deadline、时钟与
熵故障。`clock` 独立关闭 token 服务，防止存储层的检查掩盖握手层时钟缺陷。

```bash
python3 scripts/mutation-check-ipc-bootstrap.py
```

三十四组隔离变异逐条目标红/同变异独立对照绿；计数器耗尽使用初始化为
UINT64_MAX 的隔离源码副本，无生产开关。遗留标记退出 2、实际编译器 INT/TERM
回收和二十四份源码 SHA256 回滚通过。接口/字段与责任转移见
[协议说明](../../docs/reference/session-ipc.md)。本机 macOS arm64 的组件证据不等于
Linux/Windows 原生验证、服务循环、管理命令、SSH 入口或生产恢复已交付。

## 共用服务 owner 与管理请求

`ipc_server_*` 二十四类基线经真实命名 Unix 端点/共同握手，操作实际 manager/
PTY：LIST/INFO/TERMINATE/PING、最大 64 条记录、未知退出保留、跨连接查询、
显式停止、关闭服务仍保留会话、独立客户端 SIGKILL。确定性发送阻塞验证另一
客户端和同流读取继续推进；issuer 撤销、空闲过期、数据交接/失败/租约、旧代
隔离及 constructor admission 错误也单独覆盖。数据 callback 是观测夹具，不是
生产恢复 dispatcher。输入通过 manager 驱动，不宣称已实现 wire INPUT。

```bash
python3 scripts/mutation-check-ipc-server.py
```

三十七组隔离变异要求目标普通断言红/同变异对照绿，含 issuer ID 耗尽，无生产
测试开关；源文件与依赖 archive 的 SHA256 均核验。macOS arm64 与 Ubuntu
24.04 aarch64 完整 gate 222 项/57 公共符号通过；本机 service 24 类及 Linux
service/握手/token/连接 70 类 ASan/UBSan 通过。Linux 使用工程 tmp 下原生
文件系统和受信任的短路径源码副本，避免共享 FS socket chmod EINVAL、祖先
目录 group-write 和 sun_path 限制。新 service 仍是被宿主调度的组件，没有可
安装 daemon/SSH 产品入口；CREATE 组件进展见下一节。详见[服务参考](../../docs/reference/session-service.md)。

## 共用会话创建和回复丢失查询

`creation_*` 十三类基线验证明确 cwd/env/argv、固定大端 schema、最大 8192 字节、
非法 UTF-8/NUL/数量、nonce 重放/冲突/可信 principal、精确过期/永久时钟故障、
宿主分配与工厂/worker 启动拒绝，以及创建结果与当前启动状态的区别。真实 PTY
验证特殊参数不会被二次 shell 解释，关闭 ledger 后仍可输入/输出；原回调与
请求/job 在 manager join/dispose 后才释放，整个拥有的分配在退款前清除。
启动前绑定另验证可信 principal/唯一 ID、绑定先于 worker 工厂、拒绝不启动/
不分配 worker、精确 dispose/槽位释放且 ID 不复用。预启动观察线程证明绑定
回调期间可查询 manager；误持锁用内部有界等待触发普通断言，不靠进程 watchdog。

`ipc_server_create*` 八类基线另经真实命名端点，覆盖只准备一次、阻塞 CREATE
回复后断连再 QUERY 取回原 ID、跨连接/身份、畸形输入零启动、未配置能力拒绝、
实例/manager/RX 准入及固定回复字段。过期的 ledger 原始参数须在无流量 tick
清除，通过宿主 allocator 观察 owned memory；活着的 PTY 不受影响。

```bash
python3 scripts/mutation-check-creation.py
```

四十九组隔离变异逐条目标普通断言红、同变异独立对照绿、源码与依赖 archive
SHA256 回滚。失效数据必须只违反目标约束，不能靠另一条检查仍错误地变绿。
nonce 超过保留期返回 UNKNOWN，客户端不得自动重新 CREATE；不承诺无限期
exactly-once。此模块不代替公开 client、launcher、生产 VT/data dispatcher 或
真实 SSH 代理；详见[创建协议](../../docs/reference/session-create.md)。

## 共用宿主 runtime

`session_runtime_*` 中的九类 runtime 基线组合真实 PTY、creation/manager、引用与 attachment。
验证结果缓存到期不影响原会话，join 后仍保留旧代快照任务所需对象，零引用/
空闲控制器才可 forget/close；并发引用、明确 cwd/env/argv、状态/backend/apply
错误、默认 direct 启动路径的 14 个宿主分配点故障与精确退款另有验证。socketpair 管理服务覆盖
HELLO/CREATE/新连接 QUERY 和服务关闭后原 PTY 继续执行。

`python3 scripts/mutation-check-session-runtime.py`：九类基线/25 组隔离变异，
普通目标断言红、同变异独立对照绿、SHA256 回滚。独立 C 状态模型并非真实 VT
恢复证据，尚无生产 daemon/SSH 代理；详见[宿主 runtime](../../docs/reference/session-host-runtime.md)。

`session_runtime_attach-*` 十二类基线另组合 ATTACH→单次 token→DATA BIND→
DETACH，覆盖失联 issuer、未读回复、单向预约/数据租约过期、身份/重放/旧代
迟到、固定 schema 与构造/熵/时钟/容量失败。原 PTY 继续运行，BIND 后仍封锁
输入。allocator 观察 owner/token store 在退款前清零，服务 scratch 在排入
独立发送缓冲后不再持有 token。

`python3 scripts/mutation-check-session-attach.py`：十二类基线/41 组隔离变异，
只接受普通 FAIL 退出 1，独立对照及 SHA256 回滚须绿。data callback 是观测器，
不冒充 VT/恢复 dispatcher；详见[ATTACH 契约](../../docs/reference/session-attach.md)。

## USER 命令执行许可

USER 执行许可另见 `worker_authorization_*` 六类与 `session_runtime_user-*`
三类：复制 stamp/bytes、无策略拒绝、排队后撤销旧代、恢复/序号/ID/只读闸门、
取消和精确退款。相同错误码的策略拒绝与 backend 失败分别断言 NOT_EXECUTED /
OUTCOME_UNKNOWN，不能折叠。`python3 scripts/mutation-check-user-command.py`
的 34 项隔离变异须普通目标断言红、独立对照绿并 SHA256 回滚。状态模型不是
真实 VT/GUI 恢复证据，详见[USER 契约](../../docs/reference/session-user-commands.md)。

## POSIX PTY 尺寸能力

`pty_dimensions_flow/defaults/admission` 使用本测试程序的真实 exec 子进程
读取 `TIOCGWINSZ`，独立核对 80×24 默认值、像素 0/65535 的精确传递、
65536/UINT32_MAX 和零单元拒绝，以及拒绝后内核尺寸不变。open 的能力检查
还必须早于 allocator/fork。`session_runtime_user-dimensions` 验证同一
宿主检查在 USER 排队前拒绝，不分配/回调/消耗 ID；随后同 ID 的有效 resize
与原 shell 输入仍成功。

`python3 scripts/mutation-check-pty-dimensions.py`：四类基线、13 项变异，
普通 FAIL/exit 1 才算红，同变异独立对照与 SHA256 回滚须绿；起跑扫描残留
标记返回 2，INT/TERM 清理编译子进程并复原独立副本。POSIX backend 与
runtime 源在独立目录编译，平台功能宏与 native 构建一致；不触碰根源码。

## USER 数据协议与异步结果

`ipc_user_wire_*` 四类基线包含独立固定字节、二进制/最大输入、全部截断、
长度/方向/保留字/零 stamp、结果三态矛盾与错误码/拒绝原因支持集。
`session_runtime_wire-*` 七类使用真实 socketpair、HELLO/ATTACH/BIND 与原
PTY：S/L 前拒绝、恢复后输入/resize/color、ID 重放、超限 resize 与 OOM
不消耗 ID、worker 队列满 REJECT、邮箱满撤销连接、断连时旧代排队与新代
接回、backend/state 错误 UNKNOWN、清零退款和配置绑定。S/L 仍由 C 模型
消费者执行，不能替代真实终端引擎恢复。

`python3 scripts/mutation-check-user-data.py` 的 37 项隔离变异只接受普通
FAIL/exit 1，独立对照与 SHA256 回滚须绿。提前释放在途回调的变异必须先
被 BUSY 生命周期断言抓到，崩溃不算红；退休回调校验的构造用例先释放
server claim，不能让另一个失败掩盖目标校验。详见 [USER 契约](../../docs/reference/session-user-commands.md)。

## 终态冻结对象

`worker_final_*` 十类基线验证 backend close→freeze/sink→state close→排队
清理→结果发布，涵盖 EOF/已知序号 0/stop、普通冻结失败/分配失败、NULL 成功
契约失效、启动/backend/state 错误及未启用 sink。返回同一个 INTERNAL 错误，
也不能把普通冻结失败与 NULL 成功造成的 owner 失效折叠。
`session_runtime_final-*` 四类用真实 PTY 核对 FIRST/SECOND 完整观察结果，
join 并覆盖源 model 后冻结副本仍有效；独立引用/重复查询不消费结果，引用
未释放不得 forget，最后精确释放 frozen。close_state 被阻塞时即使已冻结，
query 也不得先于 worker result 返回成功；异常和启动失败保留三态/序号语义。
原 runtime invalid 用例另护 release_frozen 必填。

`python3 scripts/mutation-check-final-state.py`：15 类基线、20 项隔离变异，
目标必须普通 FAIL/exit 1、同变异对照绿并 SHA256 回滚；编译/信号/外部 watchdog 不算
有效红。C 模型和 worker mock 不替代生产 VT/恢复 wire/GUI 证据。

## 共用 DATA 恢复路由

`ipc_data_wire_*` 五类 schema 基线使用固定大端字节、全部截断、方向/保留
值、二进制输出、大小边界、已知零与未知终态，另有 exact 基线证明定向
task reap 不消费另一代已完成的任务。数值上限独立于实现常量；分块超限
测试使用 offset=0，不能被总长度检查遮蔽。

`session_runtime_route-*` 十四类基线经真实 socketpair/PTY、HELLO/ATTACH/
BIND、worker CAPTURE 和 checkpoint encoder 完成整条 DATA 路由。客户端
C 模型实际接收分块并确认 S/L，验证 S<L<final、WAIT_L 持续输出、USER
前后闸门、原 PTY 终态/新代只读接回、未知/已知零/冻结失败，及 PING 不重置
恢复或错误期限。捕获排队、编码在途和终态未发布分别被阻塞，断连须 BUSY
保留 context；空/超限/失败部分 blob 均释放，原 session 继续运行。
非法配置以拒绝 allocator 验证在分配前返回 INVALID，不由内层另一拒绝遮蔽。

`python3 scripts/mutation-check-data-router.py`：20 类基线、47 项隔离变异，
严格普通 FAIL/exit 1、同变异独立对照绿与 SHA256 回滚；编译/信号/外部
watchdog 不算红。客户端/编码器仍为 C 模型，不证明生产 VT/GUI 或 SSH 断网。

## 夹具是「规范」，不是「快照」

`fixtures/` 是 **`ssh_config` 语义的唯一权威**，由真 OpenSSH 的 `ssh -G` 对
`golden.conf` 求值产生。**禁止手改**。要改就改 `golden.conf` 再重跑
`scripts/regen-golden.sh`。

| 文件 | 用途 |
|---|---|
| `golden.conf` | 输入样例（4 个 Host 块 / 6 个模式：通配、否定、多值、跳板） |
| `golden.effective.tsv` | **对拍用**：`host<TAB>key<TAB>value`，多值多行；`#` 行为元数据 |
| `golden.json` | 完整 `ssh -G` dump（人读 / 工具读） |
| `writeback.json` | **编辑 API 的规范**：9 个稀疏编辑用例（原地补丁 / 删除 / 整块重建 / 无损往返），来自归档实现并用真 `ssh -G rc=0` 验证过；**勿手改** |
| `writeback-sample.conf` | 上者的输入样例（与 `writeback.json.sample` 逐字节相同） |

## 单一事实来源链

```
core/src/config.c 的 k_keys 表  ──►  支持集（42 键）
        │                                   │
        │ regen-golden.sh 提取              │ test_config 双向校验
        ▼                                   ▼
golden.effective.tsv 的 # supported_key 元数据
```

**改了 `k_keys` 不重跑 regen ⇒ `test_config` 因元数据过期而红。**
这由 `mutation-check-sshconfig.sh` 的 **A6** 锁住。

## ⚠️ 再生必须钉版本

`ssh -G` 的行为随版本/构建变化，且**同时读系统级 `/etc/ssh/ssh_config`**。
实测证据：

| 环境 | `secret`（无显式 IdentityFile）的默认候选链 |
|---|---|
| `OpenSSH_9.6p1`（上游 `V_9_6_P1` 源码） | 7 条（含 `id_xmss` / `id_dsa`） |
| `OpenSSH_10.3p1`（**当前锚点**，本机实测） | **5 条** |
| 上游 `master` | 6 条（换成 `id_mldsa44_ed25519`） |

所以：

1. **默认候选链是策略，不是常量** —— 内核里的 `gt_identity_policy_t`
   提供 9_6 / 10_3 / MASTER / NONE / CUSTOM 五档，每条链都注明出处；
2. 换个环境再生 = 换语义基线，提交必须显式标注 `BREAKING`，并同步更新
   AGENTS.md 铁律 #4 里的锚点说明；
3. 理想情况应在**干净容器**里再生（避免本机 `/etc/ssh/ssh_config` 污染）。
4. 锚点**不得带再生机器的身份**：`ssh -G` 的默认值里有两处环境相关
   （无 `User` 时的回落登录名、`~` 展开后的 `userknownhostsfile`），
   `regen-golden.sh` 统一映射成中性 token（`goldenuser` / `/home/goldenuser`，
   可用 `GOLDEN_NEUTRAL_USER` / `GOLDEN_NEUTRAL_HOME` 覆盖），并在落盘后
   跑**泄漏守卫**（夹具里搜到真实家目录/登录名即 `exit 1`，不产出脏锚点）。

## 对拍规则（语义等价，不是字符串相等）

`ssh -G` 对布尔**混用两种格式**（`tcpkeepalive yes` vs `pubkeyauthentication true`），
故 `test_config` 做**归一化**后再比：`yes/on/true → yes`，`no/off/false → no`。
多值键（identityfile / localforward / setenv …）以 `\n` 连接后比较。
夹具里**缺席的键 = 空**（`ssh -G` 不输出未设置的 `proxyjump` / `setenv` 等）。

`user` 随**调用者**变化，故夹具记录一个**中性 token**（`goldenuser`，见上），
测试用 `gt_resolve_opts_t.default_user` 钉住；IdentityFile 回落链同样由夹具记录，
测试以 `POLICY_CUSTOM` 注入 —— 这样夹具**自描述且与生成版本解耦**。

## 当前覆盖

| 项 | 覆盖 |
|---|---|
| Host 块 / 模式 | 4 块 / 6 模式（顺序断言） |
| 全表语义对拍 | **42 键 × 6 host = 252 项** |
| 无损往返 | 未编辑路径：**逐字节相同**；编辑路径：**除目标行外逐字节相同**（`sshconfig_writeback`） |
| 编辑语义 | 原地补丁 / 删除 / `=` 分隔符保留 / SetEnv 一行多 token / IdentityFile·LocalForward 一行一条 / reset 整块重建 |
| IdentityFile 回落机制 | 显式条目不追加默认链 / 无显式时按策略回落 / 三档预设条数 |
| `Include` 展开 | 相对包含者目录 / glob / 嵌套 / 无损写回 / 自环深度上限（test_config 第 7 节） |
| 行内注释与 Forward 归一化 | token 开头 `#` 剥离、中间 `#` 保留（ssh -G 实证）/ 括号归一化 / 负差值防护（test_config 第 8 节，SIGBUS 根因回归） |
| **未覆盖** | 编辑后往返、Forward `bind:port` 完整解析（OpenSSH 对 `8080:lh 9090` 输出 `8080 [lh]:9090`，我们原样返回） |

## 脚本

| 脚本 | 用途 |
|---|---|
| `scripts/regen-golden.sh` | 再生两份夹具（**须钉版本环境**） |
| `scripts/probe-multivalue-*.sh` | 多值指令「一行多 token vs 多行」语义探针 |
| `scripts/writeback_check.py` | 用 `fixtures/writeback.json` 驱动 `gtb` 跑编辑 API 对拍（`sshconfig_writeback`） |
| `scripts/ssh_e2e.sh` | 起本机真 sshd（非特权 + 公钥）跑 `test_ssh`（`ssh_e2e`）；无 sshd 显式 SKIP |

## 运行

```bash
bash scripts/gate.sh                                  # 含本目录两项测试
core/build/test_config core/tests/fixtures            # 单独跑对拍
bash scripts/mutation-check-sshconfig.sh              # 证明闸门有牙（14 项）
```

## 私有 Unix attachment 连接生命周期

`test_attachment_peer` 与 ssh_config 对拍独立，经 `scripts/gate.sh` 运行十三类
`attachment_peer_*` 测试。用真 Unix stream socket 覆盖读写/短写/背压、EOF、
旧代回收、HUP 后未接受输入、CLOEXEC、信号下有界超时、错误与参数拒绝；快照测试
复用有界 executor 的真实取消/借出寿命。`kill` 用独立空闲子进程的 SIGKILL，
断连后同一 PTY 继续输入/输出，新代可接管且不被旧 owner 销毁。这不是 GUI 恢复。

```bash
ctest --test-dir core/build -R '^attachment_peer_' --output-on-failure
python3 scripts/mutation-check-attachment-peer.py
```

变异脚本只修改工程 `tmp/` 中的源码副本，十五组均做目标红/独立对照绿，超时
也计失败；起跑扫标记、INT/TERM 清理、SHA256 回滚和十三类最终基线全部必需。
peer 不做身份、token 或 framing；未认证 socket 不得直接接入会话。


## attachment 编码失败与重试

`attachment_failure` 检查编码 FAILED 被 reap 后当前代撤销、恢复队列清空、
原错误保留、输入禁止，随后新代可提交/消费完整快照；`attachment_failure-stale`
检查旧代失败的回收不能撤销新代或抹掉新代任务。二者随核心 gate 运行。
`python3 scripts/mutation-check-attachment.py` 运行十一类基线、十五组变异和
SHA256 回滚。失败任务完成不会自动通知/撤销连接；owner 在等待 take 期间也
必须定期 reap，并将 retired.error 显式传给调用者。


## 私有会话程序状态存储（OSC 7501）

`session_program_status_{copy,clear,aliases,limits,states,invalid}` 验证纯 C 的
owner 记录表。输入是引擎已解码、规范化、转换成 NUL 字符串的报告；不解析 VT、
不做显示文本清洗、不重新推断 app 继承。表按首次插入顺序保存 root/分层 id，
更新保留位置，clear 删除目标与 `/` 分隔的后代，空 id clear 清全表。

回调数据必须复制；deep copy 后源表更新/销毁不能改变快照。测试还覆盖 borrowed
view 原地更新/clear、字符串与条数超额拒绝后旧记录不变、缩短更新归还计费、
未知百分比 -1 与枚举/字段长度拒绝。`python3 scripts/mutation-check-session-program-status.py`
运行六类基线、十二组目标红/对照绿及 SHA256 回滚。该模型尚未接 worker/IPC，
不代表程序摘要已可重连；OSC 133 命令生命周期另待实现。


## 私有命令生命周期摘要（OSC 133）

`session_command_status_*` 六类用例覆盖：output_start/command_end、提示符/输入
不擦上一条命令、未知/负退出码、只收到结束、零耗时/缺时钟/时钟倒退、值副本、
借用自身文本及超长拒绝。`python3 scripts/mutation-check-session-command-status.py`
运行十三组目标红/独立对照绿与 SHA256 回滚。

状态把 running、has_exit_code、has_duration 与 has_start_time 分开保存；未知
不折叠成成功或零耗时。调用者提供单调纳秒及显式有效位，结束没有有效起点或
时钟倒退时耗时保持未知。文本有界复制、不作为 UI 清洗或 wire 编码。当前尚未
接引擎回调、worker 捕获、IPC 或平台 UI。


### 私有 worker 有序配色命令

`worker_scheme_{flow,unsupported,failure,invalid}` 使用真实 PTY 与最小状态模型，
检查 COLOR_SCHEME 的复制、能力拒绝、FIFO 事件/捕获边界、无 backend 写/resize
副作用及失败终态。`python3 scripts/mutation-check-worker-scheme.py` 运行四类
基线与八项隔离变异，目标红/独立对照绿并按 SHA256 回滚。临时编译在工程
`tmp/`，脚本检查变异标记并处理 INT/TERM。该命令未接 VT/GUI 配色适配器，
默认 capability 关闭，不能把模型结果视为 full surface 配色同步证据。
