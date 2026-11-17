# 非插件剩余实施记录

按用户“剩下的也处理完”继续推进可实施的非插件功能与自动化验收；插件修改由另一 session 处理。Android 自动化使用自有模拟器，手机体验按既定人工手测分工。环境包等已明确延后扩展不因本次请求自动启用；发行/证书和人工系统操作分别记账。

临时构建与夹具位于工程 `tmp/rem18`，每批提交本轮文件，保留另一 session 的暂存索引。验收规则仍以 AGENTS.md 为准，不将故障/编译异常当正常变异红。

| 批次 | 范围 | 结果 |
|---|---|---|
| 1 | SSH 配置候选发布 | 50 项组合、六组变异、arm64 Release 通过；备份拒绝/新建失败/parse→store 变化/提交后错误/主机和规则发布均覆盖 |
| 2 | Core 原始指令快照与解析保真 | 590 CTest/186 ABI、Swift 四项、Android API 36 专用模拟器 35 项、Release/R8 与四模块 lint、八组编译变异通过；两端绑定同步 |
| 3 | 主机表单来源/保真/改名 | 73 项原生组合、16 个新保护与发布旧六组分轮通过；arm64 Release 通过 |
| 4 | 主机别名字节身份 | 所有普通 SSH/SFTP/tmux/分屏查找及选择器改为 ASCII 折叠、UTF-8 字节身份；50 项连接组合、76 项配置组合分轮、两组新变异和 Release 通过 |
| 后续 | 安全存储、终端设置、闸门必跑策略、SSH 网络/取消、文件与传输、保活及模拟器回归 | 顺序推进；未完成项不提前勾选 |

批 1 的六组均为隔离源真实编译后指定单条正常断言红、同构件独立配置读取绿、复原重编绿与 SHA256。成功操作保留连接快照/UUID，不通过刷新关闭连接；这不是外部 SSH 或整客户端回归结论。

批 2 新增 `gt_config_directive_at`，返回单个物理 Host 块内的原生参数、原文及来源；不混入 Include 展开，重复 Host 与 Match 在返回前显式拒绝。修复空白等号分隔、长 token 静默截断；NUL/未闭合引号失败，编辑候选解析失败保留旧树。Swift 文件入口拒绝无法无损往返的 UTF-8。

八组手工变异入口为 `python3 scripts/mutation-check-config-directives.py --checkout tmp/隔离检出`，不增加既有 CI 二十份。`tmp/rem18/tmp/directive-mutations-wpln432l/receipt.json` 记录 25 条（基线＋八组目标正常单断言红/同构件黄金对照绿/复原绿）及 SHA256。首轮探针未用变量编译失败、后续同秒增量未重链导致未杀死均保留，不计有效红；最终脚本强制重编重链。原始日志在同 worktree 的 `tmp/directives-*`：Core 全闸门、Swift 四项和既有宿主 50 项分轮，Android 专用 emulator-5680（API 36/arm64/4 KiB）35 项。Android 第一轮误用不存在的 transport lint 项目失败，修正为 app/bridge/terminal/data 四模块后 Debug/测试 APK、未签名 Release/R8/lint 通过。隔离构建复用逐字复制的锁定原生依赖，未改 vendor。该切片是接口与解析验证，主机表单另批验收；未发行，也不代表并行插件工作区全绿。

批 3 表单由 Core 快照驱动，脏字段按字节比较，多条身份逐行保存且引用内逗号/空格保真；明确本块/继承/默认来源，多别名共同影响、Include 只读与重复块/Match 失败不折为空表。未改保存零写盘；关闭本块值只删本块行，未知重复标量及 Include 路径要求原文件编辑。连接行排除否定模式并按 ASCII 大小写去重，ID 保留 Unicode UTF-8 差异。

新增/改名不覆盖已有同名块；字段与目标 token 改名由同一候选提交，碰撞不泄漏先改字段。预览重新求值，确认捕获原草稿和原字节快照；旧窗口不能覆盖后来提交。既有连接不会因成功编辑调用全局 reload；本片实际控件覆盖保存/取消/改名确认，未声称通道/PID 长时回归。

手工变异仍走 `scripts/mutation-check-macos-config.py --phase form|host-save|host-list|host-ui`：8＋5＋2＋1 个不同新保护，正常单条断言红、同构件独立 HostList 配置读取绿、复原重编绿及 SHA256；publication 既有六组在新代码复验通过，不增加 CI 二十份。原件为 `tmp/rem18/tmp/config-mutations-{u769enzr,i6dhwor6,9p0_lvs3,jpklm6xq,bhg71p51}/receipt.json`（form 目录以其最终日志为准）。最终同源 73 项组合和三项真实离屏原生控件通过，日志 `host-form-tests4.log`；首轮 SwiftUI 可选 Binding 编译失败、变异脚本未容许新文件的前置失败、dirty-only 旧目标触发非预期抛错均单列，未当有效红，最终改为独立正常断言并重跑。AppModel 当前并行插件修改仅按分离 hunk 保留，不据隔离构建宣布未完成插件组合全绿。

批 3 的最终 `host-form-release.log` 为 arm64 Release 构建通过；既有 Swift 并发警告保留，不将警告转称失败或删除代码换绿。

批 4 修复 Swift 规范等价比较造成的别名错配：`café` 的组合/分解写法仍分别解析为不同配置目标，列表选择 Hashable 与模型查找遵循同一规则，ASCII 大小写仍按 Core 折叠。三项新用例含实际 AppModel `remoteTarget` 的不同地址/用户/端口，身份核对不启动网络连接；既有连接选择器与表单组合 50 项、变异基线/最终配置组合 76 项分轮通过。`--phase target-match|target-selection` 两组正常单条断言红、独立读取绿、重编复原绿及 SHA256，原件 `config-mutations-amo8tjj_` / `config-mutations-5ufb_rzf`；arm64 Release `host-target-release.log` 通过。没有改插件发现、会话生命周期或 Core ABI。
