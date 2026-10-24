# Ghosterm 应用图标

## 概念

**简洁的白色幽灵剪影**：一只圆顶、两只眼睛、三个波浪裙摆的白色幽灵，
落在深靛蓝渐变底上——小尺寸下也能一眼认出剪影。

- `ghost` = 圆顶 + 波浪裙摆 + 两只眼睛（唯一的记忆点，剪影在 16–32px 也立得住）。
  幽灵在画布里**放到贴边**（约占画布 75% 高 / 64% 宽），深色背景只剩一圈
  ~12%（上下）/ ~18%（左右）的留白。
- **简洁优先**：不再画窗口边框 / 三个窗点 / 深色屏幕 / 投影——这些细节到了小尺寸会糊成一团；
  本版只保留「幽灵剪影」这一件事，其余全部丢掉。
- 两个「差异化」特征（`~/.ssh/config` 图形管理、SFTP）不进口标：小尺寸塞不下，
  且它们属于功能、不属于品牌。

配色取自母版（唯一事实来源）：

| 用途 | 颜色 |
|---|---|
| 背景 | `#232D5C` → `#0A0D1C`（中部叠一层紫蓝辉光 `#7A88FF`，34% → 0） |
| 幽灵 | `#FFFFFF` → `#F3F6FF` → `#C6D1F2` |
| 眼睛 | `#1A2142` |

## 唯一事实来源

- **`ghosterm.svg`（1024×1024）是母版**：手写 SVG，纯路径 + 渐变，不依赖任何字体，
  也不依赖任何端。
- **所有端产物都由它派生**，一律不要手改产物；改设计就改母版，然后重新生成。
- `scripts/build-icons.sh check` 会把「母版 + 脚本」与「已入库产物」逐字节对拍，
  母版改了却没重新生成 → 直接红。

## 产物与各端消费方式

| 端 | 产物 | 怎么用 |
|---|---|---|
| macOS / iOS | `apple/AppIcon.appiconset/` | 把整个目录放进 Xcode 的 `Assets.xcassets`（`Contents.json` 已含 `mac` 与 `iphone/ipad/ios-marketing` 两组 idiom）。**两边都是满画布直角 + 无 alpha**：圆角都交给系统裁（macOS 26 / iOS；见下「Tahoe 的底座」） |
| Android | `android/` | `mipmap-*/` 是 legacy 图标（含圆形）；`mipmap-anydpi-v26/` + `drawable/ic_launcher_foreground.xml` + `values/ic_launcher_background.xml` 是 adaptive icon 三件套。整目录内容对应 `app/src/main/res/` 下的同名路径 |
| HarmonyOS | `harmonyos/app_icon.svg` | 圆角独立 SVG（圆角外透明），可直接作为应用图标；`icon.svg` 是同内容副本 |
| Linux | `linux/` | `hicolor/` 就是 FHS 布局，装到 `/usr/share/icons/hicolor/`（或 `~/.local/share/icons/`）；`io.github.ghosterm.Ghosterm.svg` 作矢量兜底 |
| Web | `web/` | `favicon.ico`（16/32/48）、`favicon-32.png`、`icon.svg`、`icon-192.png`、`icon-512.png`、`apple-touch-icon.png`（180，不透明） |
| Windows | `windows/` | `Ghosterm.ico`（16…256 多尺寸）；`Ghosterm-44/-150`、`StoreLogo`、`Ghosterm.png` 给 MSIX / Store 清单。**全部连续圆角 + 透明角**（Windows 不遮罩应用图标）|
| 端侧只要「标记」 | `ghosterm-mark.svg` | 透明底、无投影、已居中放满画布；适合通知图标、内嵌 logo。注意幽灵本体是白的、眼睛是深色，**只适合深色底**，浅底请用整图 |

> **尚未接入打包**：Apple 端当前是 SwiftPM 可执行（没有 `.xcodeproj` / `.app` bundle），
> 所以图标先落在本目录、按上表在打包时接进去；其它端目录尚未开工，同理。

## 重新生成

```bash
bash scripts/build-icons.sh             # 全端
bash scripts/build-icons.sh apple       # 只做某端：apple|android|harmonyos|linux|web|windows|mark
bash scripts/build-icons.sh check       # 产物是否与母版/脚本一致（改了母版没重生成 → 红）
```

依赖：`rsvg-convert`（librsvg）· `magick`（ImageMagick）· `python3`（含 Pillow，用于几何断言）。
产物哈希记在 `SHA256SUMS`。

## 口径（生成脚本按这些口径断言，不靠自觉）

- **圆角**：**Apple（macOS + iOS）用满画布直角、不透明**——圆角交给系统裁（macOS 26 起
  也自己套遮罩，见下「Tahoe 的底座」）。其余端自带**连续圆角**（超椭圆角，指数 5，
  半径比例 `0.2237`，即 Big Sur 起的 Apple 观感）：Android legacy / Linux / Web / Windows
  都是这一档——Windows 不遮罩应用图标（Win11 任务栏 / 开始菜单直接显示原图），
  所以 `.ico` 帧与 MSIX PNG 都带透明角。只有 **favicon** 保留直角（浏览器标签直接展示）。
  圆角是 **SVG 里的 `clipPath`**，不是位图蒙版。
- **Android 前景**：108dp 视口，内容按 66dp 见方缩放居中，必须落在官方 72dp 安全区内
  ——`icons_check_android.py` 在生成时渲染 + 断言，越界直接报错。
- **位图**：一律由 `rsvg-convert` **按目标边长直接渲染**（矢量缩放），不做光栅重采样、
  不做位图合成。原因记在 `scripts/lib/icons.sh`：本机 ImageMagick 的 PNG `-resize`/`-composite`
  实测会产出斜向撕裂的坏图，这条路一概不用。
- **确定性**：同一母版 + 同一工具链 → 逐字节相同（产物不含时间戳；`-strip` 去元数据）。
- **Android VectorDrawable 的取舍**：mark 组里**每个** `<path>`/`<rect>`/`<circle>` 按文档顺序
  转成 VectorDrawable 路径（`icons_android.py`），渐变引用降为母版渐变的**平色近似**、不含投影
  （格式限制），但几何逐点来自母版。自适应图标有安全区 + 系统裁切，投影本来也不该带。

## 已知取舍

- **16px 档**：眼睛在 16px 会糊成两个灰点，只保证幽灵剪影可辨。本版已是「只留剪影」的
  简化版——窗点 / 屏幕 / 提示符这些更碎的细节一律不画；同时幽灵**刻意放到贴边**
  （约占画布 75% 高），小尺寸更有存在感、背景色块也更小。
- **macOS 26 的「底座」**：macOS 26 会给**非满画布**的旧式图标自动套一圈系统底座
  （浅色圆角框 + 内缩的图标），深色图标会被拆成「黑板 + 白框」，很难看（Ghostty 这类
  预圆角图标的通病）。所以 macOS 产物一律**满画布直角、不透明**，让系统干净地裁成圆角；
  实测：预圆角 → 有底座，满画布直角 → 无底座。本项目 `LSMinimumSystemVersion = 26.0`，
  不背旧版 macOS，故直接按 26 取。
- **iOS idiom 分组**：`Contents.json` 每个尺寸只给一条 idiom（76/83.5 归 `ipad`，
  1024 归 `ios-marketing`，其余归 `iphone`），Xcode 可能会提示补 iPad 专用尺寸。
