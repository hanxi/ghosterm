# Clang 跳转槽重定位循环的局部保护

上游仍钉 QuickJS-NG 0.18.0 的 `d7215b0ad3fc2c562e8317fd9bca058fd4863804`。
不改 submodule；构建器核对上游文件后，在自己拥有的 build 副本应用此补丁。

2026-11-06，Apple clang 21.0.0（clang-2100.3.34.2）、arm64、`-O2 -g -DNDEBUG`
加 ASan/UBSan 编译本项目实际 metadata validator 时，`resolve_labels` 的未来 JumpSlot
重定位循环出现 64 字节读取，ASan 报访问到分配末尾之后第 7 字节。独立的
`JS_NewRuntime` / 默认分配器探针加载同一 165,677 字节语料同样复现；不是 Core 自定义
分配器独有的问题。全局禁向量化的诊断档通过；仅为这一循环添加 Clang loop hint 后，
原 `-O2` 和 sanitizer 档也通过。这里记录本机观察，不声称其它编译器均受影响或已有上游 CVE。

`0001-clang-jump-slot-loop.patch` 只对该 strided field loop 禁用 Clang 向量化 / interleaving，
其它循环和全局优化保持原档。没有关闭 sanitizer、扩大分配来遮盖越界或改成只测 Debug。
指令语义见 [Clang loop hints](https://clang.llvm.org/docs/LanguageExtensions.html#extensions-for-loop-hint-optimizations)。
`test_package_compiler` 独立使用默认分配器和实际语料；包检查器的优化 sanitizer 变异档同时
覆盖自定义预算分配器。未打补丁的 ASan 复现是独立故障证据，不计为「正常断言红」变异。

上游 `quickjs.c` SHA256 为
`51b1d3919ffe812d0e01793f85d347bcea48c924c11262c5c5f00df1e4759d74`；
应用后为 `31def914f34b582152b031f7f46cdd52b4d8e4ec6a5f6ac1556718d36a21d706`。
构建同时固定 patch 字节、上游 pin / 文件与应用后内容，拒绝缺补丁或混用未修正源码。
已有正确副本复用，不因元数据重配置而重复重编引擎。更换 pin / 工具链后，先用同语料和
优化 sanitizer 档验证，再决定是否撤回。原 MIT 许可保留；独立复现日志见插件计划第二十九批。

## 编译 OOM 路径（第二十九批新增实测）

Android API36 / arm64 / 4 KiB 页、NDK 28.2 `RelWithDebInfo` 的真实包检查器在第
1000 次分配注入失败时，`resolve_labels` 仍以不完整 DynBuf 做跳转压缩，传给
`memmove` 的长度下溢，导致 SIGSEGV。第二份补丁在迭代开始与最终重定位前检查 sticky
buffer error；正常成功才转移字节码所有权。消除指令时同时清空输入对应的 atom operand，
使失败后的函数销毁不会再次释放它；未消费的重定位链也在错误出口释放。
原字节码 buffer 在这段 pass 内始终可写，`CodeContext.atom_pos` 记录跳过 source-location
指令后的实际 operand 位置。不是增加缓冲垫、隐藏 OOM 或关闭检查。

`0003-class-constructor-oom.patch` 处理同类的 class constructor operand 回填：先确认
发射缓冲区没有分配错误，再写构造器索引。逐个注入分配失败时，本机 Debug 的 ordinal 189
与优化 ASan 档的 ordinal 81 在此处写入空指针；ordinal 只对应该次构件，不是跨平台常量。

`0004-child-pool-oom.patch` 传播默认 / 显式构造函数 constant-pool 预约失败，并在编译
子函数前核对其 slot。此前 Debug ordinal 449 的 `cpool_idx >= 0` 断言退出；优化
ASan / UBSan 的 ordinal 153 在此前负索引写入后，释放时出现 arena header 索引越界。
错误路径须保留 caller 的原快照，释放全部 checker 分配并擦除。

各 patch 顺序由 `core/cmake/PluginJS.cmake` 固定文件摘要；五份应用后的 `quickjs.c` 为
`7f61e8e8f4f9b823c508ee2799da7a1d3145d70ec780ff7cff984c4ccdd70ca0`。
未修改 submodule，也未向上游提交问题；这是当前 pin / 工具链 / 实际语料的本地证据，
不声称覆盖整个引擎所有脚本的 OOM。单点复现、逐 ordinal 扫描与最终验收分别记在插件计划。

`0005-partial-function-oom.patch` 在创建函数对象前拒绝 sticky DynBuf error / 待处理异常，
避免 parser helper 的分配失败没有逐层传播 -1 时，把截断字节码交给变量解析。实测 Debug
ordinal 818 命中无效 label 断言，优化 ASan / UBSan 与 Android 的 ordinal 431 分别报
`resolve_variables` 越界 / SIGSEGV。此处依据原始错误拒绝，不将失败缓冲区修成可执行代码。

## DynBuf 与 switch 标签回填（第三十一批）

第三十批共享分配器后的 Debug 构件在实际 metadata 语料的分配序号 1075 重现崩溃。
LLDB 确认失败发生于 switch 分支的 `emit_goto → emit_u32 → dbuf_claim`：随后 default
分支仍以 `byte_code.size - 4` 记录并回填一个未成功发射的 operand，覆盖已有指令。
清理因此将已损坏的前缀误读为 atom 1115587；并非 atom 表本身缺少越界检查。

`0006-sticky-dynbuf-oom.patch` 使 claim、普通/自身复制及四个整数快路径在 error 后均
停止写入，保留失败前的连续前缀。`0007-switch-label-oom.patch` 在解析 switch 后、
回填 default 之前检查原发射错误并传播 OOM。仅加 DynBuf sticky 仍能重现旧回填崩溃，
两者分别留有失败日志。不忽略 atom、不填充损坏字节码、不放宽内存预算。

独立 `test_plugin_compiler_oom` 不依赖包 validator，覆盖有备用容量时七种写/预约操作、
class/private field/闭包及含 120 case 的 switch 逐分配失败和销毁归零。
隔离变异 runner 的 switch 边界只在危险回填前观察并阻止非法写，正常路径执行原代码；
这样可用正常断言验省略生产检查，而不将崩溃计作目标红。实际无观察钩子的包语料
逐分配扫描另列证据，不以边界观察替代真实编译器回归。
