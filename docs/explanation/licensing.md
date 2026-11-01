# 许可安排

仓库整体 **MIT License**（`LICENSE`）。

## 为什么是 MIT

共享核心与终端引擎的代码依赖主要为宽松许可组件；仓库自身的 MIT 许可不替代各组件许可：

| 组件 | 许可 | 用途 |
|---|---|---|
| libghostty / Ghostty | MIT | 终端引擎（Mac/Android/HarmonyOS/Linux/Web） |
| libssh2 | BSD-3-Clause | SSH / SFTP |
| mbedTLS / OpenSSL | Apache-2.0 / Apache-2.0 | SSH 加密后端 |

随包分发这些组件时，把各自许可与版权声明亮给用户（App 内「关于」窗口 +
运行时包内 `LICENSE`）即满足义务；**不需要**随包附源码归档。

## Android 终端符号字体

Android 内置未修改的 **Symbols Nerd Font Mono 3.5.1**，用于 Nerd Fonts 图标回退；普通文字仍走系统等宽字体。上游发行地址、归档及字体 SHA256 记录在 `terminal/android/NERD-FONTS.json`。它不是 Termux 环境包，不需要额外下载才能显示图标。

Nerd Fonts 工具本身为 MIT；字体合并的图标集还包含 CC BY 4.0、Apache 2.0、OFL 等各自许可，不能统一称为 MIT。原始发行 README 的图标来源、版本、许可表与版权说明，以及上游 glyph 目录提供的许可证，随包合并为 `licenses/NerdFontsSymbols.txt`，由 Android「关于 → Nerd Fonts · 字体与图标许可」展示。保留原始 TTF，不做删字、改名或重新绘制；上游 README 对 Font Logos 的 `unlicensed` 标注也按原文保留，不改写其许可状态。

## GPL 组件的红线（iSH / Termux，均未随包）

MIT 不豁免**第三方 GPL 组件自身的**许可义务——它们按什么许可分发，义务就
按什么算，与本项目用 MIT 无关：

| 组件 | 许可 | 现状 |
|---|---|---|
| **iSH** | GPL-3.0 | iOS 本地 shell，**延后、未集成** |
| **Termux bootstrap** | 各包混合（多 GPL） | Android 环境包，**按需下载、不进 App 包** |

一旦这两类组件真的随二进制分发（打进 IPA/APK 或预装进包），就必须履行它们
自己的 **source offer**：提供对应源码或书面要约。届时：

- iSH：随包附 iSH 对应源码（或指向本仓库构建脚本 + 上游 tag）；
- Termux：manifest 登记各包 `license` / `source_offer`（见
  `runtime-payload/README.md`），App 内提供许可与源码指引；
- 分发方式为用户自签名 IPA（不上 App Store）时，GPL 义务仍然有效。

在这两类组件真正落地前，这条只是预留提醒，不是当前义务。
