# 非插件剩余实施记录

按用户“剩下的也处理完”继续推进可实施的非插件功能与自动化验收；插件修改由另一 session 处理。Android 自动化使用自有模拟器，手机体验按既定人工手测分工。环境包等已明确延后扩展不因本次请求自动启用；发行/证书和人工系统操作分别记账。

前五批临时构建与夹具位于工程 `tmp/rem18`，材料与文件批位于 `tmp/rem19`，每批提交本轮文件，保留另一 session 的暂存索引。验收规则仍以 AGENTS.md 为准，不将故障/编译异常当正常变异红。

| 批次 | 范围 | 结果 |
|---|---|---|
| 1 | SSH 配置候选发布 | 50 项组合、六组变异、arm64 Release 通过；备份拒绝/新建失败/parse→store 变化/提交后错误/主机和规则发布均覆盖 |
| 2 | Core 原始指令快照与解析保真 | 590 CTest/186 ABI、Swift 四项、Android API 36 专用模拟器 35 项、Release/R8 与四模块 lint、八组编译变异通过；两端绑定同步 |
| 3 | 主机表单来源/保真/改名 | 73 项原生组合、16 个新保护与发布旧六组分轮通过；arm64 Release 通过 |
| 4 | 主机别名字节身份 | 所有普通 SSH/SFTP/tmux/分屏查找及选择器改为 ASCII 折叠、UTF-8 字节身份；50 项连接组合、76 项配置组合分轮、两组新变异和 Release 通过 |
| 5 | Apple full 闸门必跑策略 | 两版 Bash 各 18 项、六组 shell 正常单断言故障；Core 12 个真实 SSH/SFTP 必跑收据及 App 4 项组合通过；其它条件跳过保留 |
| 6 | macOS 私钥安全存储生产接线 | 首次导入后认证只经 Keychain 回读、主动重新导入及取消隔离；46/105 项组合分轮、18 项真实 SSH、十四组变异与 Release 通过，见[材料说明](macos-private-keys.md) |
| 7 | 文件导航/历史与树缓存 | 34 项最终组合、十组变异、33 项真实本机 SSH/SFTP 组合及 arm64 Release；在途后端取消另批，见[导航说明](macos-files-navigation.md) |
| 8 | 搜索取消、部分结果与范围预算 | 最终 68 项组合、21 组变异、26 项真实本机 SSH/SFTP 组合及 arm64 Release；旧调用取消 deadline 仍归 owner 迁移，见[搜索说明](macos-file-search.md) |
| 9 | 终端设置存储与重复声明 | 80 项组合、85 项真实本机 SSH 组合、11＋1 组新保护/原事务 22 组复验和 arm64 Release 通过；引擎候选切片后续，见[设置说明](macos-terminal-settings.md) |
| 10 | 终端配置候选校验、应用与实际回显 | full 引擎四补丁、99 项定向/104 项真实本机 SSH 组合、17 组变异、Core 596/190、C 格式及 Release 通过；详见[设置说明](macos-terminal-settings.md) |
| 后续 | SSH 网络/取消、文件与传输、保活及模拟器回归 | 顺序推进；未完成项不提前勾选 |

批 1 的六组均为隔离源真实编译后指定单条正常断言红、同构件独立配置读取绿、复原重编绿与 SHA256。成功操作保留连接快照/UUID，不通过刷新关闭连接；这不是外部 SSH 或整客户端回归结论。

批 2 新增 `gt_config_directive_at`，返回单个物理 Host 块内的原生参数、原文及来源；不混入 Include 展开，重复 Host 与 Match 在返回前显式拒绝。修复空白等号分隔、长 token 静默截断；NUL/未闭合引号失败，编辑候选解析失败保留旧树。Swift 文件入口拒绝无法无损往返的 UTF-8。

八组手工变异入口为 `python3 scripts/mutation-check-config-directives.py --checkout tmp/隔离检出`，不增加既有 CI 二十份。`tmp/rem18/tmp/directive-mutations-wpln432l/receipt.json` 记录 25 条（基线＋八组目标正常单断言红/同构件黄金对照绿/复原绿）及 SHA256。首轮探针未用变量编译失败、后续同秒增量未重链导致未杀死均保留，不计有效红；最终脚本强制重编重链。原始日志在同 worktree 的 `tmp/directives-*`：Core 全闸门、Swift 四项和既有宿主 50 项分轮，Android 专用 emulator-5680（API 36/arm64/4 KiB）35 项。Android 第一轮误用不存在的 transport lint 项目失败，修正为 app/bridge/terminal/data 四模块后 Debug/测试 APK、未签名 Release/R8/lint 通过。隔离构建复用逐字复制的锁定原生依赖，未改 vendor。该切片是接口与解析验证，主机表单另批验收；未发行，也不代表并行插件工作区全绿。

批 3 表单由 Core 快照驱动，脏字段按字节比较，多条身份逐行保存且引用内逗号/空格保真；明确本块/继承/默认来源，多别名共同影响、Include 只读与重复块/Match 失败不折为空表。未改保存零写盘；关闭本块值只删本块行，未知重复标量及 Include 路径要求原文件编辑。连接行排除否定模式并按 ASCII 大小写去重，ID 保留 Unicode UTF-8 差异。

新增/改名不覆盖已有同名块；字段与目标 token 改名由同一候选提交，碰撞不泄漏先改字段。预览重新求值，确认捕获原草稿和原字节快照；旧窗口不能覆盖后来提交。既有连接不会因成功编辑调用全局 reload；本片实际控件覆盖保存/取消/改名确认，未声称通道/PID 长时回归。

手工变异仍走 `scripts/mutation-check-macos-config.py --phase form|host-save|host-list|host-ui`：8＋5＋2＋1 个不同新保护，正常单条断言红、同构件独立 HostList 配置读取绿、复原重编绿及 SHA256；publication 既有六组在新代码复验通过，不增加 CI 二十份。原件为 `tmp/rem18/tmp/config-mutations-{u769enzr,i6dhwor6,9p0_lvs3,jpklm6xq,bhg71p51}/receipt.json`（form 目录以其最终日志为准）。最终同源 73 项组合和三项真实离屏原生控件通过，日志 `host-form-tests4.log`；首轮 SwiftUI 可选 Binding 编译失败、变异脚本未容许新文件的前置失败、dirty-only 旧目标触发非预期抛错均单列，未当有效红，最终改为独立正常断言并重跑。AppModel 当前并行插件修改仅按分离 hunk 保留，不据隔离构建宣布未完成插件组合全绿。

批 3 的最终 `host-form-release.log` 为 arm64 Release 构建通过；既有 Swift 并发警告保留，不将警告转称失败或删除代码换绿。

批 4 修复 Swift 规范等价比较造成的别名错配：`café` 的组合/分解写法仍分别解析为不同配置目标，列表选择 Hashable 与模型查找遵循同一规则，ASCII 大小写仍按 Core 折叠。三项新用例含实际 AppModel `remoteTarget` 的不同地址/用户/端口，身份核对不启动网络连接；既有连接选择器与表单组合 50 项、变异基线/最终配置组合 76 项分轮通过。`--phase target-match|target-selection` 两组正常单条断言红、独立读取绿、重编复原绿及 SHA256，原件 `config-mutations-amo8tjj_` / `config-mutations-5ufb_rzf`；arm64 Release `host-target-release.log` 通过。没有改插件发现、会话生命周期或 Core ABI。

批 5 的 required 模式由 full CI 对两个 gate 单次启用，不向后续定向变异外溢。Core 带自有 SSH 夹具重跑完整绑定套件并核对十二个方法，App 核对本地网格、SSH 网格与 SFTP 浏览；必跑用例 skipped/缺失/重复收据、空测试、缺工具链或服务启动失败均非零。optional 跳过报 PARTIAL，不再打印 PASS；不删条件测试或改变其它插件策略。日志核对不以摘要“执行 0 项的 Swift Testing”覆盖实际 XCTest，也不以普通单元通过代替 SSH 方法收据。sshd 清理跟踪实际自有 PID 并 wait。

`python3 scripts/test-apple-app-gate.py --shell /bin/bash` 与默认 Bash 5 各十八项；`python3 scripts/mutation-check-apple-gate-profile.py --checkout tmp/rem18` 六组为 shell 语法通过后的正常单 unittest 断言红、同夹具独立 ABI 失败传播绿、恢复绿及 SHA256，原件 `apple-profile-mutations-yp2oj_4j/receipt.json`。它们使用 shell 依赖桩，不冒充原生编译或认证变异。初轮 Python 字符串语法失败未计红；手工探针导入生成的单份字节码缓存已清除，测试禁止旁写字节码后重新完成上述六组和 Bash 5 十八项。工作流 YAML 语法通过。

实际原生证据位于 `tmp/rem18/tmp/apple-profile-core-native2.log` / `apple-profile-app-native.log`。Core 离线 90 项含 29 条件跳过；真实本机 sshd 档 90 项含 17 条件跳过、零失败，十二必跑方法逐项 passed。App 定向 4 项零失败，三必跑方法逐项 passed；既有 WebKit 子资源夹具检查通过。只证明本机 arm64 定向与必跑策略，非完整宿主/云端 CI/全部条件项完成。首次工作目录拷贝错误导致执行旧 gate 的结果未计为新 required 证据。

批 6：材料错误不得静默切密码，Android 容器与相对路径显式 unsupported；首轮旧“不存材料”断言和未杀死的冗余检查探针不计绿，完整说明/原件见[macOS 私钥材料](macos-private-keys.md)。导入独立保存，取消不删除已完成的材料；在途读取取消后不得交付凭据。测试仅清理明确声明的自有 Keychain 账户，不处理插件计划或产品。

批 7：最新导航身份、成功后历史提交、排队旧读跳过、同目标 cwd 去重、刷新 epoch 和树错误重试已接。根/子目录真实 AX 失败未计绿，改为有界状态区后最终控件通过，完整原件/范围见[导航说明](macos-files-navigation.md)。

批 8：取消实际进入本地/远端循环，明确拒绝 exec 才回退；stderr/未知或非零退出、帧/路径/元数据异常及预算溢出均保留部分结果并显示未完成。原查询/范围重试使用全新请求；真实 AX 取消/重试、真实拒绝权限子目录及带引号路径/字面量方括号/深度的实际 SSH 覆盖见[搜索说明](macos-file-search.md)。

批 9：设置存储复用配置事务，保持备份路径；读取错误不可保存，重新读取仅采纳文件快照。CRLF、最后标量/全部删除与字体 fallback 分开；首次创建/外部新建、身份变化、备份拒绝和提交后失效均有一次性文件证据，详情见[设置说明](macos-terminal-settings.md)。

批 10：内存候选使用最终路径递归解析并校验深浅色，主文件成功提交后应用保留对象；失败校验/备份不先写入或改变 surface，提交后应用错误和恢复结果单独报告。只读来源、实际条件值、旧编辑器与打开外观页不写盘均有专项。17 组新保护分轮正常单断言红/同构件配置绿/恢复绿/SHA，Core 596/190 与四补丁 SDK 校验通过，完整输入/收据与未计通过的探针见[设置说明](macos-terminal-settings.md)。
