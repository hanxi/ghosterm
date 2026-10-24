#!/usr/bin/env python3
"""Prove PTY descriptor isolation checks in a private core copy."""
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "core/src/io/pty_posix.c"
MARK = "GT_PTY_FD_MUTATION_MARK"


def run(command):
    subprocess.run(command, check=True, timeout=120, stdout=subprocess.DEVNULL)


def test(build, name, expected=0):
    result = subprocess.run(["ctest", "--test-dir", str(build), "-R", f"^{name}$",
                             "--output-on-failure"], capture_output=True, text=True, timeout=30)
    if result.returncode != expected:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise RuntimeError(f"{name}: expected ctest exit {expected}, got {result.returncode}")


def interrupted(signum, frame):
    raise InterruptedError(f"signal {signum}; restoring private core copy")


def main():
    original = SOURCE.read_bytes()
    digest = hashlib.sha256(original).digest()
    if MARK in original.decode():
        return 2
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    (ROOT / "tmp").mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(ROOT / "tmp")
    try:
        with tempfile.TemporaryDirectory(prefix="pty-inheritance-", dir=ROOT / "tmp") as tmp:
            work = Path(tmp)
            core, build = work / "core", work / "build"
            shutil.copytree(ROOT / "core", core,
                            ignore=shutil.ignore_patterns("build", "build-*", ".build"))
            source = core / "src/io/pty_posix.c"
            run(["cmake", "-S", str(core), "-B", str(build)])

            def compile_tests():
                run(["cmake", "--build", str(build), "--target",
                     "test_pty_inheritance", "test_api", "-j", "2"])

            compile_tests()
            test(build, "pty_inheritance")
            test(build, "api_smoke")
            print("PASS baseline: PTY isolation and API control", flush=True)
            variants = (
                ("owned descriptors lose CLOEXEC", "descriptor_flags | FD_CLOEXEC",
                 "descriptor_flags & ~FD_CLOEXEC"),
                ("child leaks caller descriptors", "fd < limit; fd++",
                 "fd < 3 && fd < limit; fd++"),
            )
            text = original.decode()
            for label, old, new in variants:
                if text.count(old) != 1:
                    raise RuntimeError(f"mutation anchor is not unique: {label}")
                try:
                    source.write_text(text.replace(old, new + f" /* {MARK} */", 1))
                    compile_tests()
                    test(build, "pty_inheritance", 8)
                    test(build, "api_smoke")
                    print(f"PASS mutation/{label}: target red, control green", flush=True)
                finally:
                    source.write_bytes(original)
                    if hashlib.sha256(source.read_bytes()).digest() != digest:
                        raise RuntimeError("private PTY source rollback SHA256 mismatch")
                compile_tests()
                test(build, "pty_inheritance")
                test(build, "api_smoke")
            print("PASS 2 mutations and rollback; repository sources unchanged")
        return 0
    finally:
        if hashlib.sha256(SOURCE.read_bytes()).digest() != digest:
            raise RuntimeError("repository PTY source SHA256 changed")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f"PTY inheritance mutation check failed: {error}", file=sys.stderr)
        sys.exit(1)
