# runtime-payload — 按需下载的「环境包」

运行时环境（Android 的 Termux bootstrap + proot、iOS 的 iSH）**不进 App 包**，
用到时才下载。本目录当前只放**清单规范**，下载/校验/安装逻辑尚未实现；未来实现也限定在这一边界内。
二进制与解压产物不入库（见 `.gitignore`）。

## 为什么不在包里

- APK/IPA 体积；用户大多只需要 SSH，本地 shell 是增强；
- 环境包体积大（Termux bootstrap 解压后 100MB+），且要按 arch/版本分别维护。

## manifest 规范（v0）

```jsonc
{
  "id": "android-termux-proot-aarch64",
  "version": "1.0.0",
  "platform": "android",
  "arch": "arm64-v8a",
  "url": "https://…/termux-proot-aarch64-1.0.0.tar.zst",
  "sha256": "<64 hex>",
  "bytes": 12345678,
  "unpacked_bytes": 234567890,
  "format": "tar.zst",
  "entrypoint": "usr/bin/login",
  "mode": "proot",
  "license": "GPL-3.0-or-later (mixed; see LICENSES/)",
  "source_offer": "https://…/sources/"
}
```

字段纪律：

- **`sha256` 必填**，下载后校验失败即丢弃；
- **`mode`**：`proot`（Android Termux）| `native-rootfs`（iSH）；
- **`entrypoint`** 相对解压根，App 不得硬编码绝对路径；
- **`license` / `source_offer`**：GPL 义务的执行体（见 `docs/explanation/licensing.md`）。

## 安装纪律（铁律 #10）

1. 校验 sha256 + 磁盘空间（`unpacked_bytes`）；
2. 解压保留 symlink / 执行位 / 顺序；
3. **原子激活**：解到临时目录 → 校验 → rename 到位；失败不破坏旧版本；
4. 可回滚、可删除；
5. **Android 执行限制**：面向 API 29+ 的普通 App 不能直接执行可写 App home 中的文件；不能把 proot 当成已证明的豁免。需在目标 SDK、系统和设备上验证整个启动链。见[官方执行限制说明](https://developer.android.com/about/versions/10/behavior-changes-10#execute-permission)。

## Android proot 路线（待 PoC，不是已交付能力）

- bootstrap 解到 App 私有目录；
- 用 proot 把 `$PREFIX` 映射到目标路径（规避 Termux 官方 bootstrap 的
  `/data/data/com.termux/files/usr` 硬编码前缀）；
- PoC 必须验证 proot 本体与 bootstrap 中程序的合法启动、`bash` 和该 payload 实际包管理器、PTY 尺寸、目标 SDK/ABI/SELinux 行为；不把 Alpine 的 `apk` 当作 Termux 的包管理器。
- PoC 未通过前不提供“已可安装”入口，不绕过 target SDK 约束宣称可用；普通 SSH 和系统 `/system/bin/sh` 先独立验证。阶段见 [Android 实现计划](../docs/reference/android-implementation.md)。
