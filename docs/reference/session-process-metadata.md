# Source 目录与前台作业元数据

后台 Source 在自己的 PTY 所有者线程上查询当前目录和前台进程组。本地 macOS 与远端 Linux 使用同一套 C worker/runtime/DATA/client 路径，界面不直接查询核心或远程操作系统，也不解析输入命令来猜测目录。

查询间隔至少 250 ms，值不变不发布。DATA `PROCESS_META`（类型 53）携带 attachment generation、目录可用标志、child 四态与目录长度及 UTF-8 字节。它没有终端序号，不修改 canonical VT state，不触发 Surface 的 OUTPUT 回调，不参与输入回执或 S/L 应用确认。客户端拒绝提前出现、旧代及格式错误的元数据。

C ABI 的 `gt_session_info_t` 追加 `has_process` 与 `gt_session_process_t`。`UNKNOWN` 与 `IDLE` 分开；目录不可用时为 false 和空字符串。目录上限 1023 字节，不截断后冒充完整路径：Source 向既有可能截断输出的 backend 多申请一个字节，只在路径完整装得下时发布。超长、非法 UTF-8、查询失败或未观测均保留未知。第一次内核观测可能早于 fork 子进程切换目录，后续观测会更新它。

Swift 绑定仅将变化后的元数据送到主线程。窗格当前目录优先使用 Source 观测，再回落到 OSC 7；分屏目录继承和文件面板可消费同一事实。断开会清除本代观测，恢复后重新取得；Source 已确认结束时显示 exited。目录和作业元数据不进入持久化会话记录。

这是破坏性 C ABI/DATA 契约更新，采用当前严格哈希匹配。macOS helper 与 Linux 双架构包均重建；当前契约为 `50137ac92a74911afc5080b813f6fefb3f87b1014f32707911314c6755305c5c`，不增加旧后端适配器。

验证包括核心 codec/owner/client 和真实 runtime 路由：字节向量、未知态、UTF-8、边界/无 NUL/截断路径、查询节流、合并重复观测、提前/旧代拒绝，以及元数据不推进终端序号。`scripts/mutation-check-session-process.py` 提供 11 项 C 隔离变异，`scripts/mutation-check-session-process-native.py` 提供 4 项原生绑定/发布变异；均要求普通断言失败、同变异对照通过及 SHA256 回滚。

真实 macOS shell 验证中文目录、前台 sleep 作业与返回空闲、更换所有者后的原 PID/目录恢复。Mac→Linux arm64 与 x86_64 QEMU 路径验证远端目录、安装、原生 RootView 恢复和实际终止；x86_64 原生硬件仍未验收。默认 AppModel 路由与恢复清单仍在后续接线，不以模块测试代替完整产品入口。
