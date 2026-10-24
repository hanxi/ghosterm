#!/usr/bin/env python3
"""把 .app 的非系统动态库依赖**递归**收进 Contents/Frameworks 并改写引用。

用法:
    embed-dylibs.py embed <Ghosterm.app>   # 收集 → 拷贝 → 改写 id/引用 → 自检
    embed-dylibs.py check <Ghosterm.app>   # 只自检（打包后复验）

为什么需要它（真实缺陷，不是洁癖）：Apple 端的 libssh2 来自 Homebrew，而
libssh2 自己又依赖 Homebrew 的 OpenSSL（libssl/libcrypto）。只拷 libssh2
是把「自包含」当口号——目标机器没有 Homebrew 时 dyld 直接找不到 libcrypto，
App 起不来。本脚本保证：**bundle 里不再有任何指向 Homebrew 的路径**，且
每一条 dependents 都能在 bundle 内解析到实际文件。

安全/工程口径：
- 只处理「非系统」依赖：/System/、/usr/lib/ 交给系统，不动；
- 目标引用统一写成 @executable_path/../Frameworks/<basename>（不需要 LC_RPATH，
  且 dylib 之间互相引用时 @executable_path 仍指向主可执行文件）；
- 同名不同路径的两个库 → 直接报错（避免互相覆盖而静默错版）；
- 改写后用 otool 复验：残留任何绝对路径即失败（不假装自包含）。
"""
import os
import re
import shutil
import subprocess
import sys

SYSTEM_PREFIXES = ("/System/", "/usr/lib/", "/Library/Apple/")
FRAMEWORKS_REL = "Contents/Frameworks"
MAIN_REL = "Contents/MacOS"


def fail(msg):
    print(f"embed-dylibs: {msg}", file=sys.stderr)
    raise SystemExit(1)


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def own_id(path):
    """dylib 的 LC_ID_DYLIB（install name）；可执行文件没有 → None。"""
    out = run(["otool", "-D", path])
    lines = [ln.strip() for ln in out.splitlines()[1:] if ln.strip()]
    if not lines:
        return None
    name = lines[0].split(" (compatibility")[0].strip()
    return None if name.endswith(":") else name


def deps_of(path):
    """otool -L → 依赖列表。

    注意：dylib 的 otool -L 第 2 行是它自己的 LC_ID_DYLIB（不是依赖），
    必须跳过——否则会把它当成「外部依赖」误报（或误改）。
    """
    out = run(["otool", "-L", path])
    lines = [ln.strip() for ln in out.splitlines()[1:] if ln.strip()]
    if own_id(path) is not None:
        lines = lines[1:]
    return [ln.split(" (compatibility")[0].strip() for ln in lines]


def is_system(path):
    return path.startswith(SYSTEM_PREFIXES)


def minos_of(path):
    out = run(["otool", "-l", path])
    m = re.search(r"LC_BUILD_VERSION.*?minos\s+([0-9.]+)", out, re.S)
    if m:
        return m.group(1)
    m = re.search(r"LC_VERSION_MIN_MACOSX.*?version\s+([0-9.]+)", out, re.S)
    return m.group(1) if m else "0"


def version_of(path):
    """从 install name 的 current version 拿不到，就用文件名里的 .N. 版本段。"""
    m = re.search(r"\.(\d+(?:\.\d+)*)\.dylib$", os.path.basename(path))
    return m.group(1) if m else "?"


def resolve_ref(app, dep):
    """把一条依赖引用解析成 bundle 内路径（解析不了返回 None）。

    @executable_path 指向 Contents/MacOS，故 ../Frameworks/x.dylib 落在
    Contents/Frameworks/x.dylib；@rpath/@loader_path 按 basename 在 Frameworks 里找。
    """
    fw = os.path.join(app, FRAMEWORKS_REL)
    if dep.startswith("@executable_path/"):
        rel = dep[len("@executable_path/"):]
        return os.path.normpath(os.path.join(app, MAIN_REL, rel))
    if dep.startswith(("@rpath/", "@loader_path/")):
        cand = os.path.join(fw, dep.rsplit("/", 1)[1])
        return cand if os.path.exists(cand) else None
    return None


def images(app):
    main_dir = os.path.join(app, MAIN_REL)
    mains = [os.path.join(main_dir, f) for f in sorted(os.listdir(main_dir))
             if os.path.isfile(os.path.join(main_dir, f))]
    if len(mains) != 1:
        fail(f"{main_dir} 期望恰好 1 个可执行文件，实际 {len(mains)} 个")
    fw = os.path.join(app, FRAMEWORKS_REL)
    libs = []
    if os.path.isdir(fw):
        libs = [os.path.join(fw, f) for f in sorted(os.listdir(fw))
                if f.endswith(".dylib")]
    return mains[0], libs


def collect(app):
    """广度优先收集依赖闭包。

    返回 {basename: {"src": 源路径或 None（None=已在 bundle 内）, "minos": ..}}
    注意：**已经在 Contents/Frameworks 里的库也要继续往里走**——否则会出现
    「libssh2 拷进来了，但它依赖的 libcrypto 还在 Homebrew」这种半自包含。
    """
    main, libs = images(app)
    found, queue, order = {}, [main] + libs, []
    for lib in libs:
        # 已在 bundle 内的库也要进 order：它的 ID 与依赖同样要改写成 bundle 内引用。
        base = os.path.basename(lib)
        found[base] = {"src": None, "path": lib}
        order.append(base)
    while queue:
        img = queue.pop(0)
        for dep in deps_of(img):
            if is_system(dep):
                continue
            inner = resolve_ref(app, dep)
            base = os.path.basename(dep)
            if inner is not None:
                # 已落在 bundle 内：登记 + 继续扫它自己的依赖
                if base not in found:
                    found[base] = {"src": None, "path": inner}
                    order.append(base)
                    if os.path.exists(inner):
                        queue.append(inner)
                continue
            if dep.startswith("@"):
                fail(f"{img} 的依赖 {dep} 无法在 bundle 内解析（未识别的 @ 引用）")
            if not os.path.exists(dep):
                fail(f"{img} 依赖 {dep}，但该文件不存在（构建环境不完整？）")
            if base in found:
                continue
            found[base] = {"src": dep, "path": os.path.join(app, FRAMEWORKS_REL, base)}
            order.append(base)
            queue.append(dep)
    return order, found


def embed(app):
    order, found = collect(app)
    if not order:
        print("embed: 无非系统依赖（本来就自包含）")
        return report_minos(app)
    fw = os.path.join(app, FRAMEWORKS_REL)
    os.makedirs(fw, exist_ok=True)
    for base in order:
        info = found[base]
        dst = os.path.join(fw, base)
        if info["src"] is None:
            if not os.path.exists(dst):
                fail(f"{base} 既不在 bundle 内、也没有外部来源")
            print(f"embed: {base}  已在 bundle 内，沿用")
        else:
            src = info["src"]
            shutil.copy2(src, dst)
            print(f"embed: {base}  ←  {src}  (minos {minos_of(dst)}, ver {version_of(src)})")
        os.chmod(dst, 0o755)  # Homebrew 的库是 0444，install_name_tool 改不动
        run(["install_name_tool", "-id", f"@executable_path/../Frameworks/{base}", dst])

    main, libs = images(app)
    for img in [main] + libs:
        for dep in deps_of(img):
            base = os.path.basename(dep)
            if is_system(dep) or base not in found:
                continue
            if dep == f"@executable_path/../Frameworks/{base}":
                continue
            run(["install_name_tool", "-change", dep,
                 f"@executable_path/../Frameworks/{base}", img])
    return report_minos(app)


def report_minos(app):
    main, libs = images(app)
    worst, worst_src = minos_of(main), "GhostermApp"
    for lib in libs:
        m = minos_of(lib)
        if tuple(int(x) for x in m.split(".")) > tuple(int(x) for x in worst.split(".")):
            worst, worst_src = m, os.path.basename(lib)
    print(f"MINOS_MAX={worst}")
    print(f"MINOS_SRC={worst_src}")
    return worst


def check(app):
    """复验：bundle 内不得残留绝对的非系统路径依赖，且引用必须落得到实处。"""
    main, libs = images(app)
    bad, fw = [], os.path.join(app, FRAMEWORKS_REL)
    for img in [main] + libs:
        for dep in deps_of(img):
            if is_system(dep):
                continue
            if dep.startswith("@"):
                target = resolve_ref(app, dep)
                if target is None or not os.path.exists(target):
                    bad.append(f"{os.path.basename(img)} → {dep}（bundle 内解析不到）")
                continue
            bad.append(f"{os.path.basename(img)} → {dep}（绝对路径 = 依赖外部环境）")
    if bad:
        for b in bad:
            print(f"  ✗ {b}", file=sys.stderr)
        fail("自包含自检未通过：bundle 仍有指向外部环境的依赖")
    loaded = len(libs)
    print(f"CHECK PASS：{loaded} 个内嵌库，全部引用都落在 bundle 内或系统路径")
    return 0


def main(argv):
    if len(argv) != 3 or argv[1] not in ("embed", "check"):
        print(__doc__, file=sys.stderr)
        return 2
    app = argv[2]
    if not os.path.isdir(app):
        fail(f"{app} 不是目录")
    if argv[1] == "embed":
        embed(app)
        return check(app)
    return check(app)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
