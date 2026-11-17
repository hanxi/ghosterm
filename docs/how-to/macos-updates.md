# macOS 应用内更新

开发版和正式版都可以在没有 Apple 开发者证书的情况下更新。应用本身仍按原有方式做
ad-hoc 代码签名；更新 ZIP 和 appcast 使用另外一对 Ed25519 密钥签名。两类签名独立，
Ed25519 不提供 Apple 身份或公证，不改变首次安装时的 Gatekeeper 规则。

## 用户操作

在 `Ghosterm → 检查更新…` 或关于窗口点击「检查更新」。只有主动检查才联网，没有
启动检查、后台轮询、自动下载或强制重启。右下角显示检查结果，选择「下载更新」后显示
下载进度。关闭提示继续下载；通过菜单或关于窗口可以再次查看。

下载经 Sparkle 校验、解包后显示「重启并更新」和「稍后」。重启保留原有的普通终端、
文件传输和插件退出确认，取消退出后可以再次选择重启。普通终端进程不跨重启恢复；
显式创建的远端 tmux 后台会话按原有规则保留。

- **dev**：固定 `releases/download/dev/version.json`，比较随包 commit。相同短 SHA 前缀
  表示当前版，不同表示远端 dev 已变化；不判断 Git 祖先关系，不比较 workflow 运行号。
- **release**：`releases/latest/download/version.json`，跟随 GitHub 最新正式发布，比较三段
  数字正式版本号。相同或更旧不提示升级，不切换到 dev/prerelease。
- 缺来源、dirty 构建、渠道不匹配、网络或清单错误都显示「查不出来」。缺更新签名或本机
  公钥不能自动安装，保留手动下载入口。只读挂载、应用转移或权限问题由更新错误明确反馈。

第一次接入需要手动安装带更新公钥的新 Ghosterm；旧的只检测版本没有更新引擎，不能
靠远端发布给自己添加下载和安装功能。

## 配置发布签名

先完成一次 `swift package --package-path apple/GhostermApp resolve`。依赖精确锁定为
Sparkle 2.10.1，上游二进制有 SwiftPM SHA256 校验；没有修改上游源码。

```bash
python3 scripts/setup-macos-update-signing.py --configure-github
```

脚本在登录钥匙串创建或复用 `ghosterm-updates-v1`，可能出现系统钥匙串授权提示。它将公钥写入
仓库 Actions variable `MACOS_UPDATE_PUBLIC_KEY`，私钥写入 Actions secret
`MACOS_UPDATE_PRIVATE_KEY`，不落盘、不输出私钥、不触发发行。备份钥匙串中的更新私钥，
不要每次发布换一对；丢失或轮换须按 Sparkle 的密钥迁移要求单独处理。

本地打包可通过 `GHOSTERM_UPDATE_PUBLIC_KEY` 注入同一公钥。两种 Apple 签名渠道都不
需要 Apple 证书。CI 若两项更新密钥都缺失，生成空 appcast 和明确的手动下载清单；只缺
一项或公私钥不匹配直接失败，不发布一个声称能更新却无法验签的包。

## 发布契约

共用 `macos-package.yml` 从实际 ZIP 内的 `Info.plist` / `BUILD-INFO.txt` 以及
`SHA256SUMS` 生成 `version.json` 和 `appcast.xml`。dev 原位发布到 dev prerelease；正式
版附件进入对应 `vX.Y.Z` 草稿，发布草稿后才被 latest 入口看到。发布 ZIP、签名 appcast、
version.json 必须来自同一轮；滚动 dev 在检查和下载之间变化时要求重新检查，不默装另一版。

`signature` / `notarized` 如实记录 Apple 代码签名与公证状态，不能据此判断 Ed25519 更新
能力。`update_signature` 为 ZIP 的 Ed25519 签名；`update_version` 与 appcast、ZIP 内
`CFBundleVersion` 相同。后者为打包时刻的 Unix 秒数，为 Sparkle 安装防降级提供同源递增
序号；workflow `build` 继续留在 BUILD-INFO 中用于追溯，不作为跨流程版本比较依据。

应用固定公钥并要求签名 appcast，Sparkle 验证下载 ZIP 的签名后才解包和安装。清单经 HTTPS
获取用于显示与渠道比较；实际可执行内容的信任以本机公钥和签名 appcast/ZIP 为准。
SHA256 和更新签名不是同一种来源证明。

打包完整复制 Sparkle.framework 及其辅助程序和 MIT 许可，保留符号链接与可执行权限。
ad-hoc 宿主保留上游框架签名；证书签名宿主按嵌套顺序重签框架、XPC 与更新程序。

## 本地验证

```bash
bash scripts/mutation-check-app-update.sh
python3 scripts/test-macos-update.py
python3 scripts/test-macos-update-install.py
```

最后一条只创建工程 tmp 下的一次性 ad-hoc 测试应用，访问本机 loopback HTTP 夹具，
不更新用户已安装的 Ghosterm。覆盖实际下载安装重启、「稍后」保持运行、退出取消后
重试、篡改 feed 和 ZIP 的拒绝。临时测试密钥只在进程内和 stdin 管道使用，不写入文件。Sparkle 的唯一测试缓存入口链接到
工程 tmp，结束后清理本轮入口及偏好，不触碰其它应用缓存。
生产服务仅使用固定 GitHub HTTPS URL；测试 HTTP 例外仅存在于一次性夹具 plist。

上游行为与签名要求见 [Sparkle 官方说明](https://sparkle-project.org/documentation/)，
自定义进度回调见 [SPUUserDriver](https://sparkle-project.org/documentation/api-reference/Protocols/SPUUserDriver.html)。

## 2026-11-17 本地证据

- 原工作区 Core 589/589、185 ABI，clang-format 19.1.7 的 272 文件检查通过；没有修改 C 核心。
- 更新 / 菜单 / 关于 / 窗口 34 项原生 XCTest；十组隔离编译变异，正常断言红、独立配置绿、复原绿及源码 SHA256。
- 实际 Sparkle 签名七项，包含 ad-hoc、正式渠道、空配置、缺单项、错误公钥、摘要变化与 dirty/渠道拒绝。
- 一次性 ad-hoc 应用实际下载 / 替换 / 重启、「稍后」保持运行、退出取消后重试、篡改 appcast / ZIP 拒绝五场景通过。
- 本地 arm64 Release 打包 / 加载 / 原生 shell 冒烟；原打包 63 项与 DMG 三组变异通过。
- 初轮旧签名判据测试失败、变异运行器过严限制单条失败计数、重启夹具未持久化回执路径，以及「稍后」夹具主动退出导致自然退出安装，均作为前置测试问题保留，不计最终通过。
- 已在 GitHub 配置更新公钥变量与私钥 secret；本机钥匙串和仓库公钥一致，实际签名独立验签通过。最初外部钥匙串读取等待已结束，最终部署密钥由独立 helper 创建并经继承管道写入 Actions。

以上为本地及配置证据；实际 GitHub 发行、旧用户应用迁移、其它 macOS 版本与权限/只读挂载矩阵单独核验。
