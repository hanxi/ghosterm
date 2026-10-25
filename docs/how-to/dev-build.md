# 拿一份 dev 版 macOS 包

每次 push 到 `main` 都会自动出一个 macOS 包，放在滚动 tag `dev` 的 prerelease 里。
本页说明**怎么装、装了之后怎么确认它到底是怎么造出来的**，以及**怎么在本地自己出一份**。

## 从 GitHub Releases 装

1. 下载 [`dev` release](https://github.com/hanxi/ghosterm/releases/tag/dev) 里的
   `Ghosterm-0.1.0-dev-arm64.dmg`（连同 `SHA256SUMS`）。
2. 校验和比对——`SHA256SUMS` 是打包脚本对**最终字节**算的：

   ```bash
   shasum -a 256 -c SHA256SUMS
   ```

3. 挂载拖进「应用程序」。**首次打开会被 Gatekeeper 拦**（dev 轨没有 Developer ID
   证书，只做 ad-hoc 签名、不公证），需要下面任一动作：
   - 右键图标 →「打开」→ 确认；或
   - `xattr -dr com.apple.quarantine /Applications/Ghosterm.app`

## 确认这个包是怎么来的：读 `BUILD-INFO.txt`

随包与 release 正文都带同一份 `BUILD-INFO.txt`，它是**实际值**而不是配置值：

| 字段 | 含义 |
|---|---|
| `版本: 0.1.0-dev (build N)` | `N` = 那次 CI 运行的 `GITHUB_RUN_NUMBER`。**没有 tag 时版本号就是 `0.1.0-dev`** |
| `架构` | 目前只有 `arm64`（`GhosttyKit.xcframework` 只切了 arm64） |
| `最低系统` | 二进制自身的 `minos` 与**内嵌依赖**里最高者的较大值——内嵌的 Homebrew 库会把最低系统抬高，所以这一行往往大于 `Info.plist` 里的原始声明 |
| `签名` | dev 轨恒为 `adhoc` |
| `Linux glibc 下限` | 内嵌的 Linux 会话安装包按架构的 `min_glibc`（如 `x86_64=min_glibc 2.34`）——**远端主机**能不能跑 `ghosterm-sessiond` 就取决于它，与本机 macOS 无关 |

`CFBundleVersion` 用的是 dev workflow 自己的运行编号。拆成 dev / release 两个 workflow
文件之后，两条轨的 build 号**不再同源单调**（`GITHUB_RUN_NUMBER` 按文件计数）；
比较先后请以 commit 为准，别拿 build 号跨轨比大小。

## 「有没有新版」的机读入口：`version.json`

dev Release 上挂了一份固定 URL 的清单，给客户端做**更新检测**用：

```bash
curl -fsSL https://github.com/<owner>/ghosterm/releases/download/dev/version.json
```

为什么是它而不是 GitHub API：`releases/download/dev/<name>` 是**固定直链**，不占
未认证 API 的 60 次/小时额度，也不需要 token；渠道判据因此全部落在文件里。

| 字段 | 口径 |
|---|---|
| `git_commit` | **dev 渠道新旧的唯一主锚**。整条 40 位，比对不等即「有新版」 |
| `build_time` | 本轮 workflow 的 `run_started_at`，只在 commit 缺失时兜底 |
| `version` / `build` | `0.1.0-dev` 与该 workflow 文件的运行编号，**不可跨轨比大小** |
| `min_macos` / `arch` | 装机下限；`arch` 目前恒为 `arm64` |
| `signature` / `notarized` | `adhoc` / `false`——这两个字段就是「不许自动安装」的机器可读声明 |
| `assets` + `sha256` + `download_base` | 拼出 dmg/zip 直链并自行校验，不走 API 拿 `browser_download_url` |
| `session_contract` / `min_glibc` | 内嵌会话安装包的契约与逐架构 glibc 下限 |

清单由 `.github/workflows/dev-build.yml` 的「生成 version.json」step 从**随包
`BUILD-INFO.txt` 与 `SHA256SUMS`** 解析而来（不是第二份真相），解析不到任何一项
就当场红——写 `unknown` 蒙过去等于把检测做成永远「已是最新」。同一 job 的「复验」
step 再从固定 URL 把它取回来对答案（commit、渠道、架构、sha256 形状），取不到或
对不上就判失败。

## dev 包的边界（不是缺陷，是现状）

- **只有 macOS**。Android / iOS 目前没有构建入口，不是 CI 漏了一条。
- **`dev` 不能当自动更新通道**：每轮 ad-hoc 签名的 cdhash 都不同，而 `dev` tag 会被
  强删重建；做自动更新要等 Developer ID 签名 + 公证（见
  [roadmap](../reference/roadmap.md) 的遗留项）。「有没有新版」的检测不受此限制，
  见上面 [version.json](#有没有新版的机读入口versionjson)——它只给直链，装不装由人。
- 构建细节与为什么必须先造 Linux 包，见 [CI 出包口径](#ci-是怎么串起来的)。

## 本地自己出一份

前提：macOS arm64 主机 + 一份已构建好的 Linux 会话安装包（只能由 Linux 机器产出）。

```bash
# 1) 契约预检：Linux 侧会喂给 CMake 的头树，是否与已验证的 mac SDK 头树逐字节相同
python3 scripts/build-session-engine.py            # 产出 build/GhosttyKit.xcframework
python3 scripts/build-session-vt-linux.py --dry-run

# 2) 在 Linux（aarch64 或 x86_64）上造 bundle：出 libghostty-vt.so + sessiond 包
python3 scripts/build-session-vt-linux.py          # 末行打印 bundle 目录

# 3) 出 mac 包（显式带上两个架构的 bundle；缺了 package-mac.sh 会直接 fail）
bash scripts/package-mac.sh --out dist --adhoc --build 42 \
  --session-linux-bundle <x86_64-bundle> --session-linux-bundle <aarch64-bundle>
```

没有 Linux 机器时可以用容器（min_glibc 由容器里的 glibc 决定，别用宿主的新版本）：

```bash
docker run --rm -it -v "$PWD":/work -w /work arm64v8/ubuntu:22.04 bash
# 容器内（与 CI 的 linux-bundle job 同一套依赖）：
#   mkdir -p tmp                       # TMPDIR 指到仓库 tmp/，容器里那目录还不存在
#   apt-get update && apt-get install -y --no-install-recommends \
#     build-essential ca-certificates cmake curl git pkg-config python3 tar xz-utils
#   git config --global --add safe.directory "*"
#   ZIG=/path/to/zig python3 scripts/build-session-vt-linux.py
```

容器里跑之前记得先取 zig 0.16.0（`libghostty` 的 `build.zig.zon` 钉了最低版本），
CI 是下载后缓存在 `tmp/zig/` 并传 `ZIG=$PWD/tmp/zig/zig`。

## CI 是怎么串起来的

```
push main ─▶ dev-build.yml
              ├─ macos-package.yml（workflow_call：channel=dev、gates=light）
              │    ├─ linux-bundle ×2（x86_64 / aarch64，ubuntu:22.04 容器）
              │    │     └─ scripts/build-session-vt-linux.py → sessiond-linux-<arch> artifact
              │    └─ mac-package ─▶ scripts/package-mac.sh → Ghosterm-macOS-dev-<sha> artifact
              └─ release-dev ─▶ 先验产物 → 删旧 dev → gh release create dev --prerelease
```

三个入口（tag `v*` / PR / 手动）走 `macos-release.yml`，调同一个
`macos-package.yml`，只是 `channel=release、gates=full`——多跑 Apple 端全量闸门、
Linux 侧的 `scripts/mutation-check-sessiond-linux.py`，并在有证书 secret 时才走
Developer ID 签名 + 公证分支。

为什么 Linux bundle 必须先造：`scripts/package-mac.sh` 硬要求内嵌 Linux 会话安装包，
而 `scripts/stage-session-runtime.py` 要求 bundle 的 session contract 与 mac 端
**逐位一致**。契约由 `daemon/CMakeLists.txt` 对「include 根下全部 `*.h` +
`patches/libghostty/8159838/**/*.patch` + schema 源文件」取 sha256 拼出，所以 Linux
侧必须打**同一补丁全集、同一顺序**，且 `--include` 指向 `<engine>/include`（36 个头，
含顶层 `ghostty.h`）而**不是** `zig build` 装出来的 `zig-out/include`（只有 35 个）。
这条链条的四条断言由 `scripts/mutation-check-sessiond-linux.py` 验牙。

## 相关文档

- [从 macOS 使用 Linux 持久会话](linux-persistent-session.md)——远端要求 glibc ≥ 2.34 的由来
- [能力矩阵](../reference/capability-matrix.md)——构建与交付一行的状态口径
- [路线图](../reference/roadmap.md)——签名 / 公证 / 降最低系统版本的遗留项
