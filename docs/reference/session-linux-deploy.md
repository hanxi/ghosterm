# Linux sessiond 用户态部署

状态：2026-10-21，私有 C 安装组件与 macOS→Linux 实测通过；共用自动重连候选已验证，原生产品入口尚未交付。
当前 GTSD/GTVC 严格匹配，不兼容旧后端，不在这一阶段引入版本协商。

## 路径与依赖

Linux 构建机用 `scripts/package-sessiond-linux.py` 生成对应架构的预编译目录。
macOS 客户端通过已认证、已核验 hostkey 的 SSH 连接，以 SFTP 上传该目录中的全部五项：

- `ghosterm-sessiond`、`ghosterm-session-proxy`；
- `lib/libghostty-vt.so.0`；
- `LICENSE`、`SOURCE-INFO.json`。

仓库为 MIT 许可，不再随包附源码归档；首次安装约 2.4 MiB 内全部为实际运行要用的文件。

远端不编译、不执行包管理器、不用 sudo、不要求 Python，不新增 TCP 监听端口。
SSH 服务须支持无 PTY 的 exec 和 SFTP；用户目录须可执行程序，系统须提供
`uname`、`getconf`、`id`、`/usr/bin/sha256sum`、匹配的 glibc 与 ELF loader。
禁用 exec/SFTP、musl、架构或最低 glibc 不匹配，明确失败；不会安装系统依赖。
目前构建脚本接受 aarch64/x86_64，实测包为 aarch64、glibc 最低 2.34，
macOS arm64 客户端连接 Ubuntu 24.04 aarch64/glibc 2.39。x86_64 尚无本轮部署证据。

`SOURCE-INFO.json` 记录构建提交、引擎 pin 与补丁目录（溯源元数据；仓库整体
MIT，源码本身以 GitHub 仓库为准）。本组件信任调用方选择的本地清单；它不替代
发布渠道的签名/真实性校验。

## 部署事务

`core/src/session/deploy.{h,c}` 与 SSH owner 共用 C 实现，在非 UI 线程独占一条
已认证 SSH 连接。先探测 OS/架构/glibc/UID/HOME，然后逐级 lstat 安装目录；
拒绝符号链接、其他用户拥有的目录和组/其他用户可写目录。目录属性缺失明确报错。

安装布局为调用方指定的绝对私有根目录：

```text
root/
  .stage-<随机标识>/       # 本次独有，目录 0700、上传文件 0600
  releases/<release-id>/  # 校验后原子发布；两个可执行文件为 0700
```

上传不覆盖已存在文件。SHA256 全表校验成功后才允许运行 payload 的 `--version`，
核对 daemon 精确引擎契约及 proxy 部署接口。最后由已校验的 proxy 执行本机
`renameat`，原子移动同文件系统的目录；跨文件系统、已有目标、链接或不安全父目录
均失败。返回不可变版本的绝对路径，没有可变 `current` 链接或 PATH 搜索。

已有版本仍要逐文件检查类型、权限、大小、散列和可执行探测，不能只看目录存在。
部署不重启、替换或迁移正在运行的 daemon；后续 HELLO 严格检查实例与契约。
安装完成后通过同一 SSH 服务的两个无 PTY exec 通道，连接 management/DATA 代理，
由代理按需后台启动共用 sessiond。服务器不需要另一套远程运行时。

安装有一个覆盖探测/上传/校验/发布的单调时钟总期限（1–120 秒），可取消；
非阻塞 IO 分步推进，进度低频上报。取消/网络错误使该专用 SSH 连接失效，调用方
必须关闭后重连；不复用半失败的 SFTP 会话。SFTP 文件句柄在取消后仍须完成释放。
发布发出但确认丢失时返回 `publication_unknown`，不能宣称回滚或重复覆盖目标；
新连接按不可变版本路径重新检查即可确定结果。中断可能遗留本次私有 `.stage-*`
目录，自动垃圾回收尚未实现，不会因此启动新 daemon 或影响已有 release。

## 证据与未完成项

```bash
bash scripts/gate.sh
ctest --test-dir core/build -R '^session_bundle_catalog$' --output-on-failure
# 仅对可丢弃的 Linux SSH 测试环境；私钥为生成的测试材料。
python3 scripts/mutation-check-session-deploy.py \
  --host HOST --port PORT --user USER --key TEST_KEY \
  --bundle LOCAL_BUNDLE --remote-root ABSOLUTE_DISPOSABLE_ROOT
python3 scripts/mutation-check-activation.py
```

macOS/Linux 核心 350 项测试和 57 个公共 ABI 符号对拍通过；关闭 libssh2 时安装接口
显式 UNSUPPORTED。部署的 12 组隔离变异覆盖架构/glibc、散列、目录信任、取消、
引擎契约、句柄释放和本地清单，每组要求普通 FAIL/exit 1、同变异 Linux 探测绿
及源码 SHA256 回滚。启动/代理/发布共 42 组变异、29 条基线通过，其中新增发布的
四组分别保护真实移动、已有目标、父目录链接和父目录权限。

实际 macOS→Linux 流程验证预编译包上传、完整散列、执行探测、原子发布、重复
安装检查和篡改拒绝；随后 macOS 的真实 VT 客户端连接已部署的 Linux 代理，
CREATE/ATTACH、快照/增量、输入屏障、客户端 SIGKILL、全新 TCP/SSH 认证后接回
原 session/原 PID、继续输入及退出后的只读恢复均通过。Linux LeakSanitizer
验证取消上传后的句柄释放。测试环境用 Python 维护夹具，不属于远端产品依赖。

这还不是完整交付：真实网络 reset/静默丢包及共用有界自动重连候选已验证，见
[协调器](session-client.md)。生产认证 provider、公开 session ABI、原生
Transport/Surface/UI 与完整 P0 资源恢复验收继续实施。
参见 [Linux 全流程路线](plan-session-daemon.md)、[SSH owner](session-ssh-link.md)。
