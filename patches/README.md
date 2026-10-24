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

> 现状（`libghostty/8159838/`，2026-10-14）：
> - `0001-host-io-backend.patch`（M1 落地）：给 surface 加 **host-managed IO**
>   后端（`ghostty_surface_config_s.host` + `host_write`/`host_resize` +
>   `ghostty_surface_process_output()`）。
> - `0002-program-status.patch`：把 OSC 7501（Program Status Protocol）从
>   解析层接到嵌入者（`host_program_status` 同步回调 + 支持查询回包）。
>   上游当时只做到 libghostty-vt 层，full libghostty 的 app 路径是 no-op。
> 两份补丁**叠加**（0002 改的是 0001 刚加的上下文），故 `gate-ghostty-patch.sh`
> 按「在干净 pin 树上按序应用」的口径检查。
>
> 历史：`libghostty/8159838/0001-host-io-backend.patch`（M1 落地）。给 surface 加
> **host-managed IO** 后端：`ghostty_surface_config_s.host` 关掉子进程/pty，
> `host_write` / `host_resize` 回调把输入与尺寸交给嵌入者，
> `ghostty_surface_process_output()` 把 transport 读到的字节喂进网格。
> 为什么、上游对照 commit 与可撤性见同目录 `WHY.md`。
