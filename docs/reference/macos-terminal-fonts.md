# macOS 终端字体选择

> 临时材料状态（2026-11-04）：本文列出的旧 `tmp/` 日志、截图、包与 AVD 已按用户要求清理；下述记录是历史摘要，不能再将这些路径当作现存附件或本轮复验。已入库的设计图片及 `site/assets/` 截图仍可访问。


入口：设置 → 外观 → 终端字体。可搜索引擎默认字体、应用内置字体和本机等宽字体，预览后点“应用”；取消或仅浏览不改配置。

默认选项复用 full libghostty 的 JetBrains Mono，不重复打包。Source Code Pro 来自 Adobe 固定提交 `803b7e23ec97ae58b6232ea76519a76d428ba268`，随包提供常规、粗体、斜体、粗斜体四个原始 TTF；其常规字形与 Android 所用版本一致。来源、SHA256 与 OFL 许可摘要位于 `apple/GhostermApp/Sources/GhostermAppModel/Resources/Fonts/manifest.json`。字体册管理的本机字体通过 Core Text 动态发现，无应用内导入或联网下载。

## 原生实现

- 在加载终端配置之前，使用 Core Text 的 `.process` 范围注册随包字体；不安装到系统，不修改用户字体目录。注册后逐一核对实际字体文件 URL，避免同名字体覆盖后仍假报随包字体已载入。
- 系统字体按实际默认字形筛选等宽家族。某些家族同时有比例与等宽字形，不能仅因为其中一款是等宽就列出整个家族。
- 发现字体在后台执行，菜单按家族去重并标记来源；预览用实际字体。引擎默认字体没有公开的 AppKit 字体对象，选择器明确说明到终端查看实际效果，不用 Menlo 等系统回退字形假冒预览，也不为预览再塞一份 JetBrains Mono。
- 字体偏好仍只有 Ghostty 配置文件这一份真相。选择经 `AppModel` 走既有备份、冲突检测、原子保存与 full libghostty 配置重载。已有终端面、会话和 shell 保持；字体度量与绘制由引擎更新。
- 选择单个字体会替换本文件中的全部 `font-family` 声明，重设默认会移除这些声明。注释、其它设置与粗体/斜体专用配置保留。打开选择器不重写未知字体或用户已有的字体回退列表。
- 有 `config-file` 附加配置时，选择器明确提示在配置页修改并禁止直接应用。此切片不另写一套 Ghostty 配置解析器来推测包含文件的字体覆盖顺序。
- 随包字体失效与终端配置无效分开报错；可继续使用引擎默认或本机字体，不能把注册失败报成成功。

SwiftPM 资源 bundle 会显式放入 `.app/Contents/Resources/`。正式 `.app` 的加载入口只认包内资源，不回退开发机的 `.build` 路径；开发态使用 `Bundle.module`。字体与许可在打包前后均校验，关于窗口可查看 Source Code Pro 完整 OFL 许可。

本切片未改 Core C ABI、Transport 或终端输出通路。

## 验证（2026-11-04）

使用完整 Xcode 的 `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`，临时目录在工程 `tmp/`。系统全局 `xcode-select` 保持不变。当前工具链为 Swift 6.4 / macOS 27 SDK；测试采用 SwiftPM native backend，打包采用脚本默认 backend，两种 SwiftPM bundle 布局都已实际加载验证。

- Core `bash scripts/gate.sh`：50 CTest / 94 ABI 通过。
- 新增 `TerminalFontTests` 13 项通过：来源/四种字形、缺失与损坏、字体目录/筛选、已保存配置、备份/重开/默认、外部修改、附加配置、预览草稿、原生视图、实际终端重载与同一 shell PID。
- 最终 `TerminalFontTests|SettingsUITests|TerminalConfigTests|AboutWindowTests` 关联组合执行 57 项，56 通过、1 个既有开发态主题资源用例跳过，零失败；实际包的主题解析检查另由 `package-mac.sh` 通过。不是完整宿主或物理显示器矩阵。
- 实际终端像素验证先证明同一 Menlo 画面两次抓帧一致，再验证 Source Code Pro 字形变化，shell PID 与终端面保持；不拿光标闪烁或换会话冒充字体切换。
- `mutation-check-macos-fonts.py` 十组分轮通过：真实编译后正常单条断言红、同变异独立配置对照绿、恢复绿及 SHA256。生产源始终未被变异，变异仅在工程 `tmp/` 隔离副本。该手工脚本不增加 macOS CI 的 20 份覆盖。
- 本地 ad-hoc `.app` 构建、嵌入依赖、签名、最小系统版本、随包主题解析及装载镜像检查通过。包中只有新增 Source Code Pro 四个字形及其许可，没有另打包 JetBrains Mono。
- `test-macos-font-package.py --app 应用路径` 四组包内资源保护通过：移除整个字体 bundle、缺少粗体、损坏斜体、损坏许可时，真实可执行文件 `--font-check` 都明确失败；同一变异包 `--config-check` 对照绿，复原绿且内容摘要一致。检查确认加载路径位于当前 `.app` 内，范围为 process。

初验记录保留：Command Line Tools 环境缺 XCTest，改用单次命令完整 Xcode；系统字体 Osaka 家族默认字形不满足等宽要求，已过滤；新增许可入口后的整窗按钮计数同步，并追加独立更新行单按钮断言，保留无安装按钮保护；既有键盘页裁切修复为显式正文角色与 640 pt 内容高，外观页新增字体后为 580 pt，最终四页底部余量为 23/26/33/22 pt，原断言上下界未放宽。像素探针的 PTY 参数、独立截图目录及首轮变异中的 Swift 抛错表达式修正后重跑，编译失败或夹具失败均未计为目标红。

当前候选 `.app`、原生截图、逐轮日志、输入和产物摘要位于 `tmp/macos-fonts/`；包保护及设备变异收据在日志所列 `tmp/macos-font-package-*` / `tmp/macos-font-mutations-*`。本地构建保留 dirty 来源，不称云端发行或公证通过。
