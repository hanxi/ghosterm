Ghosterm 开发版：原生终端、SSH 配置管理与 SFTP 文件工作区。

[产品网站](https://hanxi.cc/ghosterm/) · [使用文档](https://github.com/hanxi/ghosterm/tree/main/docs) · [已知问题与能力边界](https://github.com/hanxi/ghosterm/blob/main/docs/reference/capability-matrix.md) · [反馈问题](https://github.com/hanxi/ghosterm/issues)

## 下载

| 平台 | 安装包 | 要求 |
|---|---|---|
| macOS | [DMG](https://github.com/hanxi/ghosterm/releases/download/dev/Ghosterm-macos-arm64-dev.dmg) · [ZIP](https://github.com/hanxi/ghosterm/releases/download/dev/Ghosterm-macos-arm64-dev.zip) | Apple Silicon（arm64）；最低系统见 [version.json](https://github.com/hanxi/ghosterm/releases/download/dev/version.json) 的 `min_macos` |
| Android | [APK](https://github.com/hanxi/ghosterm/releases/download/dev/Ghosterm-android-arm64-dev.apk) | Android 10 及以上，arm64-v8a |

仅提供以上两端开发包。Android APK 是签名的 release 构建；`dev` 表示分发渠道。

## 安装与升级

**macOS**：打开 DMG，将 Ghosterm 拖入“应用程序”；升级前先退出旧版。开发包使用 **ad-hoc 签名，尚未公证**。通过浏览器下载后若系统阻止打开，请按[安装说明](https://github.com/hanxi/ghosterm/blob/main/docs/how-to/dev-build.md)处理。也可使用文档中的安装脚本或 Homebrew。

**Android**：下载 APK，在系统提示中允许对应下载工具安装应用，然后安装或覆盖升级。详见 [Android 发行说明](https://github.com/hanxi/ghosterm/blob/main/docs/how-to/android-release.md)。

## 版本与校验

本页是**滚动开发版**，附件会随成功发布更新。两端独立构建，可能来自不同提交；`dev` tag 和页面标题不能代表两个安装包的源码版本，build 号也不能跨流程比较。

| 平台 | 版本与源码收据 | SHA256 校验清单 |
|---|---|---|
| macOS | [version.json](https://github.com/hanxi/ghosterm/releases/download/dev/version.json) · [BUILD-INFO.txt](https://github.com/hanxi/ghosterm/releases/download/dev/BUILD-INFO.txt) | [SHA256SUMS](https://github.com/hanxi/ghosterm/releases/download/dev/SHA256SUMS) |
| Android | [android-build-info.json](https://github.com/hanxi/ghosterm/releases/download/dev/android-build-info.json) | [android-SHA256SUMS](https://github.com/hanxi/ghosterm/releases/download/dev/android-SHA256SUMS) |

下载时请同时取得对应清单，核对 `git_commit` 和文件摘要。更新日志见[提交记录](https://github.com/hanxi/ghosterm/commits/main/)，请以各端收据中的提交为范围；main 上的新改动不一定已进入下载包。

## 开发版范围

完整设备、输入法、网络与文件服务矩阵仍在完善，已知缺口见能力矩阵。macOS 的“检查更新”仅检测并提供下载链接，不自动安装；Android 更新请重新下载安装 APK。
