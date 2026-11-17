# macOS 终端设置存储

设置文件仍是 Ghostty 语法，由引擎解析；UserDefaults 应用偏好不进入该文件。`SettingsStore` 复用原生 `ConfigStore` 的字节/设备/inode/权限/纳秒时间快照、稳定非阻塞锁、强制备份、同目录完整暂存与原子替换。Settings 的备份保持原有 `ghostty.conf.bak` 命名；SSH 存储保持原先去扩展名的 `.bak` 规则，不为复用更改用户备份位置。

只有 ENOENT 是空文件；目录、链接、读取失败、非法 UTF-8 与 NUL 均保留错误。`rawConfig()` 现在抛错，展示缓存与读取错误分开；加载失败的缓冲区不得保存。外部新建、等长/同 mtime 替换、删除及变化都会导致保存冲突。读取与提交使用同一份捕获快照，不在写盘前偷偷读入外部文本并冒充用户已审阅。

第一次保存可建立缺失的应用配置父目录与 0600 文件。已有文件保持权限/所有者，备份为独占私有文件；备份拒绝不删旧备份或目标。rename 后错误明确返回已提交，Settings 缓存也失效，不能再次保存或把旧内容当当前磁盘成功状态。必须显式重新读取核对；不承诺对不合作写入者提供原子 CAS 或 ACL/xattr/断电持久性。

设置页配置区在读取错误时显示错误并禁止保存，提供“重新读取”。此动作只更新文件快照与编辑缓冲区，不宣称引擎已经应用；标题改为“已加载的配置文件”。保存才调用原有应用流程。读取失败后即使文件随后恢复，也须通过该动作采纳新内容，不能将空编辑器写回。

稀疏合并更新标量的最后一条本地声明，与标量回显一致；删除清除该键的全部本地声明，避免旧值重新露出。CRLF 以字节换行分隔，其他行/顺序/注释和尾换行保留，目标行保持原来的 CRLF。空值声明可识别。`value` 仅是单键原文查询，不是累计项的引擎求值；字体选择仍由 `TerminalFontSelection` 显式清除本地 fallback 声明再选择单一字体，其他重复项保留原文，不把它们描述为标量。

## 验证与后续

存储和界面专项在[分批记录](non-plugin-remainder.md)记账。初轮暴露 Settings 备份名与共享默认规则不同、Swift 测试中 throws 表达式编译错误和 CRLF Character 分隔失败，均保留并在修复后重验，不计作有效变异红。带完整引擎配置套件的 96 项为零失败/一个开发态主题资源跳过；不能记全量零跳过。独立 80 项组合含真实 AX 错误/显式重新读取/保存，零失败/跳过。


`python3 scripts/mutation-check-macos-config.py --checkout tmp/rem19 --phase settings-store|settings-ui|store` 新十一＋界面一组及旧存储二十二组复验通过。每组均真实编译后正常单条 XCTest 断言红、同构件 HostList 独立读取绿、恢复重编绿与 SHA256；原件 `tmp/rem19/tmp/config-mutations-{etnxxiuy,5wqy_y70,liclrh7u}/receipt.json`。不把前述编译失败当目标红。

最终实际本机 sshd 组合 85 项零失败/跳过，普通本地/SSH 网格及 SFTP 浏览必跑收据逐项 passed，既有 WebKit 检查通过；`tmp/rem19/tmp/settings-store-real-ssh.log` 与对应 `apple-app-test.log`。arm64 Release `settings-store-release.log` 通过，保留既有并发警告；不据隔离检出宣布并行插件整仓已绿。

引擎候选预校验与应用仍是下一切片：当前 `applyTerminalSettings` 先写磁盘才重载；全文替换拒绝后恢复错误仍由旧路径忽略。本批只关闭存储/读取/重复键问题，不把事务复用记成配置候选校验已完成；已有 surface 的字号/配色与失败时磁盘/引擎一致性须单独验收。没有改插件页面、C ABI 或 Android；不增加现有 CI 二十份。
