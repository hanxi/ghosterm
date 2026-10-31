# Android release APK

工作流为 `.github/workflows/android-release.yml`，监听 main push、PR 和手动触发。
main 与手动构建使用固定 release 密钥签名；PR 只验证优化后的未签名 release 构建。
首次云端运行已通过：[run 41207714115](https://github.com/hanxi/ghosterm/actions/runs/41207714115)，
源码为 `d585f0b`。签名 APK 为 **5,826,065 字节**；下载后 SHA256、固定 release 证书
与 16 KB 对齐检查通过。尚未对该云端 APK 单独完成设备运行验证。

后续持久历史版本也已出包：[run 41215129237](https://github.com/hanxi/ghosterm/actions/runs/41215129237)，
源码 `b0bae1b`，APK 为 **5,876,361 字节**，下载后的 SHA256、固定签名证书与对齐检查通过。
[下载该次 artifact](https://github.com/hanxi/ghosterm/actions/runs/41215129237/artifacts/13047709240)。
该包包含 §9.73 的任务历史，不含尚待完整回归的 §9.74 前台服务；云端包仍未单独完成设备运行验证。

在 GitHub Actions 中打开 **Android Release APK** 的成功运行，下载
`Ghosterm-android-release-<commit>` artifact。解压后包含安装 APK、`SHA256SUMS`
和 `build-info.json`。诊断 artifact 单独保存 R8 映射与 native 符号，均保留 14 天。

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
