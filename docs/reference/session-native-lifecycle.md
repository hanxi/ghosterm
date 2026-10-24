# 原生窗格与会话管理 Transport

`TerminalPane.attachRecoverable` 把同一套恢复 Transport 接到原生窗格，本地/SSH
只通过 options 和异步认证处理器区分。初始化先核验记录与端点、contract、创建
模式的对应关系；已 admitted 的记录不能再次 CREATE。记录成功落盘后才启动连接。

恢复保留 tab/pane 的身份与布局，每代使用新的 Surface。RootView 以 Surface 对象
身份重建 NSViewRepresentable，才能实际替换持有旧 Surface 的 TerminalHostView；
只改 @Published 指针会留下旧原生视图。新实例继承当前焦点和可见性，旧视图保持
输入关闭。快照、OUTPUT 和输入仍由 Transport 的 IO owner 直连引擎。

连接恢复、只读与退出码未知分别可见；未知退出不显示成 0。恢复时撤销过期的程序
状态和工作目录推断；输入 UNKNOWN 只显示提示，不转换成连接失败。在本窗格里跑过
又结束的会话（shell `exit`）与直连路径同一语义：收起该窗格、布局塌陷，最后一个
窗格退出即关标签页（对齐 iTerm2 / Ghostty），记录中有已知退出码才显示数值。
已结束的会话只在仍有客户端引用时保留 frozen 终态；最后一个引用释放后 daemon
立即回收并归还 Source 槽，所以恢复列表里的已结束记录不再可恢复（UI 标为不可
恢复）。`stop()`/析构/GUI 退出仍只是脱离 daemon 里的会话，不等于结束它。

## 显式管理

`SessionControlTransport` 是独立、不创建 Surface 的管理 Transport。它经同一
公开 C ABI 和 local/SSH provider 处理 inspect/terminate，所有 IO 都在非 UI owner。

- 只有原 instance/nonce 的 admitted 记录先 QUERY_CREATE，查到 ID 后关闭查询句柄，
  再打开 catalog；从不重建会话，也不假称消费了快照。
- catalog 的未知清单与已知空清单不混用；已绑定实例中缺失的已知 ID 只标记结束，
  不编造结果或退出码。
- TERMINATE 前持久化 stopUnknown，再提交一次请求；断网、取消和重连不重放停止。
- KNOWN/OK 是请求接收证明。随后刷新 catalog，直到实际 hasResult；已结束或已在
  terminating 的 Source 不再次发送 TERMINATE。忙、拒绝、不支持、未知分别保留。
- 异步记录观察失败会让最终 snapshot/completion 明确失败，不能被一次成功重读掩盖。

记录锁本身非阻塞。元数据 owner 吸收最多四秒的短暂写入竞争，不阻塞终端或 UI；
CREATE 的五秒准入屏障仍会在超时后取消发送。旧写入者不能把 stopUnknown 写回普通
session，不能复活已结束记录；已知结果必须伴随 ended。存储失败保留先前的不确定
停止事实，不据此证明远端回滚。

## 验证

```bash
python3 scripts/probe-recoverable-transport.py --pane
python3 scripts/probe-recoverable-transport.py --control
python3 scripts/mutation-check-recoverable-pane.py
python3 scripts/mutation-check-session-control-transport.py
python3 scripts/mutation-check-session-records.py
```

原生窗格探针运行实际 RootView/NSWindow、daemon、PTY，核验落盘恢复、原 PID、
原 tab/pane、实际视图交换、键盘焦点、新输入与旧 Surface 输入拒绝。
管理探针覆盖已知 ID、仅原 nonce 的创建查询、活 Source 的显式终止、实际结束
结果、自然结束后不重复停止及未知退出码；另有真实锁竞争后的元数据恢复验证。

五组窗格、七组管理、十二组记录隔离变异要求普通断言红和同变异绿色对照。
标记预检 exit 2、INT/TERM 后代清理与 SHA256 回滚均单独验证；编译失败、崩溃、
探针超时不计作变异成功。

这完成原生窗格与管理组件的接线。默认 AppModel 新建/分屏入口、启动发现列表
仍待完成；平台凭据适配、随包 helper/catalog、有序主题和 Source 元数据已分别验证，尚不能宣称全套产品入口完成。

配置/字体更新与初次 ready 屏障另见 [有序配置](session-ordered-style.md)。

Source 内核目录与前台作业观测见 [进程元数据](session-process-metadata.md)。
