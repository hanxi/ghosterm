# 专用 SSH 长连接 owner

状态：私有 C 候选组件；Linux 安装组件已接入，自动恢复协调器已验证，原生产品入口尚未交付。
日期：2026-10-21。当前仅推进连接 Linux 宿主，macOS/Windows 远端适配延后。
协议保持 GTSD/v1 严格不兼容；后续格式与兼容讨论不阻塞当前实现。

## 所有权与接口

`core/src/ipc/ssh-link.{h,c}` 消费一个已核验 hostkey、已认证的专用 `gt_ssh_t`，
成功后清空调用方指针。不得留下 shell/SFTP/exec 或原 session 的其他别名。
该连接只由一个非 UI owner 调用；文件面板保持独立 SSH 连接。

两个固定槽分别运行 management 和 DATA exec，均不申请 SSH PTY。代理、daemon、
runtime 使用绝对路径，经 POSIX 单引号引用；控制字符/超长路径明确拒绝，不用
PATH 或拼接未转义的会话输入。返回的 `gt_stream_ops_t` 直接供现有 connection/
bootstrap/control/attachment 客户端使用，不复制另一套协议或恢复状态机。

认证由已有 SSH 路径完成；该 owner 不替调用方接受未知 hostkey、不保存用于
重试的私钥，不负责平台安全存储和认证 UI。所有异步协议对象必须先销毁，再
关闭 owner；必须在进程/加密库全局清理前完成显式关闭，不能依赖 atexit 顺序。

## 有界双向流与关闭

每槽 TX/RX 各 64 KiB，构造时一次准入；两槽轮换，每轮各尝试一次 stdout/
stderr 读取和一个 libssh2 出站操作。stdout 仅交给协议；stderr 持续有限排空并
计数，内容擦除且不进终端或日志。队列满则停读/拒绝新写，不无限增长。

一个独立 4096 字节缓冲保存正在发送的 SSH payload。EAGAIN 时保留相同地址、
长度和内容，先续完原操作，再切到其他出站操作；短写只退掉实际发送字节。
打开 channel、exec 和关闭/释放也按状态续行。所有这些调用采用 nonblocking
模式，不在 owner 中等待远端网络。上述额度不包含 libssh2/crypto 内部缓冲。

流 close 只撤销本地连接，已交给 SSH 的在途输入可能结果未知，不重放、不
terminate 远端 session。cancel 先 shutdown 专用 socket，使释放不依赖远端
回复；owner 释放后才归还 fd，防止重用的描述符被加密库误写。

远端 stdout EOF 要结合 channel EOF 判定，单次 read 返回 0 仍可能只是暂时没有
payload。终态可以排空已收到的 stdout。TCP HUP/错误则使整个连接失效并擦除
排队字节。exec 拒绝显式 UNSUPPORTED，网络错误不能伪装成正常 EOF。

libssh2 的 exit-status 查询不能区分“未收到状态”与默认 0，所以本组件对 0
保留 unknown，只把实际非零退出状态标为已知；不会编造远端成功。后续若要
精确报告正常 exit 0，必须补有证据的状态存在性接缝，而非放宽三态要求。

## 验证与边界

```bash
ctest --test-dir core/build -R '^ssh_link_' --output-on-failure
ctest --test-dir tmp/sessiond-build -R '^ssh_session_flow$' --output-on-failure
python3 scripts/mutation-check-ssh-link.py
```

真实 sshd 验证双 exec 的二进制字节保真、两路公平/背压、stderr 洪泛排空、路径
引用、错误退出和取消；十项隔离变异要求普通 FAIL/exit 1 与同变异独立绿对照。
未编入 libssh2 时另测 UNSUPPORTED，不能把该档结果算成真实 SSH 通过。

daemon/真实 PTY/VT 组合经 exec 代理完成 CREATE、ATTACH、快照/有序增量和
输入屏障。客户端进程 SIGKILL 后，父进程重新建立 TCP/SSH 并认证，以原
instance 查询原 session，接回原 PID 并继续输入，结束后只读恢复。Mac/Linux
各自的本机 sshd 组合不等于 macOS→Linux 的跨 OS 部署或真实断网证据。

Linux 发布包与 SFTP 安装已在 macOS→Linux 验证，见
[部署事务与证据](session-linux-deploy.md)。共用自动恢复协调器已通过真实 TCP reset、
静默丢包、旧代租约等待和原 PID 恢复，见 [协调器](session-client.md)。生产认证
provider、公开 session ABI 与原生界面接线继续实施；P0 与完整 Linux 流程仍未完成。
后续顺序遵循 [Linux 全流程路线](plan-session-daemon.md)。

### 代理拒绝原因

共享私有 `proxy-status.h` 固定现有 `64 + activation_rc` 退出码，既用于远端
SSH owner，也供本地进程 adapter 读取。实际收到版本/能力不匹配时返回
UNSUPPORTED，instance 不匹配返回 PROTOCOL，缺失 helper 返回 NOT_FOUND，
认证拒绝返回 AUTH；这些错误终止本次恢复，不能降格成网络重试。
后续 step 保留首个错误原因，取消保留 CANCELLED。代理 stderr 仍只诊断，
stdout 不插入文本或新增帧。

零/缺失状态保持 unknown；非契约退出码保留原非零状态，不能误解为契约拒绝。
真实 sshd 覆盖十种契约错误、0 与 127，新增两组变异分别破坏识别和错误保存，
均为普通断言失败且同变异 known_hosts 对照通过。核心 387 项与 57 ABI 闸门通过。

主动关闭的 stream 只记录代理退出状态，不再把其 CANCELLED 等退出码升级为整条
SSH 失败；仍在线的代理拒绝继续立即失败。真实 sshd 增加主动 detach 后非零
退出对照，删除 detach 边界的变异会普通断言失败，独立 known_hosts 对照仍绿。
