# 原生恢复会话的有序配置

终端默认配色属于 Source 状态。全局 `App.updateConfig` 对恢复副本直接发送无序的
change_config，会破坏副本的已应用序号，不能沿用普通 PTY Surface 的更新方式。
补丁 0047 让 App 级更新跳过 empty replica，宿主深浅色通知也不直接改这类 Surface。

公开 C ABI 新增 `gt_session_style`。STYLE 携带当前解析后的深浅模式、默认背景/前景/
光标 RGB 和 256 项 RGB 调色板，走与 INPUT 相同的 generation、LIVE、序号与结果校验。
USER 类型为 69，DATA event kind 为 8，两个 payload 均为 805 字节：24 字节 stamp/
event 头、4 字节 big-endian 模式和 777 字节 RGB。逐字段编码，不发送 POD padding。
无效长度/模式明确拒绝；没有实现此能力的状态工厂返回 UNSUPPORTED。

Source 在 worker 上更新 VT 默认色，保留程序通过 OSC 设置的覆盖值。冻结对象另存
该状态的两档默认背景/前景，不能在稍后编码时取回工厂默认值；palette 和默认光标由
canonical 状态保留。设置改变后，即使 RGB 相同，也发送一次 STYLE：字体和按键配置
可能改变，不能只按颜色去重。字体由本端配置读取，不向远端上传字体或 GUI 依赖。
当前光标配置传输的是解析后的 RGB，不传输 cell-relative 光标模式。

接收端先在 IO owner 为 STYLE 保留有序位置，再在 main owner 克隆独立配置、设置精确
RGB 与调色板，并经 tagged scheme barrier 投递给引擎。只有引擎真正应用该序号，才
能声称配置已完成。初次 Surface 发布、ready 和输入启用均等待这道屏障。

已经 LIVE 的同代 Surface 在本地配置屏障期间继续编码输入；Transport 在原有有界
队列中暂存，等 STYLE 实际应用后再发送。补丁 0048 保留初次/旧代输入禁用，避免仅因
另开窗口或换字体就丢键。断网/撤销仍清空该队列，没有新增离线输入重放。

`RecoverablePaneSession` 订阅共享 TerminalApp 的配置变化；打开新 AppModel、重新加载
设置和切深浅色均走同一路径。观察器随 detach 解除；结束的冻结画面不伪造新的 Source
配置事件。STYLE 明确拒绝会显式使本次连接失败，不能一直假装等待生效。

## 验证

- 核心 USER/DATA 固定向量、全部截断/多余字节、模式、容量与 reply 类型；公开 ABI
  提交和回调字段对拍。五组 C 变异验证 palette、Source 默认色、OSC 覆盖与冻结元数据。
- 真正的 RootView/Metal 窗格以自定义背景、字体和 palette 创建，首次发布前检查实际
  引擎默认背景；detach 后同 PID/记录/窗格恢复。重新加载配置、切模式、再开 AppModel，
  在刻意阻塞 main 应用屏障时输入，最终按键全部进入原 shell，旧 Surface 输入拒绝。
- 四组原生变异护首次输入屏障、全局无序更新、LIVE 配置期间错误撤销输入及观察器断线；
  普通 XCTest 断言变红，同变异主机配置读取仍绿。marker 预检与 INT/TERM 回滚单独验证。
- 引擎十一类真实导入基线通过，Linux aarch64 真 SSH 和 x86_64 QEMU 用户态的原生安装/
  复用/同 PID 恢复/停止闭环用同一 contract 重跑；后者仍不代表原生 x86 硬件全量验收。

这次增加 C ABI 和严格契约，是 BREAKING；本地及 Linux 双架构包必须一起重建，旧后端
及旧记录不适配。未改协议升级策略：当前优先快速实现，后续升级协商另行讨论。
完整 P0 的全局/第三方/GPU 资源账本仍未通过；本项不扩大其完成口径。
