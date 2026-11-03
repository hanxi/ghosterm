# Android 原型视觉落地

基准为 `docs/design/android-prototype.html` 的最终 b 方案与用户确认的终端单行输入栏、文件远程/本地 Tab。以原生 Compose 实现；不嵌入 HTML，不把浏览器截图当作原生验收。

## 验收范围

| 顺序 | 交付 | 当前证据 |
|---|---|---|
| 1 | 完整明暗主题、文字层级、形状和统一尺寸；终端配色独立 | `GhostermTheme.kt` 映射完整 Material3 角色、15 档文字、5 档形状；CSS 明暗角色在真实设备对拍 |
| 2 | 同源 SVG → HTML 路径与 Android VectorDrawable；构建拦漂移 | 31 个 SVG 图标；Android 真实矢量栅格与 SVG 路径对拍；生成器检查已接默认构建 |
| 3 | 图标按钮、图标底板、分组、行、分隔与页面布局共用组件 | `GhostermComponents.kt` 统一组件；实际绘制、主题 tint、无障碍标签及 48dp 触控区有设备断言 |
| 4 | 主机首页、终端、文件、设置总览/五类详情；编辑/身份/信任/关于/任务沿用同一主题 | 已实现并自查；API 29 / API 36 各 120＋81 项组合通过，覆盖所有本轮修改的既有测试类及新增资源/组件/截图类 |
| 5 | 同尺寸/明暗/字号截图对照；系统键盘、真实终端与 SFTP 仍以设备测试证明 | API 29 / API 36 各 40 状态 × 明暗共 80 张，共 160 张。两套原型对照页与状态收据已生成；供用户审核 |

## 一致性约束

- 原型图标母版为 `docs/design/android-ui-icons.svg`。修改后运行 `python3 scripts/build-android-ui-icons.py`；`--check` 验 HTML 与 Android 产物。原启动图标母版和构建链独立，不改变。
- 原型 CSS 的 b 方案提供背景、表面、文字、强调、选中及边框色。Material 的其它语义角色在 `GhostermTheme.kt` 显式映射，禁止留下不属于设计的默认紫色。
- 图标绘制 22–24dp，触控区域至少 48dp；尺寸采用 dp，文字采用 sp 并尊重系统缩放。长名称、横屏与实际 IME 不许截断控制或挤掉全部终端内容。
- 业务行为、三态结果、认证、剪贴清单和任务所有权保留。主机状态来自实际会话，不能用演示的绿色圆点假装已连接。
- 首页保留生产“本机文件”入口；后台 tmux、主机编辑及独立 SFTP 从主机菜单进入。文件任务条继续展示真实状态，图形不得盖住未知/失败。
- 验收包括主机（列表/空/搜索/继续会话/菜单）、终端（收起/展开键盘/会话面板）、文件（本地/真实远端/选择/清单/空远端）、设置（总览、五类详情及选择/重置对话框）。扩展页面逐项核对主题与图标。
- 浏览器与系统字体的字形和抗锯齿差异允许存在；内容层级、图标形状、颜色、间距和入口需对齐。系统键盘、系统文件选择器和系统权限不仿绘。
- 功能测试与视觉审核分开记账；功能绿不能宣称视觉已还原。未测状态/设备必须留为未验。

## 视觉审核口径

`AndroidVisualReviewTest` 启动生产 MainActivity 与实际控制器，保存并恢复配置/偏好，使用独立目录与加密测试身份。截图中的本地终端来自实际 PTY，远端列表来自隔离的真实 SFTP，复制结果须核对目标文件原始内容，历史由该次任务归档产生。没有产品预览入口，也不把演示数据写入发行版。

普通基准为 388×844、密度 1、系统字号 1；另拍 320×640/640×320、字号 2 的文件页和实际系统键盘下的终端。API 29 的导航栏下沿为 0，API 36 为 24px，HTML 分别按相同窗口尺寸和系统栏空间渲染。状态栏时钟/系统图标、字体栅格、系统输入法不作逐像素相等要求。原生焦点、波纹与拖动柄 Tooltip 保留；Material3 1.4.0 会在调用方自定义的拖动柄外层添加 `DragHandleWithTooltip`，因此截图中仍可能出现随系统语言的短暂提示（本轮模拟器为英语），不是图标字体缺失。

这些差异是实际产品适配，审核时必须保留说明：

| 页面 | 与 HTML 的差异及原因 |
|---|---|
| 主机 | 保留“本机文件”直接入口；列表因此下移。图标统一表示 SSH 主机，配置没有主机图标字段，不根据演示名称猜测设备类型。任务摘要使用整行，容纳真实状态。 |
| 终端 | 展示真实会话名称、状态与输出；键盘由 Android 提供。极短横屏/大字号使用侧边控制布局，内容高度受系统键盘和用户字号限制。 |
| 文件 | 展示真实端点、路径、大小与任务状态；无活动会话时“回到终端”禁用。常规页只在有实际任务/历史/错误时显示任务摘要，空闲时底部时钟仍能打开任务页。 |
| 文件菜单 | 目录和项目操作采用原生锚定菜单；位置选择、清单采用底部面板。额外的预览缓存维护来自现有生产能力。 |
| 主机编辑、SSH 配置 | 原型为简化弹层；生产保留完整页面，支持继承来源、端口、身份关联和配置编辑，不把长表单塞进固定高度弹层。 |
| 设置 | 选择项使用底部面板；重置/放弃等确认使用原生对话框。保存中、失败、未知与候选修改提示保留，不能为了画面隐藏。 |
| 身份与验证 | 展示真实列表/公开元数据；不展示原型虚构的已验证主机。认证窗口保持 `SecureFlagPolicy.SecureOn`，不绕过防截屏；用操作断言验证，认证后的目录、身份和公开指纹可截图。 |
| 任务 | 使用实际预检/认证/执行/持久记录页面，比原型演示面板多出来源核对、通知状态及历史操作，不能用演示百分比替换。 |

## 复现与审核材料

```bash
python3 scripts/build-android-ui-icons.py --check
python3 scripts/test-android-ui-icons.py
ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.AndroidVisualReviewTest,com.hanxi.ghosterm.AndroidDesignResourcesTest,com.hanxi.ghosterm.AndroidDesignComponentsTest,com.hanxi.ghosterm.RemoteFilesProductTest#remoteAndLocalTabsRetainDirectoriesClipboardAndTheSameFileConnection bash scripts/test-android-ssh.sh
# 从应用私有 cache/visual-review 导出 PNG 和明暗 manifest 到工程 tmp/。
# 启动工程 tmp/ 下独立配置的 Chrome 调试端口，再渲染原型：
node scripts/capture-android-html-reference.mjs 端口 0
node scripts/capture-android-html-reference.mjs 端口 24
python3 scripts/build-android-visual-review.py --captures tmp/android-design/screens-final-36 --reference tmp/android-design/reference-388-inset-24 --output tmp/android-design/review-api36.html
```

HTML 参考脚本只去掉展示外框、调整系统栏占位并设置明确的预览状态/示例内容，不重排页面来掩盖差异。生成的审核页核对 PNG 尺寸与状态收据，支持主题/页面筛选和打开原尺寸；没有对应原型的状态明确标注。截图与设备日志保存在 `tmp/android-design/`，不入库。

## 验证记录（2026-11-03）

- Core：`bash scripts/gate.sh` 通过，50 CTest、94 ABI；未修改 C ABI 或核心实现。
- 图标生成器：七组隔离保护通过，涵盖输出漂移、线宽同步、重复编号、不支持的形状/变换/线端与 CSS 调色板漂移。都是工程 `tmp/` 副本，未改生产资源。
- 设计设备变异：五组（浅色、深色、图标误绑、tint 误绑、无障碍名称丢失）在 API 29 编译后单条正常断言红、同 APK Core 配置绿、恢复绿及 SHA256；追加共用拖动柄后已复验同五组。不是编译失败冒充目标红。
- 设置设备变异：C11–C15 五组、C16–C24 九组分轮通过。首轮 API 29 的系统夜间模式夹具没有传递所需权限，基线失败；入口已与现有 SSH 测试入口采用相同的锁定模式检测，仅在锁定模拟器上给测试系统设置命令所需权限，生产应用没有获得权限。其间新增截图用例的 String/UUID 与枚举编译错误未计作目标红，修正后重跑余下九组。
- 关联初验：API 29 曾有 41 项、68 项及后续 30 项分轮通过，不相加冒充一轮完整回归；最终源的完整关联组合见下文。
- API 36 初轮 120 项有四处失败：两个截图用例未滚动到懒列表新增主机；一处主机编辑测试未等待页面切换；一处横屏两倍字号的文件页被双行标题挤到不足一行。修正后 18 项相关组合通过，包含完整 80 张截图、真实复制、真实 SFTP、全部小窗口用例；不以 18 项代替原 120 项全绿。
- 认证窗口截屏返回空值是其防截屏保护的预期行为；测试不再尝试截该窗口。前期截图同步、弹窗多根节点与选择状态切换的失败均未计入通过记录。

最终源验证：

| 设备 | 主页面/资源/组件/截图组合 | 文件授权/清单/历史/维护补充组合 | 合计 |
|---|---|---|---|
| API 29 / arm64 / 4 KiB 页 | 120 项，326.738 秒 | 81 项，320.139 秒 | 201 个不同用例 |
| API 36 / arm64 / 16 KiB 页 | 120 项，374.287 秒 | 81 项，380.538 秒 | 201 个不同用例 |

两个组合分别执行，不称一次默认全量测试。每轮开始/成功结束的具名集合与声明数量一致，零失败/错误/跳过；跨设备具名集合相同。627 份 Android、脚本与设计输入在各轮前后及跨设备 SHA256 一致。实际安装的主/测试 APK 摘要与构建产物一致，分别为 `ce023ff0285c38097bdf8643deef32365992573716e22fd20c8457dca5ebfc53` / `0b84efeccbb23c282ba847b60bacd67a415c072c76b36113dbe2a1731ccefa5f`。

终端布局四组、任务入口/列表空间四组、清单详情/粘贴/清除与新增紧凑标题四组，共十二个不同保护组在 API 36 分轮通过：编译后单条正常目标断言红、同 APK Core 配置对照绿、复原绿与 SHA256。第一轮在已通过八组之后发现清单详情锚点因普通/紧凑两个分支而变为两处，停止；同步为紧凑分支的唯一锚点后完成余下四组，没有把定位错误算目标红。

最终 Debug/测试 APK、未签名 Release（实际 R8/资源优化）与 app/bridge/data/terminal 四模块 release lint 通过。未签名 Release 为 8,202,395 字节，SHA256 `60c7081d91fcaee3bb498dde1321d9a94461f7c5212c1759a5144a2194ae2a2d`；保留工作区 dirty 来源，不称云端发行或固定证书安装包。

审核入口为 `tmp/android-design/review-api29.html` / `review-api36.html`，各自包含完整 80 状态；`final-device-results.json`、四份 `final-api*-receipt.json`、`final-inputs-before.json`、`artifacts/sha256.json` 保留具名集合、输入与实际 APK 证据。生成器、主题、图标、共用组件、主页面和扩展页面的自查已完成；用户视觉审批另行进行。

以上均为手工设备证据，不增加 macOS CI 的 20 份覆盖，不代表默认完整 Android 客户端、全部输入法或物理设备矩阵已完成。

本轮新建的 `ghosterm-design-29` / `ghosterm-design-36` AVD 在验收后均 sync 并正常停止，保留 userdata；独立截图浏览器已关闭。原 QA AVD 未启动或修改，审核 HTML/PNG/日志继续保留。

### 后续反馈：空闲任务摘要隐藏

用户反馈后，首页与文件页均在没有当前任务时隐藏摘要卡片；仅有历史也不再撑出整行卡片。首页有历史时提供时钟图标，普通文件页保留底部时钟，紧凑文件页在更多菜单中保留“文件任务”。当前任务（包括失败/结果不明/待确认）、记录读取中与读取错误仍可见。HTML 首页同步去掉无任务的演示卡片。

API 36 / arm64 / 16 KiB 页的真实复制/归档/历史入口与小窗口组合 11 项通过（23.124 秒）；三组手工变异保护空闲隐藏、活动摘要打开和归档后历史入口，编译后正常断言红、同 APK Core 配置绿、复原绿与 SHA256。Core 50 CTest / 94 ABI、Debug/测试 APK、最终未签名 Release 与四模块 lint 通过。此反馈后的范围为该定向组合，前文 201 项及 160 张是前一版完整基线。

最新截图入口为 `tmp/android-design/empty-task-review.html`，日志为 `empty-task-tests36.log` / `empty-task-final-release.log` / `empty-task-core.log`，产物摘要为 `empty-task-artifacts/sha256.json`。

### 后续反馈：主题库与字体

主题库、字体导入和实际渲染的结构及限制见 [Android 终端配色与字体](android-terminal-appearance.md)。HTML 使用同一份生成主题目录与相同的内置字体资源；系统文件选择器在原型中仅展示入口，真实导入由 Android 产品测试验证。最新状态截图与验收日志保存在 `tmp/android-appearance/`，前文 201 项为此前视觉基线。

最终相同生产源/APK 的主题、字体及终端组合在 API 29 / 4 KiB 页与 API 36 / 16 KiB 页各 181 项通过，关于许可另各六项补充通过；两台设备各七张新截图保存在 `tmp/android-appearance/index.html`。Core 50/94、最终未签名 Release 与四模块 release lint 通过；输入、实际安装摘要、十组新增设备变异与失败修正记录见实施计划 §9.197。前文的完整视觉基线未作为本切片全量重跑结果。
