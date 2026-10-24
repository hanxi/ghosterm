# 共用终端快照容器与 DATA 编码

状态：**私有候选 v1 已接真实 VT/DATA；生产恢复未交付**。日期：2026-10-21。
容器实现为 `terminal/state/checkpoint-wire.{h,c}`；daemon 适配为
`daemon/src/vt-snapshot.{h,c}`。本地 IPC 与 SSH 使用同一个
[DATA router](session-ipc.md)，不因入口不同重新编码或另建恢复状态机。
本文不宣称 full surface 客户端、SSH 代理或跨引擎版本兼容已完成。

## 固定容器

外层 SNAPSHOT_BEGIN 的 format 为 `0x47545643`（GTVC），S/代次、分块顺序与
readonly 仍由既有 DATA 协议提供。容器不重复存放 S，不传 C struct 内存。
头部固定 128 字节，多字节整数全为大端，随后依次放 canonical 和 resources。

| 偏移 | 字段 | 约束 |
|---|---|---|
| 0 | magic[4] | ASCII `GTVC` |
| 4 | version:u16 | 1，不支持的版本拒绝 |
| 6 | header_bytes:u16 | 128 |
| 8 | flags:u32 | bit 0 当前深色，bit 1 历史曾淘汰；其余为零 |
| 12 | profile:u32 | 当前仅 1：禁止外部文件/临时文件/共享内存装载 |
| 16 | canonical_bytes:u64 | 非零，完整包含引擎 continuation |
| 24 | resource_bytes:u64 | 非零，与剩余长度精确相等 |
| 32 / 36 | cell_width/height:u32 | 每格像素，1–65535 |
| 40 / 48 | history_bytes/lines:u64 | 正值双限额 |
| 56 | continuation_bytes:u64 | 正值跟踪/导入策略 |
| 64 | image_bytes:u64 | 源资源配额；零表示禁用图像 |
| 72 | light_bg/light_fg/dark_bg/dark_fg | 四组 RGB，各三字节 |
| 84 | reserved[40] | 全零 |
| 124 | crc32:u32 | IEEE CRC32，计算时本字段视为零，覆盖头与两段载荷 |

总长包括头，最大 64 MiB，并受每次恢复传入的独立 limit 限制。解码先检查头、
长度、非空段、精确末端、标记/保留位和策略，再核验 CRC，全部通过才发布借用
view。非法输入清空 view；不能用错误分支中的部分指针创建引擎对象。
CRC 只检测损坏，身份与认证仍来自私有端点/SSH/HELLO/token。

编解码器不分配、不调用 OS 或引擎，只依赖公开 Core POD 头；可由各端 surface
复用。size/write 验证本地参数；write 要求目标与源两段不重叠，容量不足接受
零字节。parse 的 view 借用完整容器，释放输入前须完成引擎导入。容器合法不
等于引擎载荷合法：引擎仍需独立预算、页分配器、continuation/资源校验、完整
消费检查及副作用隔离。u64 配额也必须在宿主准入后才转换为平台大小类型。

## 共用编码与所有权

`vt_snapshot_encoder(factory)` 直接给 DATA router 提供既有 encoder 函数表。
恢复任务将独立 frozen 编码为 canonical/resources 与本文列出的元数据，再合成
一份连续容器。先从总 limit 扣除头部，且不越过工厂的 checkpoint 策略上限；
不能在编码后才发现头使快照超限。

引擎编码临时对象使用工厂已准入 scope。返回 blob 及连续字节使用 router
提供的线程安全 allocator，按原始大小精确回收；释放前擦除整份自有存储。
任何准入/编码/分配/封装错误都清空输出并释放局部对象。worker、checkpoint
任务、runtime 引用与代次归属沿用既有 DATA 规则，适配器不持有可变 terminal，
不停止会话，也不碰 UI。工厂必须活到真实任务 reaping/router close 之后。

## 当前运行证据

macOS arm64 下，九类 VT/真实 PTY 与五类容器基线通过。新增场景包含：独立
固定字节向量、重算 CRC 后的非法头、截断/尾字节/整数溢出、精确容量与分配
拒绝/擦除，以及真实 HELLO/CREATE/ATTACH/BIND → 捕获/编码任务 → 分块容器 →
VT 副本 → READY_S/L → USER 和输出/resize/配色增量。
测试先保持编码器持有 S，再由可信测试宿主提交 resize，确认 L>S 且副本补齐
S+1…L 后才进入 LIVE；这不是让产品客户端绕过恢复期的 USER 授权。

DATA 场景在连接断开期间保留原 shell，继续输出并回答配色查询；新代从同一
session 恢复并继续输入，LIVE 期间也执行查询。最终副本的 canonical/resources
与权威终态逐字节一致；已退出会话再次附着为 readonly，USER 明确拒绝。
内联图像另检查真实 RGBA 像素。这里的消费者是无 UI VT，不能替代 GUI 的
渲染/输入/barrier 或真实网络故障验收。

37 项隔离变异要求普通 FAIL/exit 1、同变异独立绿对照和起跑前 SHA256 回滚。
十四类 ASan/UBSan 与 TSan 检查通过；TSan 覆盖 C 组合路径，借用的引擎动态库
未重编为 TSan。遗留标记与编译期间 INT/TERM 后代清理另验。核心 gate 仍为
322 项及 57 个公共 ABI 符号，没有增加公开 client ABI。
后续[共用客户端](session-attachment-client.md) 已替换真实 VT 场景里的手写
分块/READY/USER 流程；其私有组件不代表原生 surface 接线。加入客户端后核心
gate 为 334 项、57 公共符号，本文原始容器/引擎边界保持不变。

Linux arm64 原生 GCC 对相同容器源码/固定向量跑五类基线及十六项定向变异，
与 macOS 的字节结果一致；VT 工厂、编码适配和组合夹具另仅完成 GCC 严格编译。
尚无 Linux 引擎链接运行、跨 CPU/full surface 或生产 SSH 恢复证据。

日志为工程 `tmp/vt-snapshot-{tests,mutations,sanitizers,tsan,harness,gate,linux}.log`。
依赖/源码 SHA256 在 `tmp/daemon-vt-proof.json`、`tmp/vt-snapshot-linux-proof.json`；
命令沿用[VT 工厂参考](session-vt-state.md#当前验证)，所有新基线都在同一 CMake
目标与 `scripts/mutation-check-daemon-vt.py` 中，没有单独的恢复探针协议。

## 仍需完成

profile 1 是当前接线的显式限制，不缩小最终恢复范围。外部资源结果/失败事件、
动画时钟、宿主动作、程序/命令摘要、历史淘汰增量、通用配置和完整资源账本仍须
按同一顺序接入；不能因为容器已能传图像，就宣称 LIVE 外部资源可准确重放。
生产兼容指纹/能力、客户端实际准入与原生 full surface 导入、SSH stdio 代理、
可执行服务/安装和断网测试同样未交付。私有 v1 随当前精确引擎/补丁集演进，
不维护旧格式兼容，不将本次组合通过当作 P0–P8 已完成。
