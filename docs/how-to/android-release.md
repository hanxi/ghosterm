# Android release APK

当前签名包来自 [run 41269551404](https://github.com/hanxi/ghosterm/actions/runs/41269551404)，
源码 `272d140`，APK **5,906,513 字节**。
[下载签名 release APK artifact](https://github.com/hanxi/ghosterm/actions/runs/41269551404/artifacts/13068961740)。
SHA256 为 `e6a44773eca90fa15b7f2affcb6e47d8d346a74af4fdbb9e8bf575535c0e3076`；
下载文件的提交/清单摘要、固定证书、非 debuggable/minSdk 29/仅 arm64、ZIP/ELF 16 KB 对齐及三份 native 库压缩均已核对。

同一云端包已在 API 36 arm64 / **16 KB 页**模拟器完成同证书覆盖升级，
原 28 字节来源及两个既有文件的真实页面预览保留；系统英文键盘显示时连续两条 ASCII 命令，
各自文件名和内容均通过页面读回验证，安装 APK 摘要与下载文件一致。
此专项由 Android 系统输入事件驱动，不代表组合输入法或完整 release 功能验收。
源码完整 **523 项**仅在 API 36 / 4 KB 页的 debug/测试 APK 上通过。
API 29 的三项当前输入专项另行通过，不包含 raw 字节夹具；真实设备与完整发行矩阵仍未完成。
该源码提交已含设置存储基础，生产页面尚未接入；未使用的实现可被 R8 裁剪。
五类新设置页仍在隔离工作树，不在此 APK，也不能把存储专项当作发行设置功能。
详见实现计划 §9.81–§9.83。

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
