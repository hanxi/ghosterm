# macOS SSH 配置写盘事务

2026-11-17 独立修复 `ConfigStore`。本片不修改 `AppModel`、主机编辑表单、Core ABI 或插件；`AppModel.applyEdits/addHost` 的先改内存后保存仍是下一独立切片，不能因存储层完成而关闭整个 M3。

## 写盘契约

- 初始化保留完整原始字节、文件身份和读取错误；只有真实 ENOENT 是不存在。配置链接、目录等非普通文件明确拒绝，不跟随或静默替换。
- 保存打开父目录 fd，在稳定的 `.config.lock` 上取得非阻塞独占锁，忙时明确报错。锁文件只接受当前用户的私有、单硬链接常规文件，使用后不删除；所有 ConfigStore 写者共用该锁。
- 写入前及最终提交前复核字节/文件身份、父目录和锁身份；同长度/还原 mtime 的变化也不能绕过。源码默认文件名为 `config`；自定义文件名使用对应的锁与暂存前缀。
- 已有目标先将原始字节写入 0600 独占暂存，完整写入、同步、检查关闭后原子替换 `.bak` 并同步目录。备份失败阻断目标提交；已有备份不先删除，备份路径若是目录不会递归删除。
- 新内容走同目录独占暂存，处理短写/EINTR，保留已有文件的 POSIX owner/group/权限；首次创建为 0600。同步和关闭成功后以一次 rename 替换，过程中不删除原目标。首次创建使用排他 rename，拒绝最后检查后出现的新目标。
- 提交前失败保留当时的目标，清理本次暂存；备份已成功时允许保留该备份。rename 成功后的同步或回读错误返回 `ConfigStoreError.committed`，明确目标已被替换，并阻断当前 store 的盲目重试，须重新加载。成功回读才刷新保存快照。

现有非 throwing 初始化保持调用方式，加载失败会在保存时明确报错。`isExternallyModified()` 遇到读取失败保守返回 true，不把错误当不存在。

## 验证入口

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer TMPDIR="$PWD/tmp" \
  swift test --package-path apple/GhostermApp \
  --filter 'ConfigStoreTests|ConfigEditTests|HostListTests'

# 先准备工程 tmp 下的独立 git worktree、其自身的 Core/Swift 构建目录，
# 以及相同 pin 的 libssh2、GhosttyKit 和 Sparkle；禁止指向主检出或共享构建目录。
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  python3 scripts/mutation-check-macos-config.py --checkout tmp/config-store-check
```

变异仅改指定隔离 checkout 的 `ConfigStore.swift`，目标和独立配置读取对照都是真实编译后的 XCTest。每组目标单条正常断言红、对照绿、恢复绿并核对 SHA256；编译失败、崩溃、sanitizer 和超时不计正常红。支持重复 `--case 名称`，具体组名见脚本。此手工脚本不增加 macOS CI 二十份覆盖。

## 证据与剩余范围

基线为隔离 `2b073076` 加本片 ConfigStore/测试，临时内容在工程 `tmp/cfg17/`；没有取入同期插件工作区。主/测试输入与复原摘要、逐组日志由变异脚本生成，最终结果如下。

原实现真实编译后的四条黑盒断言分别在备份目录、同长度且固定 mtime 改动、非普通目标和符号链接目标变红；同一构建的主机配置读取对照绿。mtime 首版夹具使用动态 Date 往返产生精度差，旧实现未变红；改为写入/恢复同一个整数秒时间后才计有效红。隔离 Core 首轮两项因缺 SSH 夹具工具链失败，补齐同 pin 工具链引用后 589 CTest / 185 ABI 全绿。Sparkle 下载等待中止后，改用本机已锁定同版 artifact 的私有副本；没有放宽依赖 pin 或校验。

本片只验证一次性本地配置文件与存储故障，不执行用户 SSH 数据、远端服务、插件业务或整窗 UI。原子替换由实际文件系统执行，失败注入在系统调用接缝上，不据此声称真实断电/磁盘硬件故障已验。

稳定锁协调使用 ConfigStore 的写者；任意不遵守该锁的外部编辑器仍可能在最后检查与 rename 之间写入，不承诺对所有非协作写者提供 CAS。ACL/xattr、任意网络文件系统和断电恢复不扩展为本片证据。主机表单脏字段/归属、候选模型发布、Ghostty 设置和 known_hosts 是各自独立实现。

收尾：最终 32 项 ConfigStore、6 项 ConfigEdit 与 5 项 HostList，共 **43 项通过，零失败/跳过**；最终同源 **22 组**完整变异均实际编译后单条正常断言红、独立主机配置读取绿、复原绿和 SHA256。此前 19 组无锁扫描为前置历史，不与最终 22 组累加。macOS arm64 Release 原生构建通过，clang-format 19.1.7 对隔离基线的 276 文件只读检查通过；首轮缺格式工具未执行，安装同版本到本轮 tmp 后才记通过。没有修改 C 或扩大为完整宿主/发行验收。

最终生产源 SHA256：`989d4b24ede190ebdf4e4a23cc4de1a0a1d9d9918c1246e0195dd9bad125dab2`；测试源 SHA256：`9bcbd9499dc2bd4de03693de071203f15086a924433f58a7f0b318fd84b6440c`。

工具与清理收尾：首次补装格式工具遗漏安装器 TMPDIR，未将其计为完整隔离证据；随后以工程 TMPDIR、工程缓存目录及 pip `--no-cache-dir` 重建锁定 19.1.7 工具，276 文件检查再次通过。`tmp/cfg17` 与 `tmp/set17` 两个本轮隔离 worktree 已移除；仅清理本轮 25412 普通文件 / 5794 目录 / 95 链接 / 2217035124 逻辑字节，不代表 APFS 实际回收量。主生产/测试/文案/变异脚本摘要前后不变，其它 session 工作区和 tmp 内容保留。临时日志/截图已随隔离目录清理，以上为入库摘要，不再将旧路径当现存附件。

后续候选发布批：AppModel 已改为重新解析实际来源、比对编辑快照，在独立候选上应用稀疏编辑，落盘成功才发布 AST/主机/规则列表；普通成功保存不再调用会取消认证的全局 reload。提交后同步/回读错误会重载磁盘观察并继续报错。最终 50 项原生组合、六组发布保护变异与 arm64 Release 通过；源 SHA256 `9812bbdd52ae2e4ea938e73d12eb6749452d648807c18f0ebf2dea2b0665285c`，测试 SHA256 `e26e0eb30a166ff5d2aa898d15a36a497400c271f1549f88c183c0ab6430689e`。本段更新当前发布状态，前文“下一独立片”保留为存储批当时边界。
