# gterm 前台 CLI

独立构件，不加入 Ghosterm App 包、不安装远端 helper、不修改 shell 配置。
当前接入诊断及固定包兼容性检查；命令激活、本机 IPC、安装/升级仍未实现。
它不提供业务命令，未知命令返回 `COMMAND_ACTIVATION_UNAVAILABLE`。

从仓库根目录构建（产物在工程 `tmp/`）：

```sh
cmake -S cli/gterm -B tmp/gterm-build \
  -DGHOSTERM_WITH_PLUGIN_JS=ON -DGHOSTERM_WITH_LIBSSH2=OFF
cmake --build tmp/gterm-build -j 4
./tmp/gterm-build/gterm doctor
./tmp/gterm-build/gterm plugin check --host-json ./host.json -- ./plugin-directory
```

`host.json` 是调用方明确提供的能力观察，遵循包检查器的 host 事实格式。例如：

```json
{"side":"executor","sdkVersion":"1.0.0","protocols":[1],"runtimes":["portable-js"],"capabilities":["session.rpc"]}
```

兼容性检查不验证这些事实对应的服务确实已连接，也不证明发布者身份或授予执行权限。
省略 `--host-json` 时保留 UNKNOWN；不猜测用户环境。该文件限 64 KiB，拒绝 NUL、
最终路径符号链接及非普通文件。包根目录最终路径不跟链接，内部遍历、预算、完整性及
metadata 校验由公共 Core 包 owner 完成。接受相对路径；`--` 后的目录名不再解析为选项。

成功输出单个 JSON 对象，包含包摘要、发布者声明与命令贡献；非 ASCII 字符使用 JSON
Unicode 转义，终端控制字符不会直接写到终端。检查不执行包模块：即使入口含顶层抛错，
元数据兼容的包仍可通过。`compatible` 只表示与给定 host 事实兼容，来源及运行批准固定
为 false。关闭引用失败返回非零；输出错误也不得返回零。

退出码：0 为检查兼容或所查询的 JS 引擎可用；2 为参数/host 输入错误；3 为 UNKNOWN；
4 为不支持；5 为包校验、IO 或其它失败；130 为收到取消。stdout 为 JSON，参数用法与
关闭诊断写 stderr。SIGINT/SIGTERM 只设置取消标记，采集循环在下一步停止；同步元数据
校验受 Core CPU 预算限制，返回后检查标记，不在 signal handler 调 Core。

```sh
python3 scripts/test-gterm.py --binary tmp/gterm-build/gterm
python3 scripts/mutation-check-gterm.py
```

变异 runner 只改工程 `tmp/` 中的 CLI 副本，锁定 SDK 工具离线构建；Core / 上游为只读
输入。实际重新编译 CLI，目标正常断言红、同 archive 配置对照绿、复原绿并核对源码摘要。
这是 CLI 检查入口验收，不替代未来激活、IPC、SSH 或实际业务插件验收。
