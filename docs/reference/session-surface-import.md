# 原生 Surface：空副本与 IO 线程导入

这是 macOS full libghostty 的引擎接缝证据，尚不是原生产品入口或完整 P0 验收。
补丁为 `0044`（空副本导入）、`0045`（独立配置/配色）、`0046`（真实输入屏障），
仅应用于隔离源码构建，不修改 vendor。

UI 线程创建空白、只读的 replica Surface，只接收尺寸、主题与配额等低频元数据。
快照和资源字节留在 IO owner。IO owner 对新 Surface 调用一次 import，恢复 canonical、
解析器 continuation 与资源图，再发布 generation/sequence。失败的 Surface 必须丢弃，
不能原地重试或假报 applied。这里不替换已经显示的 Terminal；每次 attachment 创建
新 Surface，后续由宿主在同一逻辑 pane/tab 中交换视图，避免旧 selection/search/render
引用指向已经释放的 Terminal。

实际 engine applied 屏障和协调器 LIVE 均成立后，宿主显式启用该 generation 的输入。
import 本身不开放输入。未知/未来 generation 与未来 sequence 不能启用；断连立即
禁用输入，迟到的旧代输入被丢弃，不能污染新代或使新代失效。原有 raw output 路径
不允许混入这个副本。空白 placeholder 不得确认尚未导入的应用位置。

默认资源 hook 拒绝宿主文件访问；没有资源能力不能静默从本机读取。资源文件类
命令在本探针中明确失败并使 applied 无效，保留原文件，不声称已实现完整资源事件
路由。GUI general/page allocator 使用已有预算域，导入失败与销毁后核对归零。
引擎 App、GPU 和全局预算仍不在此探针的完整验收范围内。

## 已验证的证据

```bash
python3 scripts/probe-session-import.py --mutations
```

十一类基线覆盖空副本、分段 CSI、半个 UTF-8 字符、RGBA 图片、损坏 canonical、
损坏资源、混入旧字节、预算不足、临时资源文件拒绝、独立字体/配色配置与普通终端输入对照。源引擎在导入前已释放，
源捕获、解码和导入均在后台线程。独立比较导入/增量后的 canonical 与资源字节，
真实 NSWindow/Metal 像素验证红色区域保留，以及增量后的绿色区域出现。

十三组变异各要求普通 FAIL/exit 1，并在同一变异下通过空 Surface 对照；分别破坏
continuation、资源图、一次性导入、空副本 applied、LIVE 输入门、generation 核对、
旧代输入隔离、资源 hook 以及独立 canonical 比较期望。没有把信号退出、编译失败
或超时算成变异通过；另覆盖配置借用、配色覆盖、错误绑定与普通终端输入。源码 SHA256 回滚、标记预检 exit 2、INT/TERM 后代清理与
失败构建产物失效已验证。另用本补丁构建重跑原有 26 项 full Surface 场景，含
真实 less/Vim，全部通过；探针必须设置工程内 TMPDIR。

本补丁改变 strict compatibility hash。客户端和 Linux payload 必须一起重建发布；
旧包拒绝新客户端是当前约定，不增加旧后端兼容或协商。公开 session/setup ABI 与
异步 DNS 已另有组件证据；产品 Keychain/凭据 UI、视图交换和完整资源/总预算验收
继续实施。

## Swift/C 原生绑定

`TerminalSurface(replica:generation:policy:)` 只在主线程接收 POD 配额/配色元数据；
`ReplicaPolicy.decode`、一次性容器导入、OUTPUT/RESIZE 与实际 applied 均在非 UI
owner 执行。配色先在 IO 建立有序屏障，再由主线程只应用标量配置；真正应用前
不前进 sequence。编码后的输入携带原始 generation/sequence，不能改为提交时位置。
损坏容器也消耗一次导入机会；已知零位置与未知位置分开。

```bash
python3 scripts/probe-replica-surface.py
python3 scripts/mutation-check-replica-surface.py
python3 scripts/check-replica-allocator.py --mutations
```

五项真实原生测试覆盖容器损坏/重用、零序号、预算拒绝、半个 UTF-8/图片、真实
键入、旧代与未来序号拒绝、输入撤销、销毁后账本回落、RESIZE/配色实际应用屏障。
七组隔离变异要求普通 XCTest 断言红及同变异对照绿；标记预检、INT/TERM 清理和
SHA256 回滚另验。GUI 复用相同预算分配器的 allocator-only 模式，只包含引擎分配
器 ABI，不引入 headless VT runtime；符号表闸门及其变异验证此边界。

真实前端输入曾暴露空副本导入前 `queueIo → queueEncodedInput` 递归；`0046` 在
编码入口关闭未 LIVE 的动态副本输入，普通 Surface 输入仍有独立对照。没有把当时
的栈溢出当成变异红；修复后真实键入及上述基线均已通过。

## SDK 产物位置

`bash scripts/build-ghostty.sh` 现在在工程 `tmp/` 中从干净 pin 生成并验证源码，
发布到 `build/GhosttyKit.xcframework`。所有 Apple 构建、打包与闸门消费同一位置；
不在 vendor 中打补丁或撤销补丁。SDK 携带完整对应头文件和 `session-engine.json`
补丁/头/静态库 SHA256 记录；allocator 私有文本头不进入 Swift 的 umbrella module。
当前生产 UI 入口与 Linux 新补丁包仍需配套接线/重建，此证据不代表 P0 全部完成。
