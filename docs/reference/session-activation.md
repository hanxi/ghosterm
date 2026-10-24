# 共用后台启动与 stdio 代理

日期：2026-10-21。候选实现适用于 macOS 与 glibc ≥ 2.34 Linux；Windows、
移动端宿主和安装发现尚未实现。管理/恢复协议、runtime 与 PTY 不因入口位于
本机或 SSH 后面而分叉。代理只转发字节，不持有终端状态、PTY 或引擎。

## 调用入口

```bash
cmake --build tmp/sessiond-build --target ghosterm-sessiond ghosterm-session-proxy
/absolute/ghosterm-session-proxy --runtime-dir /absolute/private/runtime \
  --role management --daemon /absolute/ghosterm-sessiond
```

stdin/stdout 必须是 pipe 或 stream socket，不能是 TTY 或普通文件。调用方先
发送原始 GTSD HELLO；代理验证首帧和 management/data 角色，再原样转发。
stdout 只承载协议。stderr 仅尝试一次有界、非阻塞的类别诊断，不写 argv、
env、输入或 token。代理不申请 SSH PTY，不新增 TCP 监听。

远端计划通过现有 SSH 端口的无 PTY exec 启动同一代理。此处只交付进程入口，
尚未交付 libssh2 长连接 owner、exec 参数构造、stderr 排空与退出状态处理、
网络故障重连或安装器；不能把 stdio 测试等同真实 SSH 断网验收。
本地客户端可复用私有 activation adapter 或使用相同代理；生产 Transport/
公开 C ABI 和各端 UI 尚未接线，UI 不得直接包含这些私有头。

## 启动事务

1. 连接每用户私有 Unix 端点；已有实例直接返回，实际 HELLO 仍由服务核验。
2. 仅 management 且没有指定旧 instance 时允许按需启动。DATA 或恢复旧
   instance 的连接找不到端点时明确失败，不能偷偷创建新 daemon。
3. 取得启动锁后重新连接，避免并发创建；只有仍无服务时才启动绝对路径 helper。
4. 新进程拥有独立 session，重置信号 mask/默认处置，stdin/stdout/stderr
   全部指向 `/dev/null`；只继承 FD 3 的私有启动通道，不继承 SSH 管道或 PTY。
5. daemon 完成构造后通过 FD 3 发送 READY；启动方核验契约指纹和必需能力，
   返回带 instance 的 ACK；daemon 确认 ACK 后写出 COMMIT，再开始处理服务 IO。
6. 启动方完整接收 COMMIT 后连接端点，释放启动锁。整个事务共用单调时钟期限。

READY 为 72 字节，ACK/COMMIT 各 24 字节，均有 magic/version/length 和严格
保留位校验；这条通道不复用 GTSD 数据流。发送和接收都允许短 IO。
首个 ACK 字节一旦交给 socket，启动方即保守视为 committed，此后取消、超时、
代理退出均不得杀 daemon。此前只能终止自己尚未回收的子进程；TERM 有界等待
后 KILL 并 wait。PID 只来自 spawn/wait，禁止使用 PID 文件推断进程存活。

helper 路径逐级禁止 symlink、`.`/`..`、组/其他用户可写、非 root/本用户所有
节点，叶子必须为普通可执行文件且无 set-id。运行时目录/端点沿用同 UID 认证。
同 UID 代码属于信任边界，路径校验不提供同 UID 沙箱。已有服务无需读取 helper；
不兼容实例仍保留，交由真实 HELLO 报错，不能自动替换。

## 生命周期与背压

代理双向各保留固定 64 KiB 队列，每轮每方向最多一次 4096 字节 IO，短写只
退掉实际发送字节。消费、搬移后的空区和退出缓冲均擦除。队列满时停读，
连接 HUP/错误仍优先检查；Darwin 使用实际读/写过滤器探测满队列下的 EOF。
连接死亡后未接受输入丢弃；不重放未知输入，不终止原 session。

`serve-detached` 是内部入口：校验独立 session、FD 3 内核 peer 凭据和标准流
隔离，缺少启动握手时退出。`serve` 保留前台运维用途，显式 TERM/INT 仍关闭
宿主和会话。当前进程不实现退出后自动重启、系统服务管理或跨宿主迁移。

## 验证入口

```bash
ctest --test-dir tmp/sessiond-build -R '^activation_' --output-on-failure
python3 scripts/mutation-check-activation.py --build-dir tmp/sessiond-build
```

27 类 CTest 覆盖并发启动、锁内重查、启动前取消/超时、ACK 后取消不杀会话、
不兼容/能力缺失、FD/信号/标准流隔离、逐字节短 IO、pipe/socket 背压、满队列
断连和 stderr 堵塞。真实 VT 的 DATA 恢复另经代理运行；代理 SIGKILL 后查询
同一 instance 的原 session。独立链接检查禁止代理带入 manager/runtime/PTY/
引擎对象；38 项变异要求普通 FAIL 与同变异绿对照，含源码 SHA256 回滚。

macOS/Linux arm64 均通过 daemon 全量 396 项、core gate 348 项与 57 个公共
符号对拍；两端相关 27 类 ASan/UBSan 通过。Apple leak detector 未启用，Linux
启用；借用的引擎库未重编 sanitizer。变异遗留标记退出 2、INT/TERM 编译中断
与 SHA256 回滚另验。源码/库指纹及日志保存在工程 `tmp/session-activation-*`。

这批验证不覆盖真实网络中断、全原生 GUI、完整资源事件和产品安装；P0 仍未完成。
