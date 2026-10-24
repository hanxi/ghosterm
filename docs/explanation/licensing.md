# 许可安排

仓库整体 **MIT License**（`LICENSE`）。

## 为什么是 MIT

仓库自己的代码（`core/`、各端宿主、`terminal/`、脚本、文档）依赖的组件都在
宽松许可侧，MIT 分发无冲突、义务最轻（只需保留版权与许可声明）：

| 组件 | 许可 | 用途 |
|---|---|---|
| libghostty / Ghostty | MIT | 终端引擎（Mac/Android/HarmonyOS/Linux/Web） |
| libssh2 | BSD-3-Clause | SSH / SFTP |
| mbedTLS / OpenSSL | Apache-2.0 / Apache-2.0 | SSH 加密后端 |

随包分发这些组件时，把各自许可与版权声明亮给用户（App 内「关于」窗口 +
运行时包内 `LICENSE`）即满足义务；**不需要**随包附源码归档。

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
