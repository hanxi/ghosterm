# macOS 私钥材料与 Keychain

2026-11-17，本片落实 AGENTS.md 铁律 #5；没有修改密钥规则。实现为 `NativePrivateKeyStore` → `KeychainStore`，由 `SSHCredentialSelector` 消费。配置中保留 IdentityFile 引用，私钥不写入 App 的文件、日志或快照。

## 导入与读取

第一次使用尚无记录的本机身份文件时，读取当前用户持有的私有常规文件：上限 1 MiB、不跟随最终符号链接、CLOEXEC、完整读取并核对 fd/路径身份、权限和纳秒时间，拒绝 NUL、非法 UTF-8 与无法分类的格式。这是导入输入，未声称简单格式分类等于完整密码学验证。先写 Keychain，再从 Keychain 读回交给认证；写入/解锁/读取失败不能用文件字节替代，也不静默切换到密码。

首次导入用排他 SecItemAdd，并发赢家为准。之后 Keychain 是材料真相：删除或改动来源文件不会隐式更换已存材料。主机表单的“从文件更新钥匙串私钥…”可显式重新导入当前逐行身份引用（菜单最多前 64 条）；该动作独立于配置保存。更新使用 SecItemUpdate，首次创建才 Add，竞争时重试 Update，拒绝前不先删原记录；回读不符明确报告已提交但需核对，不能盲目回滚。来源文件不由 Ghosterm 删除或改写，仍由用户管理。删除 Host/IdentityFile 不删除可能被其它主机使用的已存材料；可在 macOS“钥匙串访问”按 Ghosterm 私钥条目及 `key:<路径>` 账户定位并删除。

导入是独立的安全存储动作，取消登录或表单不删除已完成导入。请求取消信号贯穿生产材料读取，在安全存储/文件阶段间及返回前检查，旧结果不能提交给 Core；已开始的 Security 调用可能晚到或超时，错误明确标为结果未确认。没有宣称系统调用可撤销、Swift 的全部内存副本已擦除或异常进程死亡会自动清理用户的 Keychain 记录。

没口令的私钥不新增应用口令框；需要口令时仍使用独立解锁问法，登录密码按服务器允许方式询问。密码/口令缓存仍仅在认证接受后保存。系统 Keychain 自身的授权/锁定规则保持，访问失败显式报告。

`ghosterm-identity:<UUID>` 属于 Android 的加密容器，不能当作 macOS 路径；本端明确返回 unsupported 并要求重新导入关联。相对身份路径也明确拒绝，应使用绝对路径或已展开的 `~`，不依赖 App 启动目录。不会从复制的配置推定跨端材料已迁移。

## 验证

隔离当前已提交基线 `02fadef6`（包含插件被动发现提交），构建/临时输入在工程 `tmp/rem19`；未修改插件产品或 Core ABI。

- Keychain、材料存储、凭据与真实表单控件组合 46 项通过；最终配置/表单/别名/材料基线 105 项通过。真实 Keychain 使用一次性账户或一次性路径，清理后核对 notFound。比较摘要/布尔结果，不输出真实材料。
- 自有本机 sshd 的 18 项普通入口、主机信任、认证、SSH 网格与 SFTP 组合通过；原生 required gate 核对三必跑收据，既有 WebKit 子资源夹具通过。旧 `testConnectDoesNotStashUnlockedKeyInKeychain` 在首轮造成唯一失败，其“不保存材料”规则与当前铁律/待办冲突，未记整轮绿。替换为首次实际认证→Keychain 材料核对→移除一次性来源→再次实际认证；原有无口令不询问口令断言保留，不靠删除失败断言换绿。
- `mutation-check-macos-config.py --phase private-store|private-credential|private-binding|private-utf8|private-ui` 共十四个不同保护分轮通过（8＋2＋1＋1＋2）：真实 Swift 编译后单条正常断言红、独立 HostList 读取绿、复原重编绿与 SHA256。来源组初次只移除路径检查仍被 fd ctime 检查拦住，未计有效红；随后移除整组来源一致性保护得到正常红。初轮测试逻辑与 `&&` 的 throwing 编译错误单列，不计通过。
- arm64 Debug/测试及 Release 原生构建通过。不增加 CI 二十份，不据定向结果宣称完整宿主、全部系统授权/锁定矩阵或已发行。

原件在 `tmp/rem19/tmp`：`keychain-tests6.log`、`keychain-ssh-native2.log`、`keychain-release.log`。变异收据为 `config-mutations-8t48icdd`（首四组有效记录，之后未杀死的单检查单列）、`config-mutations-7fshvbjh`（后四组）、`config-mutations-az2re18k`、`config-mutations-bnb_ei5f`、`config-mutations-ispn400l`、`config-mutations-3lbmujlo` 中的 `receipt.json`；同一保护不重复累加。

闸门只对自己生成的 `GHOSTERM_TEST_KEYCHAIN_OWNED_REFERENCE` 做测试后清理：必须同时等于 SSH 夹具引用、位于当前工程 tmp 的 `apple-app-*` 目录且名为 clientkey。不得据任意 GHOSTERM_SSH_KEY 删除用户记录，也不扫描其他账户；普通材料用例各自清理独立账户。
