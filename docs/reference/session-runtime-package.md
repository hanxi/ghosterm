# 会话运行时打包与校验

macOS 应用附带本地 sessiond/proxy、独立 libghostty-vt，以及 Linux aarch64 与 x86_64
预编译包。客户端不把 macOS 二进制发到 Linux：探测系统、架构、loader/glibc 后选择
相应 Linux 包，经现有 SSH/SFTP 发布。当前两份 Linux 包的最低 glibc 均为 2.34；
缺架构、契约不符或依赖不符明确失败，未提供 musl 包。

`build-sessiond-mac.py` 以 macOS 13 deployment target 构建无 SSH 客户端依赖的 helper，
检查 SDK 与引擎头文件一致，限制 Mach-O 依赖为系统库和包内 VT dylib；先签名，再计算
SHA256。构建目录按引擎路径隔离，避免不同引擎的链接缓存混用。每个 immutable release
包含 LICENSE 与 SOURCE-INFO.json（构建提交、引擎 pin、补丁目录等溯源元数据）；
仓库整体 MIT，不随包附源码归档。

`stage-session-runtime.py` 校验本地 runtime.json 和 Linux GTB1 清单的固定文件集合、
SHA256、尺寸、契约、架构与路径；同名 release 已存在但内容不同会失败，不覆盖。
应用数据位于 `Contents/Resources/sessiond`，不把清单伪装成 Helpers 代码；
Mac helper 保持可执行，Linux ELF 在 Mac 侧作为数据读取，安装后由远端设置执行权限。

根 catalog.json 格式为 1，包含严格 contract、本地 release 和 Linux 架构→release 索引。
`SessionRuntimeCatalog` 在后台限量读取清单、流式校验文件 SHA256，拒绝文件 symlink、
其他用户所有权或组/全局可写文件；未知格式/架构/契约明确不支持。恢复前还须将记录的
contract 与当前受信 catalog 比较，不把旧记录伪装为新协议；没有兼容旧后端的适配层。

```bash
python3 scripts/build-sessiond-mac.py
python3 scripts/stage-session-runtime.py --help
# 可重复传入两种架构；使用支持数组的 Bash 4+。
bash scripts/package-mac.sh --help
python3 scripts/probe-session-runtime-stage.py
python3 scripts/mutation-check-session-runtime-stage.py
python3 scripts/mutation-check-session-runtime-catalog.py
python3 scripts/mutation-check-session-runtime-builder.py
python3 scripts/probe-session-runtime-package.py --app tmp/session-native-package/Ghosterm.app
```

已产出并深度验证 ad-hoc 签名应用，复制其中本地 helper 到新目录后真实 PTY/Surface 恢复
通过；helper 不依赖工程目录或 Homebrew。应用主程序自身依赖的 Homebrew 库目前将实际
最低系统提升至 macOS 26，打包器如实计算，helper 的 deployment target 不代表整个应用。
发布证书、公证与正式分发未验。清单与发布层有七组变异，Swift catalog 八组，真实 Mach-O
依赖一组；均使用普通断言失败与同变异绿色对照，marker 预检及中断后的 SHA256 回滚。

默认产品入口已通过独立的安装/恢复/停止与 GUI 强杀验收，不能仅由打包结果推断；[设置有序更新](session-ordered-style.md) 已接线。后续任何协议更改必须重新构建三种 payload，统一 contract；历史记录明确报
不支持，保留后续升级/兼容设计讨论。

## 默认入口与进程恢复验证

`DefaultRemoteSessionTests` 使用默认 AppModel 连接与启动恢复路径；Linux aarch64
为原生 VM，x86_64 为 QEMU 用户态容器。aarch64 默认入口还经真实 TCP reset
与静默黑洞恢复。实际 GUI 进程验证通过以下探针运行，配置仍由专用夹具注入：

```bash
python3 scripts/probe-default-session-gui.py --executable /absolute/Ghosterm.app/Contents/MacOS/GhostermApp
# 本地路径不需要 Linux 夹具环境。
python3 scripts/probe-default-session-gui.py --local --executable /absolute/Ghosterm.app/Contents/MacOS/GhostermApp
```

本地 daemon 与 Linux aarch64 远端均已通过。探针启动真实 RootView，收到带随机
标记的新命令响应后 SIGKILL GUI，再从同一
记录启动恢复。第二个 GUI 须再次获得新响应，且 instance/session/PID/目录一致，
最后显式关闭并观察结束。`.app` 可执行文件名以包内实际名称为准。测试清理仅
作用于指定的 `ghosterm-test-*` 私有远端目录和经 UID/PID/executable 核验的宿主。
此探针不证明完整 P0 资源或 GPU 预算。

2026-10-21 源码 `54f221585da7` 的 ad-hoc 签名包已复验：包内实际二进制的本地
和 Linux aarch64 GUI SIGKILL/重启原 PID、移动目录后的 helper 原生恢复、
许可声明与深度签名均通过。此测试包为 macOS arm64，最低 macOS 26.0
（当前内嵌 OpenSSL 依赖的最低系统），未公证。校验和及 PID/instance/session
验收记录随测试包提供；后续文档修订不改变该源码版本的二进制。
