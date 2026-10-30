# libssh2 1.11.1：SFTP EOF 与未完成初始化清理

源码：`https://libssh2.org/download/libssh2-1.11.1.tar.gz`，SHA256
`d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7`。
不修改 vendor 或系统 Homebrew 安装；补丁只应用到工程 `tmp/` 的校验后源码副本。
保留上游 BSD-3-Clause 许可；构建继续随包提供 libssh2 的 `COPYING`。

## 为什么需要

Android 远端文件需要能够取消的专用连接。外层在 `libssh2_sftp_init`
返回 EAGAIN 后检查 deadline，并不能中断库内部不返回的循环：服务器声明
16384 字节 SFTP VERSION payload，只发类型与版本号后发送 channel EOF，
1.11.1 的部分报文读取循环会不断把 0 加到已读长度，无法结束。
头部 EOF 则会反复返回 EAGAIN，未明确报告协议流已经结束。

`0001-sftp-eof.patch` 将这两种 EOF 映射为 `LIBSSH2_ERROR_SOCKET_RECV`，
部分报文缓冲随错误释放。行为移植自
[上游 SFTP 实现](https://github.com/libssh2/libssh2/blob/master/src/sftp.c)
（2026-10-30 核对的 `sftp_packet_read`，在 1.11.1 的原命名下实现）；
不是新增可恢复协议，也不把断线伪装成“不支持 SFTP”。

`0002-sftp-init-cleanup.patch` 处理另一条所有权边界：成功前尚未安装 SFTP
channel destructor；原 session free 只释放 init 对象，遗漏 partial packet
和已入队但尚未匹配 VERSION 的报文。新增内部清理函数在错误关闭及 session
释放时、channel 仍有效时，先清队列和 partial buffer，再释放 init 对象。
重复进入清理为空操作；成功初始化后 session 的 init 指针本来就会清空。
没有新增公开 libssh2 API，也没有改变 Ghosterm C ABI。

## 构建与当前范围

Android 的 `scripts/build-android-native.py` 从校验过的 tarball 重新解包后
依序应用两份补丁，使用独立 CMake 构建目录，补丁 SHA256 纳入依赖收据。
库产物 SHA256 仍由原缓存校验检查；原未打补丁的缓存不能命中新收据。
当前未提交工作区另有 `scripts/build-host-ssh2.py`，默认 Core 与 SwiftPM 已消费
`build/host-native` 中的同版补丁库，打包时嵌入它及收据。主机协议与普通打包
已通过，新增依赖守卫变异失败，不能记成依赖切片验收完成；详见
[主机构建与证据](../../../docs/reference/host-ssh2-build.md)。
两端产品取消接线、Android 文件 owner 与完整 A4 仍未交付。

## 复现与有牙验证

```bash
bash scripts/bootstrap-ssh-channel-tests.sh
python3 scripts/test-libssh2-sftp.py --fetch --mutate
# 已有源码包时离线复用：
python3 scripts/test-libssh2-sftp.py \
  --archive tmp/android-native/arm64-v8a/libssh2-1.11.1.tar.gz --mutate
```

脚本校验锁定 SHA256，在独立目录构建真实静态库，并直接链接公共 libssh2 API
探针。认证通过真实 SSH，协议故障端明确观测 channel/subsystem/VERSION 顺序。
六个场景：正常 VERSION、头部 EOF、正文 EOF、部分报文后取消/超时、非 VERSION
报文入队后取消。自定义分配器追踪初始化开始后的分配；必须在 session 释放后归零，
取消不能靠提前取消而绕过 partial allocation。正常成功及关闭是独立对照。

五组变异分别撤去头部 EOF、正文 EOF、partial buffer 释放、报文队列清理和
session 释放时的 init 清理。每组重新编译，要求指定目标断言红（正文循环由外部
8 秒超时明确判红）、同库正常连接绿、复原后目标绿与源码 SHA256 一致。
另以两组隔离 Python 变异验证补丁记录和 archive 摘要守卫：目标拒绝断言红，另一种错误收据仍被拒绝、合法收据仍通过，恢复后复验。生产补丁/探针/协议夹具不被变异；临时构建清理，日志保留于
`tmp/libssh2-sftp-tests/`。这些检查尚未纳入 CI。

分配器另观察到正常认证在 SFTP 开始前留下两笔 48 字节分配；诊断栈位于
`curve25519_sha256` / handshake。该问题没有由本补丁修复；本测试只证明
**SFTP 初始化期间新分配**的回收，不声称整个 libssh2 连接零泄漏。
同样不覆盖所有内存分配失败、后续 SFTP 操作取消、原子文件提交或完整断网矩阵。

设备产物另外使用以下命令测试；它先核对实际 Android archive 与补丁收据，
再用同一 NDK 将探针链接到这些 archive，经指定设备运行六个协议场景。
临时私钥只通过 adb 标准输入送入探针内存，不落设备文件或进程参数；测试二进制和
本轮 adb reverse 在结束时清理。该测试不包含远端文件页面或 JNI owner 接线。

```bash
python3 scripts/test-libssh2-sftp.py --android emulator-5554
```

## 撤除条件

升级锁定版本后，在实际发布依赖上证明两种 EOF 有界返回、取消/超时后 partial
及排队报文完整释放，并让正常连接对照通过；再移除补丁和构建收据字段。
不能只依据上游版本号、正常 SFTP 能打开或编译成功而撤除。
