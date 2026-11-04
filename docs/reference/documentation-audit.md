# 文档逐份核对记录（2026-11-04）

源码基线 `d9ef45a`。检查仓库自有 Markdown、设计 HTML、随包许可文本及相关资源清单；vendor 上游文档不改写。表中“保留”表示核对后没有发现需要修改的当前描述，不是漏查。

当前功能对照生产调用方、公开头、默认值、固定资源 manifest 与工作流；历史实施日志按日期和提交核对后续替代关系，不将删除的原始日志重新认证为实测。本轮不声称重新执行全部历史设备/变异/完整宿主测试。

## 逐份 Markdown

| 文档 | 处理 | 核对与修正 |
|---|---|---|
| [.github/release-notes/dev.md](../../.github/release-notes/dev.md) | 保留 | 核对两端共用正文接线、8 个资产名、安装和各自收据入口。 |
| [AGENTS.md](../../AGENTS.md) | 保留 | 核对目录/引擎/临时目录/CI/ABI/提交规则；历史切片数量按日期保留，验收规则未改。 |
| [CLAUDE.md](../../CLAUDE.md) | 保留 | 仅转引 AGENTS.md，目标存在。 |
| [README.md](../../README.md) | 保留 | 核对下载名、平台边界、目录及开发命令；无 Lynx 归档 tag 残留。 |
| [android/README.md](../../android/README.md) | 修正 | 修正 API 29 无证据、跨远端确认、发行入口及清 tmp 后 Zig 配置；连接复用与源码一致。 |
| [apple/README.md](../../apple/README.md) | 修正 | 修正当前 ABI、Android tmux/设置/粘贴/预览已接入状态；区分历史设置失败与后续布局修正。 |
| [assets/icons/README.md](../../assets/icons/README.md) | 修正 | 删除图标设计决定整个项目最低 macOS 26 的旧说法；保留母版、派生和系统遮罩证据。 |
| [core/tests/README.md](../../core/tests/README.md) | 修正 | 修正 Android 已消费非覆盖提交及 golden 输入/两份再生输出；逐项核对脚本与公开头。 |
| [docs/README.md](../README.md) | 修正 | 导航条目与所有目标/锚点核对，并链接本审阅记录。 |
| [docs/architecture/overview.md](../architecture/overview.md) | 修正 | 修正设置、进度/暂停/重试、缓存/外部打开、FIFO/跨远端确认的旧待办；分层约束不变。 |
| [docs/design/tern-reference/README.md](../design/tern-reference/README.md) | 保留 | 原始截图来自官网网页演示，非原生截图；核对尺寸/图片路径及历史日期。 |
| [docs/explanation/licensing.md](../explanation/licensing.md) | 修正 | 区分实际平台与规划，收回关于窗口即可代表全部组件盘点的笼统断言；原始许可不改。 |
| [docs/how-to/android-device-check.md](../how-to/android-device-check.md) | 修正 | 按新菜单定位文件入口，修正 tmux 手指轻点不弹键盘的旧预期，补横滑/连发检查。 |
| [docs/how-to/android-release.md](../how-to/android-release.md) | 修正 | 改为可执行的下载/校验/CI 指南；移除重复旧包摘要，历史详情仍在实施日志。 |
| [docs/how-to/dev-build.md](../how-to/dev-build.md) | 修正 | 改正已删除的 ZIP/DMG 文件名、Android 无发布渠道、tag 删除重建等说法；保留不同签名档位证据。 |
| [docs/how-to/mouse-report-recovery.md](../how-to/mouse-report-recovery.md) | 保留 | 核对显式安装/预览参数、脚本路径、Shell 钩子与历史验证范围；无新发现需改。 |
| [docs/how-to/tmux-sessions.md](../how-to/tmux-sessions.md) | 修正 | 修正 Android 未接列表/attach，明确本文标签操作仅属 Mac。 |
| [docs/reference/android-implementation.md](android-implementation.md) | 修正 | 修正当前契约中的旧终端布局、字体度量、已发行记钥及发布描述；§9.1–§9.204 逐节核对先后依赖，旧失败/数值/摘要不改写。 |
| [docs/reference/android-terminal-appearance.md](android-terminal-appearance.md) | 保留 | 核对 725+3 主题、两正文内置字体、导入预算/限制、默认值与生成器；无需改。 |
| [docs/reference/android-visual-implementation.md](android-visual-implementation.md) | 修正 | 明确已清理的临时审核页/截图/AVD；201 项视觉基线与后续定向分别保留。 |
| [docs/reference/apple-ui-pitfalls.md](apple-ui-pitfalls.md) | 保留 | 核对 14 条工程经验的生产/测试入口；示例是历史失败/规避而非当前功能承诺。 |
| [docs/reference/capability-matrix.md](capability-matrix.md) | 修正 | 修正 EOF 完整测试、暂停/安全重试/历史发起、关系确认、设置/外部打开及发行 minos。 |
| [docs/reference/current-status.md](current-status.md) | 修正 | 分别读取线上 Mac/Android 实际来源；更新绑定数，区分布局修复与完整宿主重跑。 |
| [docs/reference/host-ssh2-build.md](host-ssh2-build.md) | 修正 | 修正 Android 调度/后台仍未接入的旧结论，保留受控操作/独占 owner 契约。 |
| [docs/reference/macos-terminal-fonts.md](macos-terminal-fonts.md) | 修正 | 对照字体 manifest、SettingsView 580/640 高度与历史 56+1 记录；标明临时包已清理。 |
| [docs/reference/optional-tmux-implementation.md](optional-tmux-implementation.md) | 修正 | 核对退役闭包/命令/信号/输入修复，保留各轮历史，标记临时取证已清理。 |
| [docs/reference/plan-files-manager.md](plan-files-manager.md) | 保留 | 核对旧双栏已替代、当前消费者及目录/预览/传输缺口；S1–S7 保留为历史。 |
| [docs/reference/plan-optional-tmux.md](plan-optional-tmux.md) | 修正 | 修正 Android 尚待 A8 的过时状态，保留当年计划不包含 Android 的验收范围。 |
| [docs/reference/plan-panel-tab-menu.md](plan-panel-tab-menu.md) | 保留 | 核对 FlowLayout、TabContextMenu 与窗格动作的替代关系；旧全局按钮方案已注明历史。 |
| [docs/reference/plan-tab-chrome-refactor.md](plan-tab-chrome-refactor.md) | 修正 | 核对单窗口、Studio/Rail、菜单与切片历史；标明 tmp 探针和取证不可再取。 |
| [docs/reference/plan-tabs-close-and-newtab.md](plan-tabs-close-and-newtab.md) | 保留 | 核对 TabKind.localFiles、当前关闭/继承接线与旧 R1/R2 区别；历史契约不改。 |
| [docs/reference/roadmap.md](roadmap.md) | 修正 | 替换过时 A2–A10 清单；原切片表明确历史，不再用旧 929 项当最近完整记录。 |
| [docs/reference/workspace-handoff.md](workspace-handoff.md) | 修正 | 修正移动端后续已交付能力，区分旧设置失败和后续修复；临时交接材料不可再取。 |
| [patches/README.md](../../patches/README.md) | 修正 | 补齐 Android VT 0003–0005 概述、修正 Android 受控 SFTP 未接说法。 |
| [patches/libghostty/8159838/WHY.md](../../patches/libghostty/8159838/WHY.md) | 保留 | 对照 pin 与 0001–0003 实际补丁路径、导出/线程接缝、撤除条件；无变更。 |
| [patches/libghostty/android-vt-8159838/WHY.md](../../patches/libghostty/android-vt-8159838/WHY.md) | 保留 | 对照 0001–0005、libc/clear/Kitty 边界与构建收据；无变更。 |
| [patches/libssh2/1.11.1/WHY.md](../../patches/libssh2/1.11.1/WHY.md) | 修正 | 修正 Android 文件 owner/产品取消未交付说法；九场景与七组历史证据保留。 |
| [runtime-payload/README.md](../../runtime-payload/README.md) | 保留 | 仅规范，未实现安装器；核对 API 29 执行限制、PoC/校验/激活要求，无当前能力误报。 |
| [site/README.md](../../site/README.md) | 保留 | 核对静态目录、Pages 工作流/域名、三张截图收据与固定下载名；无需改。 |
| [terminal/README.md](../../terminal/README.md) | 修正 | 清理混入接缝说明的旧文件里程碑；改为当前引擎/宿主职责和证据范围。 |
| [terminal/android/README.md](../../terminal/android/README.md) | 修正 | 修正 14sp/设置未接/固定系统字体度量，补主题字体与触屏/连发的当前接线。 |
| [terminal/apple/README.md](../../terminal/apple/README.md) | 修正 | 核对 full 引擎回调/主线程销毁/剪贴板边界，补旧 tmp 取证已清理说明。 |

## 设计、资源与原始许可

- `docs/design/android-prototype.html`：保留原设计交互，增加清楚可见的历史布局说明；§9.203–§9.204 的原生菜单/全屏方案优先。
- `docs/design/tabs-chrome-prototype.html`：核对实际原生截图/官网参考的区分、PNG 路径与 JSON 收据；保留。
- `docs/design/chrome-visual-options.html` 与 `chrome-studies/window.html`：历史方案比较与示意图，非当前客户端；资源路径均存在，保留。
- `site/index.html`：下载与文档目标存在；三张图片 SHA256 与 `screenshots.json` 一致。页面已经明确历史测试截图范围，保留。
- `android-appearance-catalog.js`、主题 JSON、UI 图标 SVG：生成器 `--check` 通过，不手改生成内容；字体/主题 manifest 与许可摘要检查通过。
- `tab-chrome-native/layout-review.json`、`desktop-observations.json`：保留历史截图/系统观察收据，不解释为当前完整回归。
- 仓库 LICENSE 及 Apple/Android 共五份随包字体/主题许可 TXT：原始版权/许可全文不改写；字体来源与摘要随各自 manifest/生成器核对，不将 Nerd Fonts 全部图标集统一称为 MIT。

## 本轮验证与限制

42 份既有 Markdown 逐份核对；连同本记录，43 份 Markdown 的 695 个本地文件/锚点及 5 份 HTML 的资源引用检查通过，三张网站截图 SHA256 对拍通过；Homebrew 远端 cask 实读仍为 arm64 / Tahoe / no_check，与安装说明一致。Core 51 CTest / 95 ABI、Apple 91/4 与 JNI 57/38 对拍通过，Apple 原生编译通过。Android Debug/测试 APK 构建通过；不以文档改动替代设备验收。

清空 `tmp/` 后旧 Zig 路径失效，首次 Android 构建因此失败，不算通过。已从官方发行索引下载并核对 SHA256，将工具链放入 `build/android-toolchain/`，再显式设置 ZIG 重试。旧 AVD/设备取证未复原；历史条目中的临时路径不可用，原摘要保留。
