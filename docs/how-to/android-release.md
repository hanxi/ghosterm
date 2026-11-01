# Android release APK

当前签名包来自 [run 41288109410](https://github.com/hanxi/ghosterm/actions/runs/41288109410)，
源码 `ab2f008`，APK **6,146,193 字节（约 5.86 MiB）**，含正式图标、五类设置及外部只读打开。
[下载签名 release APK artifact](https://github.com/hanxi/ghosterm/actions/runs/41288109410/artifacts/13076715009)。
SHA256 为 `c157ec5e908078b73292baabb7f6f2c9e8aec72b59f61123cca5bd7fa46fbeda`。
实际提交/clean 收据、固定证书、非 debuggable/minSdk 29/仅 arm64、R8/资源裁剪、三份 native 库压缩及 ELF/ZIP 16 KB 对齐已核对。

同一云端包已在 API 36 arm64 / 16 KB 页模拟器验证同证书覆盖升级、九个旧文件预览保留、外观保存/重启、关于提交及两条连续 ASCII 系统键盘命令。
通过正式页面 ACTION_VIEW 和真实系统选择器，独立 UID 接收应用读回完整文件摘要，写入被拒绝。
功能代码完整 **609 项**仅在 API 36 / 4 KB 页的 debug/测试 APK 通过（1,551.598 秒）；API 29 最终 30 项另验。
文本片段/可靠管道完成边界仍在 §9.93 隔离修复，不属于这个 APK；真实设备、CJK 输入法、完整功能与发行矩阵仍未完成。
证据与失败重试范围见实现计划 §9.92。

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
