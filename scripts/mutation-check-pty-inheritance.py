#!/usr/bin/env python3
"""Prove PTY descriptor and signal-mask isolation in a private core copy.

--apple also checks native window -> PTY -> system ssh -> tmux, requiring the
GHOSTERM_SSH_* environment of an isolated loopback sshd.
"""
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
    result = bounded(command, 180)
    if result.returncode:
        raise RuntimeError("build/tool failed:\n" + result.stdout)


def bounded(command, seconds):
    return subprocess.run(["bash", "-c",
        '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require pty-inheritance; shift; gt_timeout "$@"',
        "pty-inheritance", str(ROOT), str(seconds), *map(str, command)],
        stdout=subprocess.PIPE, text=True, stderr=subprocess.STDOUT)


def test(build, name, expected=0):
    result = bounded(["ctest", "--test-dir", str(build), "-R", f"^{name}$",
                      "--output-on-failure"], 30)
    if result.returncode != expected:
        sys.stdout.write(result.stdout)
        raise RuntimeError(f"{name}: expected ctest exit {expected}, got {result.returncode}")


def interrupted(signum, frame):
    raise InterruptedError(f"signal {signum}; restoring private core copy")


def main():
    apple = "--apple" in sys.argv[1:]
    if apple and not all(os.environ.get(k) for k in
                         ("GHOSTERM_SSH_HOST", "GHOSTERM_SSH_PORT", "GHOSTERM_SSH_USER", "GHOSTERM_SSH_KEY")):
        raise RuntimeError("--apple requires isolated loopback sshd credentials; no SKIP")
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
            core = work / "core"
            build = core / "build"
            shutil.copytree(ROOT / "core", core,
                            ignore=shutil.ignore_patterns("build", "build-*", ".build"))
            source = core / "src/io/pty_posix.c"
            run(["cmake", "-S", str(core), "-B", str(build)])

            if apple:
                (work / "apple").mkdir()
                for package in ("GhostermApp", "GhostermCore", "GhostermTerminal"):
                    shutil.copytree(ROOT / "apple" / package, work / "apple" / package,
                                    symlinks=True, ignore=shutil.ignore_patterns(".build", ".swiftpm"))
                for name in ("build", "terminal", "assets"):
                    (work / name).symlink_to(ROOT / name, target_is_directory=True)

            def apple_test(red=False, control=False):
                if not apple: return
                result = bounded(["xcrun", "swift", "test", "--package-path", work / "apple/GhostermApp",
                                  "--filter", "HostListTests" if control else "TmuxResizeTests"], 240)
                out = result.stdout
                targets = ("testLocalTmuxResizesWithoutInput", "testLocalShellSSHResizesRemoteTmuxWithoutInput")
                valid = (result.returncode == 1 and "XCTAssert" in out and "Build complete" in out
                         and all(any(name in line and "failed (" in line for line in out.splitlines())
                                 for name in targets)) if red else (
                    result.returncode == 0 and "Test Suite" in out and "skipped" not in out)
                if not valid: raise RuntimeError("native resize expected " + ("red" if red else "green") + ":\n" + out)
                if red: print("PASS native resize: local tmux and system-SSH tmux both reject inherited mask", flush=True)

            def compile_tests():
                # Force a fresh object/archive even with whole-second mtimes.
                for obj in build.rglob("pty_posix.c.o"): obj.unlink()
                run(["cmake", "--build", str(build), "--target",
                     "test_pty_inheritance", "test_api", "-j", "2"])

            compile_tests()
            test(build, "pty_inheritance")
            test(build, "api_smoke")
            apple_test()
            print("PASS baseline: PTY isolation and API control", flush=True)
            variants = (
                ("owned descriptors lose CLOEXEC", "descriptor_flags | FD_CLOEXEC",
                 "descriptor_flags & ~FD_CLOEXEC"),
                ("child leaks caller descriptors", "fd < limit; fd++",
                 "fd < 3 && fd < limit; fd++"),
                ("child inherits blocked signals", "sigprocmask(SIG_SETMASK, &child_signals, NULL)",
                 "sigprocmask(SIG_BLOCK, &child_signals, NULL)"),
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
                    if "signals" in label:
                        apple_test(red=True)
                        apple_test(control=True)
                    print(f"PASS mutation/{label}: target red, control green", flush=True)
                finally:
                    source.write_bytes(original)
                    if hashlib.sha256(source.read_bytes()).digest() != digest:
                        raise RuntimeError("private PTY source rollback SHA256 mismatch")
                compile_tests()
                test(build, "pty_inheritance")
                test(build, "api_smoke")
                if "signals" in label: apple_test()
            print("PASS 3 mutations and rollback; repository sources unchanged")
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
