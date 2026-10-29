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
| 无损往返 | 未编辑路径：**逐字节相同**；编辑路径：`sshconfig_writeback` 的 9 个既有用例验证目标行以外逐字相同，不能扩大为全部配置形态已覆盖 |
| 编辑语义 | 原地补丁 / 删除 / `=` 分隔符保留 / SetEnv 一行多 token / IdentityFile·LocalForward 一行一条 / reset 整块重建 |
| IdentityFile 回落机制 | 显式条目不追加默认链 / 无显式时按策略回落 / 三档预设条数 |
| `Include` 展开 | 相对路径以 **`~/.ssh`** 为基准，与包含者所在目录无关；glob / 嵌套 / 无损写回 / 自环深度上限（test_config 第 7 节）；Core 已先展开 tilde，再调用 POSIX glob，Android 也走同一语义 |
| 行内注释与 Forward 归一化 | token 开头 `#` 剥离、中间 `#` 保留（ssh -G 实证）/ 括号归一化 / 负差值防护（test_config 第 8 节，SIGBUS 根因回归） |
| **已知缺口** | 非连续重复指令编辑会按匹配数量连续 splice，可能误删中间无关行；Android 有调用方拒绝保护，Core/Apple 未修复。Forward `bind:port` 完整解析仍未实现（OpenSSH 对 `8080:lh 9090` 输出 `8080 [lh]:9090`，我们原样返回） |

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
bash scripts/mutation-check-sshconfig.sh              # 15 组：A1–A9、B1–B6
bash scripts/mutation-check-include.sh                # Include 四组变异
```

以上是宿主 C 测试，不能当作 Android 设备证据。Android A1 另有配置事务与页面测试、
`mutation-check-android-config.py` 十一组变异；跨端当前结果见[路线图](../../docs/reference/roadmap.md)。

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
