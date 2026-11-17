# macOS 普通 SSH/SFTP 打开

普通终端和文件连接创建独立打开作用域，默认 60 秒截止时间从认证流程开始前建立。DNS、主机验证、凭据等待、ProxyJump 各跳认证以及随后 channel/PTY/shell 或 SFTP 子系统打开使用同一截止时间。shell 传递实际余额；SFTP 取余额与原有五秒子系统上限的较小值，不在认证后重新获得五秒。

作用域持有线程安全取消令牌。停止终端、关闭标签、重载配置或取消文件工厂 Task 时直接置位，不把令牌更新排在繁忙 IO owner 后面。认证开始、解析完成、提示回调及定时推进重新检查取消/过期；不支持的配置仍在 DNS 前明确拒绝。受控绑定继续负责 Core 的实际 channel/PTY/shell/SFTP 等待，不增加静默旧入口回退。

交接前再次检查取消和页面身份。原生 shell 已打开但取消先到时，不创建 surface，transport 按原 owner 释放；已开的 SFTP 子系统先在 owner 关闭再返回取消。仍存在的终端窗格显示“SSH 连接已取消”，重载不会留下永久“连接中”。认证和打开作用域的完成按各自代次/对象身份移除，旧完成不能撤销替代请求或消费其取消记录。

本片只接普通终端、独立和内嵌文件连接的打开阶段。tmux 管理/attach 的专用命令和后续生命周期、插件执行均不借用本片结论。SFTP 后续 list/realpath/任务/预览/关闭仍有旧阻塞调用和主线程等待；多地址选择、文件 owner 迁移和保活继续分批处理。没有公共 C ABI、JNI 或 Android 产品改动。

## 验证对象与证据

手工验证使用工程 `tmp/rem19` 的隔离原生构建、一次性配置、离屏原生窗口和自有环回 sshd。`scripts/mutation-check-macos-ssh-opening.py --checkout tmp/隔离检出` 另启动一个故意不回应 SFTP INIT 的自有服务；取消在实际受控 Core 协议等待中生效。忙队列测试分别观察终端停止与文件 Task 取消，两个测试与真实协议停滞分别记账。

本批二十个不同新保护分轮通过：`tmp/rem19/tmp/ssh-opening-mutations-g6l2ip48/receipt.json` 为完整二十组；真实 SFTP 等待增量为 `ssh-opening-mutations-6grklhz1`，只增强原取消组，不另加组数。最后收紧已移除窗格的取消记录清理后，八个模型组全部在 `ssh-opening-mutations-87shri3m` 复验，最终基线/组合各二十五项，含新二十项与 HostList 五项。每组均实际编译后单条正常断言红、同构件独立配置读取绿、恢复重编绿与 SHA256；不把多轮相加为更多不同保护。

最终自有 sshd 组合六十二项零失败/跳过，普通本地/SSH 网格与 SFTP 浏览三个 required 方法逐项通过，既有 WebKit 检查通过。日志为 `tmp/rem19/tmp/ssh-opening-native-current-final.log` 及对应 `apple-app-test.log`。实际停滞子系统另由上述二十五项组合验证，未将缺少该专用环境的 skipped 当成协议等待通过。

旧跳板十三个不同保护分轮复验：十二组完整记录在 `tmp/aso11/tmp/ram-dvvdsc6i/logs`，重载增量为 `ram-sjfaai5s/logs`，不是十三组单轮全跑。旧认证十一组按既有 suite 汇总判据通过，原件 `tmp/rem19/tmp/ssh-opening-old-auth2.log`，不混称本批二十组的单条目标红。最终冻结源码的二/三跳各十种产品场景通过，另有离线配置/错误/取消/期限选择；`tmp/aso11/tmp/ra-l2tnqt77/logs/result.json` 保留完整输入摘要。所有连接仅访问自有环回服务。

隔离 arm64 Release 通过；取消记录清理后的当前 main 与已保留插件工作组合又在独立构建目录通过 Release，`tmp/rem19/tmp/ssh-opening-current-release-final.log`（97.21 秒）。源码合入仅使用普通 SSH hunk，未覆盖或运行插件业务；不据生产编译声称插件套件已绿。Core/C ABI/JNI 未改，不新增 Android 测试结果。

新脚本为手工验收，不增加 macOS CI 二十份，也不据本机定向结果宣布所有 SSH 服务器/网络/客户端或发行完成。

首轮测试错误包括 Swift throws 表达式编译失败、异步 MainActor 测试等待不当，以及错误地假定 shell 取消不丢弃连接；均未记有效红。最初两个去除回调的变异遗漏闭包第三参数使用而编译失败，修正变异后重新取证。旧认证夹具复制后权限成为 0644、旧跳板目标同时触发两条断言的前置失败也不计通过；保持私钥来源策略和单条目标红判据，分别修复夹具权限与前置守卫。旧重载探针只撤去认证取消时仍被新作用域兜住，未杀死不计红；增量同时撤去该次重载的两条取消路径，保持生产保护，重新取得单条正常目标红。

认证夹具明确登记并清理自己导入的 Keychain 身份，拒绝密钥测试也清理准确的自建记录；不扫描或删除用户身份。受影响临时路径从源码根推导到工程 `tmp/`，不依赖 XCTest 是否保留 TMPDIR。历史失败和分轮原件保留，后续通过不覆盖它们。
