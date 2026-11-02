# Android release APK

当前已核验签名包来自 [run 41455186293](https://github.com/hanxi/ghosterm/actions/runs/41455186293)，源码 `6deb04b` / clean。
[直接下载签名 release APK](https://github.com/hanxi/ghosterm/releases/download/dev/Ghosterm-android-arm64-dev.apk)（[共用 dev Release](https://github.com/hanxi/ghosterm/releases/tag/dev)）。
APK **8,127,663 字节**，SHA256 `fb26f4bb585fe8ac644904438fe769363b4700d9020e476b1751be5aeee7cdce`；下载时同时核对 `android-SHA256SUMS` 和 `android-build-info.json`，滚动链接可能已由新提交更新。
固定发行证书、非 debuggable/minSdk 29/仅 arm64、实际 R8/资源优化、native 压缩、完整 Nerd Fonts/许可、ELF/ZIP 16 KiB 对齐及构建机路径检查均已核验。

该实际包在 API 29 / arm64 / 4 KiB 页覆盖升级后保留保存主机，公网域名 `ssh.github.com:443` 已解析并连接到主机密钥确认，再明确取消，未接受信任或认证（§9.150）。实际正式包的终端系统节点亦已提供可见正文并过滤隐藏文字。大字体文件复制与实际包升级读取的范围见 §9.142–§9.143。旧包的 16 KiB 页外部只读打开证据仍只属于对应产物，不回填为新包全量。
最近同源 4 KiB 页完整通过来自 `18db11b`（API 29 / API 36 各 912 项）；`84b984e` 的 16 KiB 页全量为 911/912，不能记绿。`6deb04b` 已提交终端可见文字无障碍和紧凑菜单布局同步，签名 CI 与下载校验通过，默认 915 项回归仍在运行。用户手机 Wi-Fi/移动网络、实际 TalkBack、完整键盘/外部端点/发行矩阵仍未完成。

最终按用户要求与 macOS 共用 `dev` Release，文件名固定为 `Ghosterm-android-arm64-dev.apk`、`Ghosterm-macos-arm64-dev.dmg` 和 `Ghosterm-macos-arm64-dev.zip`。这里 dev 表示分发渠道，Android 仍是固定密钥签名、R8/资源裁剪的 release 构建。
Mac 使用 `SHA256SUMS` / `BUILD-INFO.txt` / `version.json`，Android 使用 `android-SHA256SUMS` / `android-build-info.json`，各自记录版本和实际提交。8 个附件已实际下载逐一比对；临时 android-dev Release 及 tag 已删除。两端发布共用互斥组，Mac 原位更新并只清理旧 Mac 归档，不能再删除整个 Release。

当前证据与分轮范围见[实施计划 §9.150–§9.151](../reference/android-implementation.md#9150-终端无障碍的实际-dev-发行2026-11-02)；旧 `a70a0d0` 的外部打开/SAF 专项保留于 §9.93–§9.94。

前次 `29d3f3d` 的 [CI 41282597767](https://github.com/hanxi/ghosterm/actions/runs/41282597767) 产出 6,129,125 字节签名包；五类设置与正式图标的完整/专项证据保留于 §9.91。

前次 `ca6f104` 来自 [run 41257144722](https://github.com/hanxi/ghosterm/actions/runs/41257144722)，
APK 5,903,897 字节，SHA256 `2ad94cd1764c72a4e7c2bb0536a6573e1f7f4a449a6c8d88c7666180abea39bf`。
其完整 498 项及 16 KB 页复制专项保留于[§9.79](../reference/android-implementation.md#979-a2-输入组合接受与-api-29-兼容复核2026-11-01)，不借给新包。

工作流为 `.github/workflows/android-release.yml`，监听 main push、PR 和手动触发。
main 与手动构建使用固定 release 密钥签名；PR 只验证优化后的未签名 release 构建。

前次文件进度包 `ab7c53f` 来自
[run 41241009189](https://github.com/hanxi/ghosterm/actions/runs/41241009189)，APK 5,903,009 字节，
SHA256 `95f9ff833beedca262f98557ba76c92379881a9f15da74b23157b4f555fdbfb1`。
该包的 491 项完整测试与 16 KB 页专项保留原范围，见
[§9.78](../reference/android-implementation.md#978-a10-签名-release-与-16-kb-页实际运行2026-11-01)。

历史通知修复包 `6bed87f`（含 `50e4829`）来自
[run 41230615030](https://github.com/hanxi/ghosterm/actions/runs/41230615030)，APK 5,889,501 字节，
SHA256 `753822e77c626a3852a64d4bac7d587c40a52ba7b700d7df8940316f67c20496`。
该包已在独立 API 36 / 4 KB 页模拟器验证升级、通知恢复/跳转、28 字节复制、取消和服务退出，
范围见[§9.75](../reference/android-implementation.md#975-通知恢复后补发当前任务2026-11-01)；
这些专项不借给新包。此前 `5441c5d` 的包曾暴露通知恢复不补发，已由历史修复包解决。

首次云端运行已通过：[run 41207714115](https://github.com/hanxi/ghosterm/actions/runs/41207714115)，
源码为 `d585f0b`。签名 APK 为 **5,826,065 字节**；下载后 SHA256、固定 release 证书
与 16 KB 对齐检查通过。尚未对该云端 APK 单独完成设备运行验证。

后续持久历史版本也已出包：[run 41215129237](https://github.com/hanxi/ghosterm/actions/runs/41215129237)，
源码 `b0bae1b`，APK 为 **5,876,361 字节**，下载后的 SHA256、固定签名证书与对齐检查通过。
[下载该次 artifact](https://github.com/hanxi/ghosterm/actions/runs/41215129237/artifacts/13047709240)。
该历史包包含 §9.73 的任务历史，不含 §9.74 前台服务；随后已在 API 36 模拟器实测本地终端创建文件及只读预览，不代表完整发行矩阵通过。

在 GitHub Actions 中打开 **Android Release APK** 的成功运行，下载
`Ghosterm-android-release-<commit>` artifact。解压后包含安装 APK、`SHA256SUMS`
和 `build-info.json`。诊断 artifact 单独保存 R8 映射与 native 符号，均保留 14 天。

工作流在恢复签名材料之前运行 `bash scripts/check-android-api.sh`，分别检查
app、bridge、data、terminal 的 release lint；app 单独 lint 不覆盖依赖模块源码。
新增入口在本地已通过 API 30 调用的编译后变异验证，并在 ca6f104 的上述成功 CI 中实际执行通过。

当前仅 arm64-v8a，声明最低 API 29；设备验证范围仍见
[Android 实现计划](../reference/android-implementation.md)。release 构建不代表完整功能或发行矩阵已验收。

## 包体

release 启用 R8 优化、优化资源裁剪与 native 符号剥离。
native 库在 APK 中压缩存储，Android 安装时解压；运行依赖与许可保留。
不要用整个 bridge 包的 blanket keep 规则绕过 R8，JNI 方法的名字必须保留。
安装验证必须使用压缩后的构建。

工作区首轮本机构建依次为：R8 后 20,429,961 字节，固定应用 NDK 并剥离符号后
9,729,929 字节，再压缩 native 库后 5,876,425 字节。对应 debug 包为
54,258,326 字节。这些是本机工作区数据，实际 CI 包大小以其 `build-info.json` 为准。

## 签名

仓库使用以下 Actions Secrets，并由工作流注入环境变量：

- `ANDROID_RELEASE_KEYSTORE_BASE64`
- `ANDROID_RELEASE_STORE_PASSWORD`
- `ANDROID_RELEASE_KEY_PASSWORD`
- `ANDROID_RELEASE_KEY_ALIAS`

密钥在受限的工程 `tmp/` 文件中解码，结束时删除；不会进入 artifact。
本机备份在 `~/.local/share/ghosterm/android-signing/`，子目录含 keystore、凭据和公开证书信息。
后续构建继续使用同一密钥，保证 APK 可以覆盖升级。

本地签名构建使用 `ANDROID_RELEASE_STORE_FILE` 指向 keystore，并提供上述密码和 alias
环境变量，执行：

```bash
bash scripts/build-android.sh :app:assembleRelease
```

产物为 `android/app/build/outputs/apk/release/app-release.apk`。
未提供签名文件时只能得到未签名 release APK，不作为可安装包交付。


## CI Gradle 缓存目录

CI 在 setup-java 之前将 GRADLE_USER_HOME 设置为 runner 用户目录下的 `.gradle`，与锁定的 setup-java v6.1.0 默认缓存位置一致；依赖缓存和 wrapper 缓存才会覆盖实际构建目录。此前 job 将 Gradle home 指到工程 `build/android-toolchain/gradle-home`，setup-java 却检查用户目录，导致发布虽然成功但缓存未保存。本地脚本仍默认使用工程内工具链目录，也继续尊重调用方显式设置的 GRADLE_USER_HOME。

修正依据是[锁定版本的缓存实现](https://github.com/actions/setup-java/blob/b843ebf9b913146b38da2eaa7aeddf76074ae1ad/src/cache.ts)。workflow 语法检查已通过；首次实际保存和后续命中仍须读取对应 CI 日志确认，不能仅凭配置或一次总耗时宣称加速比例。
