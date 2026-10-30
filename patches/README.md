# patches — 上游补丁

对上游（libghostty、libssh2、iSH 等）的**任何**本地修改都放这里，每个补丁必须有
同目录的 `WHY.md`（铁律 #9），内容至少包含：

1. **为什么**要改（要解决的具体问题，最好附失败现象/日志）；
2. **上游对照 commit**（补丁基于哪个版本）；
3. **是否需要同步升级**（上游若已合入，何时可撤）；
4. **验证方式**（哪个闸门/脚本能证明补丁生效）。

```
patches/
└── libghostty/
    └── <version>/
        ├── 0001-<slug>.patch
        └── WHY.md
```

> Apple full 引擎补丁集（`libghostty/8159838/`）：
> - `0001-host-io-backend.patch`：surface host-managed IO、输入/尺寸回调与输出 feed。
> - `0002-program-status.patch`：OSC 7501 接到嵌入者与查询回包。
> - `0003-semantic-prompt.patch`：OSC 133 语义提示符接到嵌入者。
>
> 三份补丁按序叠加到锁定 pin 的隔离源码副本；`build-ghostty.sh` / 普通引擎构建器产生 SDK 和收据，`gate-ghostty-patch.sh` 验证补丁与导出符号。旧 session/replica 专用补丁已退役，不重新应用。
> 每份补丁的理由、上游对照和撤除条件见 [WHY.md](libghostty/8159838/WHY.md)。

Android libssh2 补丁见 [1.11.1/WHY.md](libssh2/1.11.1/WHY.md)：SFTP 头部/正文 EOF 与未完成初始化清理。Android 构建从锁定源码重新解包应用，补丁哈希纳入收据；实际设备六场景及隔离 native/收据变异已有证据。当前未提交工作区的默认主机构建与 SwiftPM 也已接同版补丁库，但新增依赖守卫变异失败；两端产品受控打开仍未接线，详见[主机依赖状态](../docs/reference/host-ssh2-build.md)。

Android VT 补丁另见 `libghostty/android-vt-8159838/`：`0001-link-android-libc.patch` 使 Android 无 SIMD 构建链接系统 libc，防止空 `calloc/free` 替身污染宿主 C 分配器。已入库 `0002-local-clear.patch`，提供不写 PTY、不重置 parser/mode 的本地清屏 API。`build-android-native.py` 在隔离副本应用并记录补丁哈希，安装头文件与 archive 同时校验；Apple 构建与打包仍只使用上方三份补丁。最新设备与构建守卫证据见[Android 实现计划 §9.14](../docs/reference/android-implementation.md#914-a2-文本操作工作区与文档复核2026-10-30)；整个 A2 仍未完成。理由和撤除条件见 [WHY.md](libghostty/android-vt-8159838/WHY.md)。
