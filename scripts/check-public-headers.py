#!/usr/bin/env python3
"""Check public-header dependency direction and standalone C11 compilation."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def dependency_graph(include):
    headers = sorted(include.rglob("*.h"))
    aggregate = include / "ghosterm.h"
    if aggregate not in headers:
        raise RuntimeError("missing public aggregate ghosterm.h")
    graph = {}
    for header in headers:
        edges = []
        for kind, name in re.findall(
                r'^\s*#\s*include\s*(["<])([^">]+)[">]',
                header.read_text(), re.MULTILINE):
            candidates = ([header.parent / name] if kind == '"' else []) + [include / name]
            target = next((p.resolve() for p in candidates if p.is_file()), None)
            if target is None:
                if kind == '"':
                    raise RuntimeError(f"unresolved quoted include: {header.name}: {name}")
                continue  # System include; the compiler validates it.
            if not target.is_relative_to(include):
                raise RuntimeError(f"public header escapes include root: {target}")
            if header != aggregate and target == aggregate:
                raise RuntimeError(f"subheader includes aggregate: {header.relative_to(include)}")
            edges.append(target)
        graph[header] = edges

    visiting, visited = set(), set()

    def visit(header):
        if header in visiting:
            raise RuntimeError(f"public include cycle: {header.relative_to(include)}")
        if header in visited:
            return
        visiting.add(header)
        for target in graph[header]:
            visit(target)
        visiting.remove(header)
        visited.add(header)

    for header in headers:
        visit(header)
    return headers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--include", type=Path, default=ROOT / "core/include")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    include = args.include.resolve()
    headers = dependency_graph(include)
    (ROOT / "tmp").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="public-headers-", dir=ROOT / "tmp") as tmp:
        source = Path(tmp) / "standalone.c"
        for header in headers:
            name = header.relative_to(include).as_posix()
            source.write_text(f'#include "{name}"\n#include "{name}"\n')
            subprocess.run([*shlex.split(args.cc), "-std=c11", "-Wall", "-Wextra",
                            "-Werror", "-fsyntax-only", "-I", str(include), str(source)],
                           check=True, timeout=30)
    print(f"PUBLIC HEADERS PASS ({len(headers)} standalone C11 headers; acyclic dependencies)")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"public headers failed: {error}", file=sys.stderr)
        sys.exit(1)
