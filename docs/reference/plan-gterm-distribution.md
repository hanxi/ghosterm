# gterm 与 GitHub 插件分发设计

## 用户操作

以下命令是拟定接口。首版发行完成后才公开安装入口。

```sh
# 在运行 Executor 的机器上执行，也可以是本机
curl -fsSL https://raw.githubusercontent.com/hanxi/ghosterm/gterm-install-v1/scripts/install-gterm.sh | sh
gterm upgrade

# 在已有 gterm 的机器上查看发布信息并安装 Executor
gterm plugin show owner/repo
gterm plugin install owner/repo@v1.2.3
gterm plugin install owner/repo:preview@v1.2.3
gterm plugin install owner/repo@v1.2.3 --plugin preview --plugin table
gterm plugin install owner/repo@v1.2.3 --all
gterm plugin upgrade owner/repo:preview
```

默认安装到 `~/.local/bin`，支持自定义前缀。安装器检测系统与架构，打印安装路径和验证结果。如果目录不在 PATH，打印对应设置方法。Ghosterm 可在 SSH 插件启动配置中使用绝对路径。

客户端在 Ghosterm 的插件页面输入同一个仓库地址。对于多插件仓库，页面列出插件供单选、多选或全选，并对选中的插件展示发布者、版本、权限与执行位置。若选中的插件需要 Executor，页面为选定的目标生成固定到该发布版本与所选 Executor 的安装命令。用户在目标机器执行，Ghosterm 随后检查实际部署的摘要、协议与能力。界面和 CLI 共用发布格式。

## gterm 二进制发布

- 独立 GitHub Actions 工作流在 PR 构建并验证产物。正式打包由 `gterm-v*` 标签触发，使用验证通过的同一组产物发布到 GitHub Releases。独立标签让 Executor 能与桌面应用分别升级。
- 首批支持 macOS arm64、Linux x86_64 和 arm64。按平台记录最低 macOS 版本或 Linux libc 基线，逐一验证。生产物含 `gterm`、开发辅助工具和许可。Executor 运行无需 Python，开发子命令使用 Python 3。
- 每个资产有 SHA-256。Ed25519 签名的版本索引绑定版本、平台、架构和资产摘要。首次安装通过版本化 HTTPS 地址取得启动脚本，脚本内置发行公钥。信任已经安装的客户端和它的发行根公钥后，升级无需重新下载并运行启动脚本。
- 安装和升级先把资产下载到用户安装前缀的暂存区，验签、对拍摘要、检查平台和 `gterm doctor` 后，原子切换到新版本目录。保留上一个可用版本供回滚。失败时当前版本仍可运行。升级默认跟踪稳定渠道，指定版本可固定安装，回滚须显式选择。
- CI 用实际分发资产在每个平台执行安装、升级和回滚，并验证失败分支，包括错误架构、签名或摘要错误、下载中断和重复安装。工作流在同时发布应用时仍只写自己的资产与索引。

## GitHub 插件源

首版使用 GitHub Releases。作者在插件仓库用现有 Lua 工具生成 ZIP、发布描述和 Ed25519 签名，上传约定名称的资产。Release tag 便于查找，真正安装的版本锁定到签名中的插件 ID、版本、发布者、公钥指纹和 ZIP 摘要。同一 tag 的资产变更时，已锁定版本拒绝静默替换。

`owner/repo` 是来源地址。首次安装展示声明发布者和签名公钥指纹。官方渠道沿用现有信任根，第三方仓库首次需要用户确认发布者指纹，或提供预先配置的渠道策略。更换来源或换钥要经过明确的信任流程。下载后复用现有安装器验证完整性、能力差异和兼容性。已确认的发布者、已安装版本和本次运行授权分别记录。

`gterm plugin install owner/repo` 解析最近的稳定发布并展示具体版本。单插件仓库可省略插件名。多插件仓库必须显式指定一个或多个插件，或者使用 `--all`。交互确认后记录锁定版本，后续显式 `plugin upgrade` 才切换。带 tag 的安装默认保持固定版本，用户可显式选择新版本。升级先展示新增权限，失败时保留原版本。原有基于本地签名资产的安装仍可用于离线环境。安装过程只处理资产和静态清单。

### 一个仓库发布多个插件

GitHub Release 可附一个签名的插件索引。索引绑定仓库发布 tag，并为每个插件记录仓库内短名、完整插件 ID、插件自身版本、Client 和 Executor 角色，以及独立签名资产的名称和摘要。一个仓库 tag 可以包含不同版本的插件，例如仓库 `v1.2.3` 中的 Preview 1.4.0 和 Table 0.8.0。短名只在该仓库内解析，安装身份以完整插件 ID 和已信任的发布者为准。

- `plugin show owner/repo` 列出插件及各自版本和角色。短名不存在或冲突时拒绝安装。
- `owner/repo:preview@v1.2.3` 只安装 Preview。重复 `--plugin` 可选装多项。 `--all` 安装本次发布中具有当前目标角色的所有插件，并在确认前列出实际计划。gterm 安装 Executor，Ghosterm 安装 Client。显式选到角色不匹配的插件时返回错误并指明安装位置。
- 批量安装先展示全部选中项的版本、权限变化和目标角色，再下载并逐包验签。整批通过后，在当前安装端用同一个目录事务切换所选版本。任一项失败时保留此前的活动版本，并报告失败的插件。
- 锁记录按插件保存仓库、tag、插件版本、角色与摘要。`plugin upgrade owner/repo:preview` 只更新 Preview。`plugin upgrade owner/repo` 只更新该仓库已安装且未固定的插件。仓库后来新增的插件需要明确安装。
- 官方插件可按一个 Release 建索引，Preview、Table 等各自保留 ZIP 和签名。单插件仓库可继续发布一个签名 ZIP；多插件仓库通过索引消除资产归属歧义。

## Client 和 Executor

| 插件声明 | 安装位置 | 运行方式 |
| --- | --- | --- |
| 只有 Client | Ghosterm 本地插件库 | 在原生宿主加载 Client |
| 只有 Executor | 本机或选中的 SSH 目标的 gterm 插件库 | gterm 启动 Executor，宿主按协商能力提供通用交互 |
| 同时声明两侧 | Client 装在 Ghosterm，Executor 装在目标 gterm | 一个签名 ZIP、同一发布版本，两侧各自验签和授权 |

前端是 Lua Client 与宿主提供的原生界面能力。后端是 Lua Executor，负责工作区文件与受控进程等操作。两侧入口、依赖能力、命令和协议仍来自同一份 Lua 静态声明。首版复用整个签名 ZIP，每侧只加载自己的入口。仅当有实际体积或平台需求时，再启用签名发布中的 Executor 变体。

双侧插件允许先装 Client。Executor 缺失、版本或摘要不匹配时，界面展示目标和需要的精确版本。升级可先暂存任一侧，在两侧兼容且用户确认后启动新版本。目标由用户选定，SSH 安装、签名信任和运行授权互不继承。远端私有仓库与其他 Git 托管服务留待有实际需求时扩展。

## 实施顺序

1. 打通三个目标平台的独立 gterm 打包、签名、安装、升级和回滚，先将稳定的安装入口交给用户。
2. 为 `gterm plugin` 增加 GitHub 来源解析和锁定记录，沿用现有签名安装及权限审查。
3. 向 Ghosterm 插件页接入同一个来源格式。用 Preview 验证本机和 SSH 两侧安装、升级配对、摘要不匹配和回滚，再用 Preview 与 Table 同一 Release 验证单装、选装、全选、角色筛选、批量失败及单独升级。
