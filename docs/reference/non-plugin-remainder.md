# 非插件剩余实施记录

按用户“剩下的也处理完”继续推进可实施的非插件功能与自动化验收；插件修改由另一 session 处理。Android 自动化使用自有模拟器，手机体验按既定人工手测分工。环境包等已明确延后扩展不因本次请求自动启用；发行/证书和人工系统操作分别记账。

临时构建与夹具位于工程 `tmp/rem18`，每批提交本轮文件，保留另一 session 的暂存索引。验收规则仍以 AGENTS.md 为准，不将故障/编译异常当正常变异红。

| 批次 | 范围 | 结果 |
|---|---|---|
| 1 | SSH 配置候选发布 | 50 项组合、六组变异、arm64 Release 通过；备份拒绝/新建失败/parse→store 变化/提交后错误/主机和规则发布均覆盖 |
| 2 | Core 原始指令快照与解析保真 | 590 CTest/186 ABI、Swift 四项、Android API 36 专用模拟器 35 项、Release/R8 与四模块 lint、八组编译变异通过；两端绑定同步 |
| 后续 | 主机表单来源/保真/改名、安全存储、终端设置、闸门必跑策略、SSH 网络/取消、文件与传输、保活及模拟器回归 | 顺序推进；未完成项不提前勾选 |

批 1 的六组均为隔离源真实编译后指定单条正常断言红、同构件独立配置读取绿、复原重编绿与 SHA256。成功操作保留连接快照/UUID，不通过刷新关闭连接；这不是外部 SSH 或整客户端回归结论。

批 2 新增 `gt_config_directive_at`，返回单个物理 Host 块内的原生参数、原文及来源；不混入 Include 展开，重复 Host 与 Match 在返回前显式拒绝。修复空白等号分隔、长 token 静默截断；NUL/未闭合引号失败，编辑候选解析失败保留旧树。Swift 文件入口拒绝无法无损往返的 UTF-8。

八组手工变异入口为 `python3 scripts/mutation-check-config-directives.py --checkout tmp/隔离检出`，不增加既有 CI 二十份。`tmp/rem18/tmp/directive-mutations-wpln432l/receipt.json` 记录 25 条（基线＋八组目标正常单断言红/同构件黄金对照绿/复原绿）及 SHA256。首轮探针未用变量编译失败、后续同秒增量未重链导致未杀死均保留，不计有效红；最终脚本强制重编重链。原始日志在同 worktree 的 `tmp/directives-*`：Core 全闸门、Swift 四项和既有宿主 50 项分轮，Android 专用 emulator-5680（API 36/arm64/4 KiB）35 项。Android 第一轮误用不存在的 transport lint 项目失败，修正为 app/bridge/terminal/data 四模块后 Debug/测试 APK、未签名 Release/R8/lint 通过。隔离构建复用逐字复制的锁定原生依赖，未改 vendor。该切片是接口与解析验证，主机表单另批验收；未发行，也不代表并行插件工作区全绿。
