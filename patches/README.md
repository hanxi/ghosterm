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

> 当前补丁集（`libghostty/8159838/`，2026-10-29 核对）：
> - `0001-host-io-backend.patch`：surface host-managed IO、输入/尺寸回调与输出 feed。
> - `0002-program-status.patch`：OSC 7501 接到嵌入者与查询回包。
> - `0003-semantic-prompt.patch`：OSC 133 语义提示符接到嵌入者。
>
> 三份补丁按序叠加到锁定 pin 的隔离源码副本；`build-ghostty.sh` / 普通引擎构建器产生 SDK 和收据，`gate-ghostty-patch.sh` 验证补丁与导出符号。旧 session/replica 专用补丁已退役，不重新应用。
> 每份补丁的理由、上游对照和撤除条件见 [WHY.md](libghostty/8159838/WHY.md)。
