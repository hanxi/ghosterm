# 可取消的 SSH 认证连接器

状态：2026-10-21，私有 C 组件已通过真实 sshd 与超时/取消验证；共用
SSH provider 已接入，继续公开 ABI 和原生异步 DNS/平台凭据入口。当前没有旧版本协商。

## 所有权与步骤

`core/src/ipc/ssh-connector.h` 定义接口，实现在 `core/src/ssh.c`，复用既有
SSH 对象、known_hosts 与认证材料。成功 open 接管一个全新的、已设置解锁凭据的
SSH 对象；失败不接管。只有一个非 UI owner 调用，既不创建线程，也不在后台
遗留无法取消的 DNS/认证 worker。

调用方另外提供已解析的 IPv4/IPv6 地址和 scope。连接器不调用 DNS；原生解析
必须异步、可取消，且包含在上层恢复总期限内。网络地址仅用于 connect，主机密钥
身份仍取原 SSH host/port，不能用解析后的 IP 替换 known_hosts 身份。

流程是 TCP 非阻塞连接、SSH 握手、主机密钥检查、认证方式探测、userauth，每次
step 做有界的非阻塞尝试。EAGAIN 保留同一对象/参数继续原操作。一次单调时钟
期限覆盖 TCP 到认证完成，等待主机密钥确认也不续期；回退/失败明确终止。

主机密钥有独立状态：未检查、已知、未知、变更。已知匹配继续；变更立即失败；
未知停在 HOSTKEY，供原生低频确认 UI 处理。这里不调用既有同步 decider，避免
在 IO owner 内等待 UI。UNKNOWN 决定仍等待，REJECT 明确失败，ACCEPT 先写入
known_hosts，再继续认证；写回失败不能假装信任已保存。

READY 只能 take 一次，将已认证 SSH 交给专用双通道 owner。libssh2 可恢复其
blocking 模式，但 OS socket 必须继续 NONBLOCK：手工把 socket 改为阻塞会绕过
libssh2 的超时等待，真实 exec 读取曾因此卡在 recv。socket 始终 CLOEXEC。
取消先 shutdown 仍保留的 fd，再以固定极短释放预算清理 session，最后归还 fd；
不等待远端认证回复，不启动 PTY 或 daemon。对象、认证材料在所属 owner 清理。

## 同步 SSH 路径的补充修复

旧路径以“session 指针非空”判断握手幂等，失败的 hostkey 检查也会留下该指针。
现在另存已完成核验标志；失败握手再次调用不能返回成功，authenticate 也要求
已核验，防止绕过 hostkey 拒绝。新连接器在核验与方式探测完成后同步设置标志。

密码、私钥与口令的旧副本在替换/关闭前擦除。擦除测试只观察六个固定的非密钥
标签，不记录真实私钥内容。SHA256 指纹去掉 Base64 填充，已与独立
`ssh-keygen -lf ... -E sha256` 对拍；修正同时用于旧同步路径。

## 证据

```bash
bash scripts/gate.sh
ctest --test-dir core/build -R '^ssh_connector_' --output-on-failure
python3 scripts/mutation-check-ssh-connector.py
```

macOS/Linux 核心 374 项、57 公共 ABI 符号；关闭 libssh2 时显式 UNSUPPORTED。
七类基线包含本机 TCP 黑洞真实期限、取消/时钟、准入、材料擦除与真实 sshd。
真实 SSH 验证未知等待/确认、已知不弹窗、拒绝/变更、认证失败、精确指纹、
确认期间期限、认证对象交接后的命令输出及旧握手拒绝后的认证隔离。

18 组隔离变异要求普通 FAIL/exit 1、同变异绿对照、SHA256 回滚，覆盖期限、时钟、
socket NONBLOCK/CLOEXEC、地址/信任身份、取消、未知/变更信任、写回、认证结果、
准入、材料/owner 擦除、旧路径核验及指纹填充。macOS ASan/UBSan、Linux
ASan/UBSan/LSan 通过。macOS 不支持 LSan，不将它算作通过。

上层状态机、安装及真实网络恢复分别见 [协调器](session-client.md)、
[部署](session-linux-deploy.md)、[双通道 owner](session-ssh-link.md)。
本组件的已解析地址不冒充原生 DNS 已交付，后者继续在平台接线阶段完成。
