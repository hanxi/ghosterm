# macOS 菜单粘贴、确认与响应链

用户菜单的 `paste:` 现在调用 full libghostty 的 `paste_from_clipboard`，由引擎读取标准文本剪贴板、检查危险/多行内容并决定是否要求确认。程序化 `sendText` 仍保留原用途；用户粘贴不能借它绕过检查。安全文本与引擎允许的 bracketed paste 保留原编码，显式关闭保护仍按配置执行；不是遇到不支持的表示就偷偷退回文本注入。

确认分别表示用户粘贴、程序读取和程序写入。粘贴提示显示本次捕获文本的前 4096 个 UTF-8 字节，并标记截断，避免一个巨大组合字簇绕过预览上限；批准发送同一次捕获的完整请求并由引擎编码，不重新读取系统剪贴板。拒绝或撤销不发送输入；重复请求不能替换正在审阅的请求，旧回调不能批准新一轮。切换活动窗格、隐藏和销毁分别撤销未决请求；显示自身确认 sheet 导致窗口失焦，不当作切换窗格。

选中即复制已有引擎配置与标准写回消费者，本片补真实鼠标选区专项：`copy-on-select=true` 只复制实际选区；程序写入的 ask/拒绝仍保持独立语义。没有新增一个只声明不消费的设置开关，关闭确认规则也未更改。

## 菜单验证范围

进程安装的真实 `AppMenu` 继续使用 nil target，让 AppKit 按窗口/第一响应者找 `copy:` / `paste:`；无需修改既有菜单实现。新测试覆盖两个窗口、同窗口切窗格、普通 NSTextView，以及真实本地 PTY 回显。仅在测试进程注入 key/main-window 查询，菜单、nil target、响应链和实际窗口/视图均为生产对象；不显式改菜单 target。

后台 XCTest 使用 prohibited 策略，无法取得操作系统 key 窗口。首次直接请求系统焦点的三个用例失败已留档；新夹具明确限定环境查询，不激活或抢用户焦点。它不证明系统前台热键、跨应用激活或真实输入法体验，相关人工系统验证另记；不能把这层证据写成真实前台操作。

## 手工证据

隔离源码位于 `tmp/rem19`，候选构建在 `tmp/clp12`（已提交 main 加本批文件），保留另一 session 尚在重构的插件工作区。终端包完整 47 项零失败/跳过，原生确认框、原始快照、拒绝/撤销、单次授权、窗口/选区与大剪贴板均有专项；原件 `tmp/rem19/tmp/clipboard-terminal-full1.log`。

`python3 scripts/mutation-check-macos-clipboard.py --checkout tmp/隔离检出` 的十六个不同组完整通过：`tmp/rem19/tmp/clipboard-mutations-6zudfxyd/receipt.json`；增加实际本地 PTY 用例后，两菜单组在 `clipboard-mutations-zidgpe5e` 复验，不另计新增组。每组实际编译后单条正常 XCTest 断言红、同测试包独立网格/配置对照绿、恢复重编绿和 SHA256。原 `mutation-check-osc52.py --platform apple` 六组另已完整复验，日志 `clipboard-osc52-mutations.log` 与 `tmp/rem19/tmp/osc52-mutations/apple/`，不以它们冒充新十六组同轮证据。

最终候选跟进到已提交 `03ae03ed` 再合本批，实际自有 SSH 组合五十一项零失败/跳过；含进程菜单到真实 `/bin/sh` 本地 PTY 的 UTF-8 行读回、普通 SSH/default 入口、配置/设置与字体。`clipboard-candidate-ssh-final.log` 为实际 XCTest，`clipboard-ssh-current-driver2.log` 为服务启动/退出与非跳过检查。最初未提供 SSH 环境的五十项有一跳过，不记全绿；独立依赖下载等待也未计构建通过，复用已验证 pin 的私有缓存后重新验证。

受影响 `mutation-check-focus.sh` 的 F2 定位同步保留取消行为，目标红进一步要求编译完成、正常断言失败、零 unexpected 且无崩溃/sanitizer；绿拒绝空测试/跳过。五个既有保护完整复验，十四次目标/对照/最终检查通过，Source/View/Test SHA256 复原，原件 `tmp/rem19/tmp/gt-mutfocus-evidence-GB4bG4/` 与 `clipboard-focus-mutations.log`。它按既有整套焦点测试检查，不混称新十六组的单条目标红。

已提交 main 加本批的候选 C 格式 289 文件通过；私有前置检出 283 文件通过，Python/shell 语法也通过。原工作区全格式检查命中另一 session 的 `core/src/extension/session.c` / `session.h` 在途插件改动，未改它们或宣称原工作区全绿。跟进 `03ae03ed` 的候选 arm64 Release 通过（48.82 秒），原件 `clipboard-current-main-release.log`；前置候选 Release 58.63 秒通过也单独保留。无公共 Core ABI/JNI、Android 产品或上游补丁变化，不增加 CI 二十份。标准文本之外的 selection/binary/Kitty clipboard 表示和完整终端/系统矩阵不借本片结论扩大。

首轮剪贴板副本缺少数组类型标注而编译失败；夹具误建第二个引擎 App 导致崩溃，已遵守进程单实例并恢复测试配置。两者均单独记账，不当正常变异红；最终通过不覆盖它们。测试只使用自己的剪贴板字符串、私有配置和后台窗口，保存/恢复原剪贴板内容，不将其写入日志或快照。
