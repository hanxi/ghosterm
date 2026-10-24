# 本地 stdio 代理 provider

`core/src/ipc/local-session-provider.{h,c}` 将本地进程 IO 接到与 SSH 相同的
session-client。管理和 DATA 各一条 Unix socketpair，通过 posix_spawn 将子端接到
同一 session-proxy 的 stdin/stdout；不请求额外端口，不新增本地恢复协议。
绝对路径直接作为 argv，调用方路径在 open 时深复制，不经 shell 拼接。

每次 step 只做非阻塞 IO 与 WNOHANG waitpid。每条 stderr 每轮最多排空 4096
字节，不混进终端数据、不进入 UI 事件队列。终端字节直接使用原 socket-stream，
没有另一套积压队列。代理的现有退出契约保留具体错误，版本/实例拒绝立即失败。

子进程只继承三个标准描述符：macOS 使用 CLOEXEC_DEFAULT，glibc 2.34+ 使用
spawn closefrom。其他缺少等价隔离能力的平台显式 UNSUPPORTED，不能假装已支持。
stdin/stdout 的子端保持普通流，父端采用 NONBLOCK/CLOEXEC，fd 小于 3 时先移出
标准描述符范围。调用方不得启用 SIGCHLD 自动回收或由其他 waiter 回收这些子进程。

reset 在协议流已销毁后关闭代理 stdio，只向本 provider 拥有、尚未回收的代理
PID 发取消信号；250ms 后仍不退出则升级 SIGKILL，最后用 WNOHANG 回收。
不向进程组或 daemon 发信号，不结束 PTY/session。重试等待前一代代理回收，
固定最多两个代理；destroy 在仍有子进程/活动借用时拒绝释放。宿主必须保留 owner，
在非 UI 线程继续 quiesce，不能把 wait/join 放到主线程。

## 证据

```bash
ctest --test-dir core/build -R '^local_provider_' --output-on-failure
python3 scripts/mutation-check-local-provider.py
ctest --test-dir tmp/session-host-build -R '^session_client_local_proxy_flow$' --output-on-failure
```

八类基线覆盖 argv 深复制、双角色二进制回显、stderr 洪泛、任意调用方 fd 隔离、
具体退出错误、缺失 helper、忽略 TERM 的代理升级回收、重试与时钟失败。
十一组隔离变异均要求普通 FAIL/exit 1、同变异独立绿对照与 SHA256 回滚；
标记预检 exit 2、INT/TERM 后代清理已通过。macOS/Linux 基线和变异通过，
macOS ASan/UBSan 与 Linux ASan/UBSan/LSan 通过。
macOS/Linux 核心 395 项、daemon 447 项和 57 公共 ABI 闸门通过。

真实 daemon/PTY/VT 经双代理执行 CREATE/ATTACH/恢复屏障，故障后同 instance、
session、PID 和更高 generation，继续输入并取得终态；没有只靠回显冒充恢复。
这尚未接入原生产品入口；共用公开 ABI、平台绑定和 UI 交换流程继续实施。
