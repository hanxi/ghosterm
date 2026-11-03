# Android 终端配色与字体

设置 → 外观与终端提供独立的应用外观、终端配色、字体、字号、行距和光标设置。修改终端外观不重建会话或 PTY。

## 配色来源

保留石墨蓝、墨绿、纸白三个原有 ID；另内置当前 Ghostty pin 所使用的 725 个主题，共 728 种。与 macOS 打包的 Ghostty 主题库同源，支持名称搜索和深浅筛选。离线可用，不在设置页联网下载。

每个主题包含前景、背景、光标/光标文字、选区/选中文字及 16 色 ANSI 调色板。程序通过 OSC 和 DECSCUSR 设置的颜色、光标覆盖继续由 VT 管理；设置更新默认值，程序重置后使用新默认值。Canvas 绘制选区与光标文字；其余颜色经 TerminalSurface 的受确认配置进入 VT。应用 UI 的深浅主题独立。

`docs/design/android-terminal-themes.json` 为提取后的固定输入，`terminal/android/THEMES.json` 记录包 hash、Ghostty pin、来源与许可摘要。`scripts/build-android-appearance.py` 从这一输入生成 Kotlin 目录、HTML 数据和测试参考；默认 Android 构建执行 `--check`，并验证随包字体及许可摘要。更新主题库须一并更新来源收据和上游 pin，不手改生成的 Kotlin/JS。

## 字体来源与导入

提供系统等宽字体、JetBrains Mono、Source Code Pro。系统字体来自 Android；两个内置字体通过 `res/font` 随包，来源、固定版本与 SHA256 见 `terminal/android/FONTS.json`。两者为 OFL；许可与主题 MIT 声明可在关于页面读取。

导入使用 Android 系统文件选择器 `OpenDocument`，按次读取内容 URI，不保留 URI 授权。只支持包含完整 ASCII 正文字形、字宽一致的单字体 TTF/OTF；不支持 TTC、WOFF、比例字体或仅符号字体。每文件最多 4 MiB，最多 16 个导入字体，字体目录总量最多 32 MiB。读取有期限、可取消，可靠管道的生产者错误不会当成功。

应用验证字体自身 cmap/hmtx 与实际 Typeface 字宽后，以 SHA256 命名保存副本至应用私有 `files/fonts/`。目录 0700、文件 0600，暂存写入并同步后原子发布；名称元数据独立记录。重复内容不新增副本。读取验证普通文件、摘要及大小；损坏或缺失明确显示错误，不能静默宣称已应用。导入完成后需点选应用；移除需确认，已选字体不能移除。字体选择和删除经同一个设置 owner 排队，防止多窗口先检查后删除竞态。

字体解析和准备在 IO 线程完成，UI 使用已准备的 Typeface。绘制字体和网格测量字体一致，切换会重新计算单元格、基线并调整既有终端尺寸。正文优先使用所选字体，Nerd 符号来自既有 Nerd Symbols Mono，中文和 Emoji 由 Android 回退。Canvas 按终端单元格绘制，不承诺跨单元格编程连字。

偏好 schema 仍为 1，新增可选 `fontFamily`；旧 16 项文档缺此字段时默认系统等宽字体，无需重写旧文件。原主题 ID 不变，新主题使用 `ghostty:名称`，导入字体使用 `custom:SHA256`。

## 验证入口

- `python3 scripts/build-android-appearance.py --check`
- `python3 scripts/test-android-appearance-assets.py`：隔离副本中的九组输入、许可和生成产物漂移守卫。
- `TerminalAppearanceCatalogTest`：固定目录、旧偏好、私有副本、字形校验、完整性、预算、取消和选择/删除顺序。
- `TerminalAppearanceProductTest`：真实主题/字体选择、实际字形和网格、会话 PID、系统 DocumentsUI 导入及旋转、可靠流失败、取消和删除。
- `TerminalPreferencesTest`：实际 VT 调色板、OSC 覆盖/重置、Canvas 选区/光标和既有 owner。
- `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-appearance.py`：真实编译后正常断言红、同 APK Core 配置绿、恢复绿与 SHA256。

以上为定向手工验收入口，不自动增加 macOS CI 的 20 份覆盖。实际执行范围与失败记录见 Android 实施计划；默认完整客户端、物理设备和全部字体提供方矩阵另行验证。
