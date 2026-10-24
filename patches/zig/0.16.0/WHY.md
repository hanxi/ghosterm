# Zig 0.16.0 Threaded I/O 的 P0 宿主线程接缝

这是对标准库**隔离生成副本**的补丁，不修改安装的 Zig 或 vendor。原始文件
由 `scripts/prepare-session-io.py` 从所选 Zig 的 std_dir 读取，必须同时匹配：

- Zig 版本 `0.16.0`；
- `Io/Threaded.zig` SHA256 `eb7bbb4ddf590ec3d0d2a1ee5c3845ef4984d9e440965a3e7c920cd0c906df94`；
- `Io/Threaded/test.zig` SHA256 `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
  （钉版本发行包中的文件为空，仍逐字复制，不删上游测试入口）。

输出只在工程 tmp/ 下的隔离 engine 中，路径为 `src/lib/p0_threaded_io.zig`
及其原有测试相对路径。0001 补丁在该副本上应用；生成文件和二进制不入库。
主构建/非 P0 生产引擎不因此自动采用实验接口。换标准库源码必须更新 pin、
补丁与运行证据，不能仅看版本字符串相同就接受。

## 原因与边界

Threaded 的四处创建点（async/concurrent/groupAsync/groupConcurrent）直接
调用 std.Thread.spawn 并 detach；POSIX 实现即使给 SpawnConfig.allocator，
仍把入口参数分到 c_allocator。它的 wait_group 在 worker 返回时结束，早于
外层入口参数及备用信号缓冲释放，不能据此释放宿主 allocator。

本补丁保留标准库的任务、取消、队列、互斥、条件变量与普通 I/O 实现，仅
增加可选 registry，把这四处 spawn 交给 libghostty 的 P0 宿主入口帧实现。
owned 模式不 detach：registry 保留 pthread 句柄，在 Threaded.deinit 之后
逐个 join，确认外层帧和信号注册也已结束。无 registry 的 legacy 路径仍
调用原 std.Thread.spawn/detach。

libghostty 实验补丁 0043 选择两条工作线程、16 MiB 栈，两种调度限额都显式
设置。帧及信号缓冲使用全局宿主 allocator；原生栈/guard 的容量由 GUI 同池
另外预留。实际 pthread 栈和连续不可访问 guard 由 C 观测核验，不把请求值
当作实际值。支持证据仅为当前 macOS arm64，不宣称其他端支持。

std.Io.async 饱和时按原标准库契约内联执行；concurrent 饱和明确返回
ConcurrencyUnavailable。没有给 I/O 操作增添后台无界队列或静默备用引擎。
源码派生仍遵守 Zig 标准库的原许可证，原始内容来自用户安装的钉版本工具链。

P0 验证屏障另读取 mutex 保护的 busy_count。标准库在通知 Future/Group
结果之后才递减该计数，因此 await 结果并不代表槽位已重新空闲。验证预热
只在有界轮询确认空闲后返回；它不改变普通 async/concurrent 的返回契约。
连续 400 次复用保留为回归，避免仅跑四次漏掉调度窗口。

MIT 声明另存于本目录 `LICENSE`；生成器也复制到隔离源码旁的
`src/lib/p0_threaded_io.LICENSE`，SHA256 为
`5c537d6853e005298a285d508cff9ac7192cea23576c840d485b2b586a7ff177`。
许可证、源文件与测试 stub 三者均校验完才写入，不接受版本、来源或路径漂移。
