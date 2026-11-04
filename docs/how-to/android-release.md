# Android release APK

[下载 dev APK](https://github.com/hanxi/ghosterm/releases/download/dev/Ghosterm-android-arm64-dev.apk) · [共用 Release](https://github.com/hanxi/ghosterm/releases/tag/dev)

Android 包为固定发行密钥签名、启用 R8/资源裁剪的 release 构建，支持 arm64-v8a，最低 Android 10（API 29）。`dev` 表示滚动渠道，不表示 debug 包。

## 核对下载版本

同时下载 `android-build-info.json` 和 `android-SHA256SUMS`，先核对 `git_commit`、构建状态、APK 名称和 SHA256，再安装。两端独立发布，不从共享 tag、页面标题或 Mac build 号推断 Android 版本；清单也可能在两次下载之间滚动，遇到不一致应重新取得同一轮附件。

```bash
shasum -a 256 -c android-SHA256SUMS
```

[当前状态](../reference/current-status.md)记录带日期的发布快照和最近完整测试范围。历史 `ae164f1` 的发行与两端 942 项见实施计划 §9.190–§9.191；后续设备存储、触屏/连发、连接复用及专注布局见 §9.200–§9.204。不能把历史完整套件借给后续 APK。完整手机/输入法/网络/提供方矩阵仍未完成。

## CI 与附件

工作流为 `.github/workflows/android-release.yml`，监听未被路径过滤的 main push、PR 和手动触发；纯 Markdown 或 `docs/` 变更不触发。非 PR 构建使用固定 release 密钥签名；PR 验证优化后的未签名 release。只有非 PR 且 main 的发布 job 上传到共用 dev Release，手动选择其他分支不会发布。

共用 Release 中，Android 仅更新 `Ghosterm-android-arm64-dev.apk`、`android-SHA256SUMS` 和 `android-build-info.json`；Mac 使用自己的五份附件。两端发布 job 共用 `dev-release` 互斥组，原位更新 Release，不能删除整个 Release 或另一平台附件。正文共用 [.github/release-notes/dev.md](../../.github/release-notes/dev.md)。

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

修正依据是[锁定版本的缓存实现](https://github.com/actions/setup-java/blob/b843ebf9b913146b38da2eaa7aeddf76074ae1ad/src/cache.ts)。workflow 语法检查已通过；`f5eda91` 的成功 CI 日志已确认 Gradle 与 wrapper 缓存恢复及 primary key 命中；没有据此宣称固定加速比例，先前首次保存仍以对应旧日志为准。


## 手机复测与结果记录

在原安装上覆盖升级，关于页记录实际 commit、手机型号和 Android 版本；安装来源的“dev”字样不代表 debug 包。不要用模拟器结果填写真机记录。

1. 在“编辑主机 → SSH 身份”关联已导入密钥并保存，再连接主机。无口令配置密钥应自动使用；加密身份仍需口令。未配置时选择只作用当次，不应静默修改 ssh_config。
2. 在 SSH 终端点“文件”，顶部应显示该远端主机，显示目录或具体错误；打开短文本查看真实内容，返回后终端应仍连接。首页“本机文件”应进入应用本机目录。
3. 用自己的测试目录验证长按选择、复制清单、目标处粘贴和冲突“保留两者”；比对实际目标内容。分别记录本机→远端、远端→本机和系统文件选择器的结果，未测方向留空。
4. 测试中文输入、组合取消/删除、唯一键盘开关、横屏和大字体。外接键盘和 TalkBack 有实际设备/服务时分别记录；模拟按键回调不能记成物理键盘通过。
5. 将 Wi-Fi、移动网络及实际使用的 VPN/私人 DNS 分开记录。域名连接失败时附页面错误文字和所在步骤，不把一次成功扩大成完整网络矩阵通过。

反馈只需型号、系统版本、commit、操作步骤和实际结果。不要在反馈中提供私钥、口令或包含凭据的截图。
