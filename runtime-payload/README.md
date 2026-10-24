# runtime-payload — 按需下载的「环境包」

运行时环境（Android 的 Termux bootstrap + proot、iOS 的 iSH）**不进 App 包**，
用到时才下载。本目录只放**清单规范与下载/校验逻辑**；二进制与解压产物不入库
（见 `.gitignore`）。

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
5. **Android 额外注意**：app 数据目录内**不能直接 exec**（Android 10+ W^X）。
   proot 方案下由 proot 负责挂载与执行，**proot 自身**仍须过这一关。

## Android proot 路线（已定）

- bootstrap 解到 App 私有目录；
- 用 proot 把 `$PREFIX` 映射到目标路径（规避 Termux 官方 bootstrap 的
  `/data/data/com.termux/files/usr` 硬编码前缀）；
- 以实现细节为准的 PoC 必须先验证：**proot 能起 + `apk`/`bash` 能跑 +
  PTY 尺寸正确**，再谈 UI 接入。
