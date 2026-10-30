# core/tests — 测试与黄金夹具

## 加密私钥认证重试

`ssh_connector_e2e` 使用临时加密 OpenSSH 私钥，在真实 sshd 上验证错误口令返回凭据请求、替换正确口令后同一连接认证成功。夹具仅在工程 `tmp/`，结束后删除，不输出材料或口令。libssh2/OpenSSL 的内存私钥解析可能返回 `LIBSSH2_ERROR_FILE`，仅认证私钥路径将它作为 `GT_ERR_AUTH` 重试；握手/协议错误不因此降级。该回归随现有 Core 闸门执行，不增加公共 ABI。

`python3 scripts/mutation-check-ssh-core.py credentials` 在隔离副本撤去这一分类，真实认证目标红、配置对照绿、恢复绿并核对 SHA256。`mutation-check-ssh-connector.py` 的既有 18 组另保护取消、deadline、信任与所有权；Android 另有真实产品凭据路径和七组设备变异，证据及历史失败见[实现记录 §9.31](../../docs/reference/android-implementation.md#931-加密私钥重试与密码产品流程2026-10-30)。

## SFTP 现有验证的边界

真实 sshd 的正常传输、文件操作、暂停/取消与既有变异不覆盖以下全部失败语义。2026-10-30 源码复核确认，仍须补实现和故障回归：

- `job_download` 在打开远端文件前用 `wb` 打开本地目标；远端打开失败可能已经截断已有文件。暂存后提交和非覆盖创建尚无完整契约。
- 上传/下载没有检查 `libssh2_sftp_close` 与本地 `fclose` 的结果；已读取/写入的字节数不证明缓冲关闭或目标提交成功。
- `gt_sftp_walk` 仅裁剪大于 64 的正深度，负深度的递归条件没有硬上限；头文件注释所称“仍有编译期护栏”不代表实现已兑现。`remove_all` 有独立深度检查，不能将两者混为一谈。
- 单 worker/FIFO 是每个 `gt_sftp_t` 的约束，不是跨连接的全局限并发；暂停中的当前 job 仍占用该会话 worker。

本节只是源码覆盖核对，不新增已通过的测试数量；失败场景、Apple 调用方与 Android A5/A6 前置见[路线图](../../docs/reference/roadmap.md#macos-下一步与共享缺口)。

## SFTP 打开草稿的失败边界

`2852d6c` 后未提交工作区新增 `test_sftp_open` / `sftp_open_e2e`，后者只验证正常 OpenSSH 打开、预先取消、零预算与真实目录，Core CTest 数量为 39。独立 `scripts/test-sftp-open.py` 才包含故障协议矩阵；初始 13 项通过后新增部分 VERSION 后 EOF，第 14 项超时。后续 §9.45 的隔离修复库上 17 项通过、Android 依赖六场景通过，但 macOS 默认依赖尚未切换。因此 Core gate 通过不等于该草稿验收通过，也不能把此前十组新变异记为当前 14 项基线通过。

当前公开 ABI 为 82 项，Apple 80/2、JNI 40/42；两端文件产品尚未消费该新入口。EOF 有界返回、部分初始化缓冲释放及后续阻塞操作仍待修复/验证。复现命令、既有八组 SFTP 变异和失败证据见 [实现计划 §9.44](../../docs/reference/android-implementation.md#944-sftp-打开前置与跨端工作区复核2026-10-30未验收)。这些新增脚本仍属工作区草稿，不称为已提交的 CI 闸门。

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

## PTY 描述符与信号屏蔽隔离回归

`pty_inheritance` 纳入核心 ctest。先开一个 PTY，再真 exec 一个检测子进程，
验证前一条会话的 master、两条 wakeup fd 和调用方故意未设置 CLOEXEC 的管道
都未泄漏进去；父进程所有 transport fd 也必须设置 CLOEXEC。第二个 PTY 保持
运行时，第一个 PTY 必须在两秒内关闭，避免继承 master 导致 hangup/收尸卡死。
测试失败时先关检测子进程，避免回归用例自己永久挂起；CTest 另有十五秒硬超时。

另在调用线程屏蔽 SIGWINCH/SIGUSR1 后启动真实 exec 子进程，验证子进程不继承
屏蔽集合、调用线程的集合不变；连续放大/缩小后，由子进程的 SIGWINCH handler
唤醒并回报新尺寸，不发送任何输入。只查 `stty size` 不能证明通知送达。

```bash
python3 scripts/mutation-check-pty-inheritance.py
```

脚本在工程 `tmp/` 的独立 core 副本编译，三组分别清掉 CLOEXEC、跳过子进程 fd 隔离、保留继承的信号屏蔽，
要求目标红而 API 冒烟对照仍绿，随后 SHA256 校验回滚并复绿。
生产 fork/exec 间只使用 async-signal-safe 调用，描述符枚举上限在父进程确定。

macOS `TmuxResizeTests` 另经真实窗口/full surface、本地 shell 和系统 ssh，验证
tmux 客户端与内层窗格在放大/缩小时自动跟随，期间不敲键、不切窗口，内层 PID 保持。
`bash scripts/apple-app-gate.sh` 会提供隔离 loopback sshd；未提供时 SSH 用例明确跳过。
向变异脚本传 `--apple` 并提供同样的 `GHOSTERM_SSH_HOST=127.0.0.1`、PORT/USER/KEY
环境，可在隔离 Swift/core 副本验证信号变异使这两条原生用例失败、HostListTests
对照通过、恢复后通过；缺夹具不计通过。

## POSIX PTY 尺寸能力

`pty_dimensions_flow/defaults/admission` 用真实 exec 子进程读取 `TIOCGWINSZ`，验证
80×24 默认值、像素 0/65535 精确传递、65536/UINT32_MAX 和零单元拒绝，拒绝后
内核尺寸保持不变；open 检查早于 allocator/fork。`mutation-check-pty-dimensions.py`
保留三类普通 PTY 基线与十组隔离变异，目标普通断言红、同变异对照绿并核对 SHA256。
旧 USER/runtime 协议的三组变异随能力整体退役。

## 通用 SSH 认证

`ssh_connector_*` 保留非阻塞连接、总期限、hostkey 三态、真实 sshd 与材料清理验证。
`ssh_auth_bridge` 验证公开 `ghosterm/ssh_auth.h` 的翻译、凭据与连接所有权。
运行 `python3 scripts/mutation-check-ssh-auth.py` 验证八组公开翻译变异，
`python3 scripts/mutation-check-ssh-credentials.py` 验证六组原生凭据选择变异。
它们不依赖旧 daemon、安装器、记录或私有协议；临时文件均位于工程 `tmp/`。

## SSH 零超时读取

Transport 的 `timeout_ms=0` 表示轮询；libssh2 的 timeout 0 表示无限等待，不能直接转发。`gt__ssh_shell_read` 在零超时时暂时使用非阻塞模式，返回前恢复 blocking/timeout；TIMEOUT 与 EOF 仍分开。Android native owner 的输入后轮询触发了这条共享缺陷，页面旋转后再输入也可能表现为不刷新。

`test_ssh` 用远端 `read` 屏障保持静默，零超时必须返回 TIMEOUT，随后输入可解除屏障并正常读取；10 秒 alarm 是阻塞故障的退出守卫，不是性能目标。`mutation-check-ssh.sh` 现在有五组隔离变异，新增 `zero-poll` 回到 libssh2 的无限等待时定向失败，同变异配置对照绿，恢复后绿与 SHA256 一致。TOFU 变异已随写入错误传播改为“跳过写入但返回成功”，不以编译错误作为目标红。

## 主机信任与撤销

`known_hosts_e2e` / `known_hosts_ecdsa` 使用隔离的真实 sshd，分别以 ed25519 与 ECDSA
主机密钥验证同步握手和公开异步认证；不是纯离线字符串测试。

| 输入 | 当前行为 |
|---|---|
| 明文主机列表 | 匹配普通条目及 `*` / `?`，命中的 `!` 否定项排除该行；保留原有裸名及 `[host]:port` 匹配方式 |
| 匹配主机与密钥的 `@revoked` | `GT_ERR_HOST_KEY`；异步状态为 `GT_SSH_AUTH_HOST_REVOKED`；无论普通信任记录在前或在后，都不进入确认/凭据阶段 |
| 其他主机或其他密钥的撤销 | 不撤销当前密钥；测试保留正常普通信任对照 |
| 当前主机的 `@cert-authority` | 不当作普通信任；无明确匹配普通记录时返回 `GT_ERR_UNSUPPORTED`；若普通记录匹配，信任依据仅为该普通记录 |
| 当前主机的未知标记 | `GT_ERR_UNSUPPORTED`，不提示接受为新主机 |
| 已解析的 hashed 条目 | `GT_ERR_UNSUPPORTED`，即使另有普通记录匹配。尚无法判断哈希是否属于当前主机，不能跳过潜在撤销；这不是哈希匹配实现 |
| 文件不存在 / 读取失败 | ENOENT 继续未知主机流程；其他打开、读取、关闭错误返回 `GT_ERR_IO`，不当作空信任库 |

`bash scripts/mutation-check-known-hosts.sh` 调用 Python runner，在工程 `tmp/` 的独立
Core 副本中运行 **25 组**变异；每次清洁构建，真实 SSH 项必须实际启动两类 sshd，SKIP/缺测试
结束标记直接失败。目标断言红、同变异配置求值对照绿、复原测试绿并核对 SHA256。
覆盖旧算法过滤/多记录/裸名规则，以及标记保留、撤销优先、CA/未知/哈希拒绝、
通配/否定/其他密钥、读取失败和同步/异步路由；K16/K18/K19 继续护真实保存失败与调用方阻断，K17/K20 随写入重构迁至真实文件事务的关闭/打开故障注入，仍有独立配置对照，不再拿 RLIMIT 写失败冒充 fclose 错误。K21–K25 护确认期间重新验证、并发重复记录、撤销/冲突状态及记录上限。该脚本仍是手工闸门，未纳入 macOS CI 20 份。

边界：这不是完整 OpenSSH known_hosts 解析器，畸形行仍可能按现有形状过滤跳过；
裸名匹配任意端口是沿用的现有契约，本轮没有进行 OpenSSH 端口语义对拍。
未实现哈希匹配与主机证书验证。TOFU 现在与 `gt_known_hosts_remove` 使用共享事务：
稳定锁与进程内互斥、锁内重新读取、完整备份、暂存/同步/原子替换。等待确认期间新增的撤销、
密钥冲突或不支持规则阻断认证；另一连接已保存相同记录则不重复追加。公开移除接口只接受普通
非哈希记录，整份字节快照不符时拒绝；结果区分冲突、未提交以及已提交后发生错误。
真实缺失父目录及 `RLIMIT_FSIZE` 仍验保存失败阻断，关闭错误另以独立 syscall 故障注入验证。
两个真实 SSH owner 同时接受同/不同身份，以及 TOFU 与移除相撞，均检查无记录丢失。

`known_hosts_store` 用生产事务代码及真实文件验证 20 种独立故障、原始字节保真、备份权限、
符号/硬链接和 FIFO 拒绝、过期快照、外部变更、线程与进程锁竞争和有界锁等待。
`python3 scripts/mutation-check-known-hosts-store.py --apple` 提供 17 组 C、2 组 Swift 变异，
每组编译后目标红、独立配置对照绿、恢复绿与 SHA256；JNI/Kotlin 同类证据在 Android runner。
故障仅注入测试编译单元，生产库不包含故障开关；真实认证套件另外经过实际链接的 Core 库。

写侧要求绝对路径，拒绝目标和最终父目录的符号链接及非普通/非当前用户/多硬链接文件；
不自动创建缺失父目录。备份与稳定锁留在目标目录，暂不自动清理历史备份。250ms 只约束锁等待，
不约束阻塞文件系统调用；所有平台必须在后台 owner 调用。非协作写者仍存在检查到 rename 的竞态，
不声称任意外部编辑无损或已验证断电恢复。同步/回读失败发生在 rename 之后时，不能显示“原文件未改”。

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
| 配置读取错误 | `sshconfig_read_error` 以目录触发真实 fread 错误，必须返回失败；ENOENT 仍为合法空配置。`mutation-check-sshconfig.sh` A9 验目标红、同变异 golden 绿与复原绿 |
| 无损往返 | 未编辑路径：**逐字节相同**；编辑路径：`sshconfig_writeback` 的 9 个既有用例 + 5 个补充用例（`=`、非连续重复指令替换/删除、多值增减与后续编辑）逐字节对拍；不能扩大为全部配置形态已覆盖 |
| 编辑语义 | 原地补丁 / 删除 / `=` 分隔符保留 / SetEnv 一行多 token / IdentityFile·LocalForward 一行一条 / reset 整块重建 |
| IdentityFile 回落机制 | 显式条目不追加默认链 / 无显式时按策略回落 / 三档预设条数 |
| `Include` 展开 | 相对路径以 **`~/.ssh`** 为基准，与包含者所在目录无关；glob / 嵌套 / 无损写回 / 自环深度上限（test_config 第 7 节）；Core 已先展开 tilde，再调用 POSIX glob，Android 也走同一语义 |
| 行内注释与 Forward 归一化 | token 开头 `#` 剥离、中间 `#` 保留（ssh -G 实证）/ 括号归一化 / 负差值防护（test_config 第 8 节，SIGBUS 根因回归） |
| 非连续重复指令 | 已按实际匹配行删除，替换保留首条的位置/缩进/拼写/分隔符；其他指令、注释、CRLF 和相邻块保持原字节。B7/B8 回退连续 splice 验目标红、同变异求值对照绿、复原绿与 SHA256 |
| 字段来源与归属 | `sshconfig_metadata` 覆盖显式/继承/默认、包含文件来源、同名块拒绝编辑及重解析后来源；`mutation-check-config-metadata.py` 用 8 组隔离 C 变异验证目标红/API 对照绿/恢复绿与 SHA256，`--apple` 另验 Swift 绑定转换；见 [Android §9.5](../../docs/reference/android-implementation.md#95-字段来源与编辑归属2026-10-29) |
| IdentityFile 累积来源 | `sshconfig_identities` 覆盖源码顺序、引号/UTF-8、显式/继承、否定匹配、共享块/重复块、Include、编辑后重解析和无默认项；Match/相关参数错误整体拒绝。`mutation-check-config-identities.py --apple` 验 8 C + 1 Swift 隔离变异；JNI/Kotlin 另有 3 组设备变异，见 [Android §9.34](../../docs/reference/android-implementation.md#934-identityfile-来源查询与平台绑定2026-10-30)。新增接口只读，不承担配置写盘或密钥选择 |
| 别名重命名 | `sshconfig_rename` 覆盖仅替换目标 token、引号/转义/CRLF/无末尾换行、同块别名保留、大小写冲突、包含文件/歧义拒绝、重解析失败保留原对象。12 组 C 与 1 组 Swift 隔离变异通过，Android 改名切片验收见 [§9.9](../../docs/reference/android-implementation.md#99-主机别名重命名2026-10-30) |
| **已知缺口** | 标量来源查询覆盖三个连接字段，IdentityFile 另有累积条目查询，macOS 表单均尚未消费；完整块指令视图未接入，macOS GUI 尚无别名改名入口。Forward `bind:port` 完整解析仍未实现（OpenSSH 对 `8080:lh 9090` 输出 `8080 [lh]:9090`，我们原样返回） |

## 脚本

| 脚本 | 用途 |
|---|---|
| `scripts/regen-golden.sh` | 再生两份夹具（**须钉版本环境**） |
| `scripts/probe-multivalue-*.sh` | 多值指令「一行多 token vs 多行」语义探针 |
| `scripts/writeback_check.py` | 用 `fixtures/writeback.json` 驱动 `gtb` 跑编辑 API 对拍（`sshconfig_writeback`） |
| `scripts/ssh_e2e.sh` | 起本机真 sshd（非特权 + 公钥）跑 `test_ssh`（`ssh_e2e`）；无 sshd 显式 SKIP |

## 运行

```bash
bash scripts/gate.sh                                  # 配置 / PTY / SSH / 公开头
core/build/test_config core/tests/fixtures            # 单独跑对拍
bash scripts/mutation-check-sshconfig.sh              # 17 组：A1–A9、B1–B8
bash scripts/mutation-check-include.sh                # Include 四组变异
python3 scripts/mutation-check-config-rename.py --apple # 重命名 12 C + 1 Swift 变异
```

以上是宿主 C 测试，不能当作 Android 设备证据。Android A1 另有配置事务与页面测试、
`mutation-check-android-config.py` 当前 59 组通过记录包含别名重命名、配置错误恢复、真实 SAF 导入与表单校验，不能继续把最初十二组称为当前覆盖。跨端结果见[路线图](../../docs/reference/roadmap.md)。

## 可选 tmux

普通 core gate 不依赖安装 tmux：`tmux_command` 与 `tmux_adversarial` 验证有界执行、
输出上限、严格解析、缺失/权限、版本、策略与变更结果三态。合并 tmux 改动另跑：

```bash
python3 scripts/test-tmux.py
python3 scripts/mutation-check-tmux.py
python3 scripts/test-tmux-ssh.py --host HOST --port PORT --user USER --key KEY --remote-root /absolute/project/tmp
```

前两条要求真 tmux >=3.2a（`TMUX_BINARY` 可指定绝对路径）；SSH 测试要求隔离账户和远端
工程临时目录，管理与 attach 都由被测 libssh2 实现。系统 ssh 仅用于布置/清理测试配置。
八模式覆盖 lifecycle/names/identity/policy/attach/race/empty/owner-kill；测试只终止自己的 socket，
不调用用户默认实例。变异脚本的编译、目标及对照都经 `gt_timeout`，不把编译失败当断言红。
SSH 故障模式可在命令末追加 `reset blackhole backpressure lost-create lost-terminate query-timeout query-cancel`。
同一 SSH 参数传给 `scripts/mutation-check-tmux-ssh.py` 可跑六组变异；
PTY 拒绝、exec 拒绝和仅 SFTP 账户另用 `--key` 与 `--setup-key` 指定受限/布置密钥，
分别运行 `pty-denied` / `exec-denied` / `sftp-only`。受限账户不会假装支持交互终端。
关闭标准描述符和无 wait status 的本地子进程也在离线命令测试中覆盖；缺 tmux、
旧版本与启动输出污染以类型化状态区分，Swift 绑定及原生列表均验证此口径。
macOS make 按整秒比较时间戳时，快速变异可能留下旧可执行文件，故强制删除受影响 object、
archive 与测试可执行再重链；回滚同样执行，并核对原始 SHA256。

`names` 同时验证 tmux 3.6a 与 3.7c 的实际名称行为：3.6a 规范化控制字符及点/冒号；
3.7c 可拒绝控制字符并保留点/冒号。明确拒绝必须返回 REJECTED，且原会话名称不变。

## 连接配置准入

`connection_config` 是普通 SSH 连接的显式配置能力检查；与 42 键求值 golden 分开记账。`gt_config_check_connection` 只读 AST，保留来源/原字节，不执行 DNS 或 Match 命令；首值优先、累积列表、Host 否定与 Include 来源有独立断言。当前允许值、未知/Match 的保守拒绝及两端调用边界见[实现计划 §9.24](../../docs/reference/android-implementation.md#924-共享连接配置准入与两端接线2026-10-30)。通过不代表完整 OpenSSH 兼容或 Android SSH 已接通。

本切片离线 gate 为 **35 项 CTest / 74 ABI**；`python3 scripts/mutation-check-connection-config.py` 在隔离副本运行 14 组，每组清洁构建后具体目标断言红、同变异 `test_api` 绿、恢复绿与 SHA256 一致。macOS 产品路由另由 auth 11 组验证，Android 配置桥/按钮另有五组设备变异；不是用 C 测试替代平台行为。本脚本为手工闸门，未新增 CI 脚本数量。

### 主机信任记录快照

`known_hosts_snapshot` 验证共享只读 `gt_known_hosts_*`：保留 marker、原始 UTF-8 字节范围/CRLF/末行无换行、哈希标志和畸形记录；注释不成为记录，NUL/大小/条目限额错误不变成空列表。返回字段形态不是信任决定，也未验证 base64 中的公钥。认证的 `gt__kh_visit` 与快照共用词法读取，既有撤销/未知规则/错误传播继续由真实 sshd 套件验收。

`python3 scripts/mutation-check-known-hosts-snapshot.py --apple` 跑十组隔离 C 与一组 Swift 变异，目标红、独立 API/配置往返对照绿、恢复绿与 SHA256。Android 的只读切片为四组 JNI/Kotlin 设备变异；后续删除绑定加入四组，当前 runner 共八组。它们不增加 CI 20 份覆盖，不能作为文件移除/并发提交或产品页面完成的证据；记录见[Android §9.36](../../docs/reference/android-implementation.md#936-主机信任记录快照与平台绑定2026-10-30)。

## 可取消的普通 SSH shell 准入

`gt_transport_open_ssh_shell_controlled` 使用正数剩余预算，零值不回落到默认超时；取消为 CANCELLED，超时/IO 为 IO，PTY/shell 明确拒绝为 UNSUPPORTED。既有 `gt_transport_open_ssh_shell` 和 exec 入口仍保留自身契约。调用方使用专有 IO owner，失败后丢弃连接；预算检查不构成对锁等待或清理的硬实时承诺。

- `python3 scripts/mutation-check-ssh-shell.py`：隔离 C 参数状态与真实 sshd 的 Swift 取消/预算/PTY 尺寸，1 C + 3 Swift 变异，独立配置对照。
- `python3 scripts/mutation-check-ssh-shell-unavailable.py`：真实无 libssh2 archive 的 UNSUPPORTED；变异状态码目标红、独立 API 对照绿、恢复绿与 SHA256。
- Android 阶段停滞、明确拒绝/断线区别与 owner 生命周期用协议故障端验证，正常 PTY/PID 用 OpenSSH；入口和范围见 [Android §9.41](../../docs/reference/android-implementation.md#941-ssh-通道打开预算与取消2026-10-30)。

这些是手工验证，不新增 CI 覆盖。共享通道代码变更还需回归既有 SSH/exec/tmux，不能以新接口定向替代旧入口。
