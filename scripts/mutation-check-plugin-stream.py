#!/usr/bin/env python3
"""Recompile private bridge mutants; assertion red, independent config green,
restored full bridge suite green and original SHA256. Never edit the worktree.
"""
from pathlib import Path
import sys
import hashlib
import json
import os
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["core/src/extension/stream.c", "core/src/extension/fd-stream.c", "core/src/extension/frame.c"]
INPUTS = SOURCES + ["core/src/extension/stream.h", "core/src/extension/frame.h",
                    "core/src/base/allocator.h", "core/tests/test_bridge_stream.c",
                    "core/CMakeLists.txt", "scripts/mutation-check-plugin-stream.py"]
MARK = "GT_PLUGIN_STREAM_MUTATION"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    tmp = ROOT / "tmp"
    tmp.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="plugin-stream-mutations-", dir=tmp))
    logs = work / "logs"
    logs.mkdir()
    env = {**os.environ, "TMPDIR": str(work), "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
           "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
    originals = {name: (ROOT / name).read_bytes() for name in SOURCES}
    if any(MARK.encode() in original for original in originals.values()):
        print("Residual bridge stream mutation",file=sys.stderr);raise SystemExit(2)
    hashes = {name: digest(ROOT / name) for name in INPUTS}
    commands = []

    def run(label, args, expected=0, assertion=False, timeout=120):
        args = [str(arg) for arg in args]
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-stream; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p = subprocess.run(["bash", "-c", shell, "plugin-stream", str(ROOT), str(timeout), *args],
                           cwd=ROOT, env=env, capture_output=True, text=True, timeout=timeout + 10)
        output = p.stdout + p.stderr
        (logs / (label + ".log")).write_text(output)
        commands.append({"label": label, "returncode": p.returncode})
        if p.returncode != expected or (assertion and "FAIL " not in output):
            raise RuntimeError(label + ": expected normal assertion failure" if assertion else label + ": failed\n" + output)
        if assertion and any(marker in output for marker in ("AddressSanitizer", "runtime error:", "UndefinedBehaviorSanitizer")):
            raise RuntimeError(label + ": sanitizer/crash is not an assertion red")
        return output

    try:
        source = work / "source"
        shutil.copytree(ROOT / "core", source, ignore=shutil.ignore_patterns("build", "build-*"))
        # Rebuild only the private C sources with no SSH dependencies. A real
        # unrelated config test links the same mutant library as the control.
        build = work / "build"
        run("configure", ["cmake", "-S", source, "-B", build,
                          "-DGHOSTERM_WITH_LIBSSH2=OFF", "-DCMAKE_BUILD_TYPE=Debug",
                          "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"])

        def rebuild(label):
            objects=[('ghosterm_core','src/extension/'+n+'.c') for n in ('stream','fd-stream','frame')]+[('test_bridge_stream','tests/test_bridge_stream.c')]
            binaries=['test_bridge_stream','test_config']
            for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
            for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
            run(label + "-build", ["cmake", "--build", build, "--target", "test_bridge_stream", "test_config", "-j", "2"])
            output=(logs/(label+'-build.log')).read_text()
            for owner,name in objects:
                if 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' not in output:raise RuntimeError('Missing actual compilation: '+label+' '+name)
            for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+name for name in binaries]]:
                if marker not in output:raise RuntimeError('Missing actual link: '+label+' '+marker)

        def suite(label):
            run(label, ["ctest", "--test-dir", build, "-R", "^bridge_stream_", "--output-on-failure"])

        def control(label):
            run(label + "-control", [build / "test_config", source / "tests/fixtures"])

        rebuild("baseline")
        suite("baseline")
        control("baseline")
        cases = [
            ("wait-eof", "fd-stream.c", '        if (!ready) {\n            return result(GT_BRIDGE_WAIT, 0, GT_OK);\n',
             '        if (!ready) {\n            return result(GT_BRIDGE_EOF, 0, GT_OK);\n', "pipe"),
            ("hup-tail", "fd-stream.c", "/* HUP/ERR still require a read: buffered tail precedes EOF or real error. */",
             "if (!writing && (fds[0].revents & POLLHUP)) return result(GT_BRIDGE_EOF, 0, GT_OK);", "pipe"),
            ("stop-wake", "fd-stream.c", '    while (write(s->wake[1], &byte, 1) < 0 && errno == EINTR) {\n    }\n',
             '    (void)s;\n    (void)byte;\n', "stop"),
            ("deadline-reset", "fd-stream.c", "int remaining = now < end ? (int)(end - now) : 0;",
             "end = now + ms; int remaining = now < end ? (int)(end - now) : 0;", "deadline"),
            ("zero-length", "frame.c", "if (!length || length > d->limit)", "if (length > d->limit)", "limits"),
            ("oversize", "frame.c", "if (!length || length > d->limit)", "if (!length)", "limits"),
            ("truncated-eof", "frame.c", '    if (!d->ready && d->header_used) {\n',
             '    if (false) {\n', "codec"),
            ("partial-offset", "frame.c", '        w->sent += r.count;\n',
             '        w->sent = w->length;\n', "lifecycle"),
            ("wait-consumes", "frame.c", '    } else if (r.state != GT_BRIDGE_WAIT) {\n',
             '    } else if (r.state == GT_BRIDGE_WAIT) {\n        w->sent++;\n    } else {\n', "lifecycle"),
            ("route-switch", "frame.c", "if (w->stream && w->stream != s)", "if (false)", "lifecycle"),
            ("early-release", "stream.c", '    if (r == GT_BRIDGE_RELEASED) {\n',
             '    if (r == GT_BRIDGE_RELEASED || r == GT_BRIDGE_DRAINING) {\n', "lifecycle"),
            ("stopped-write", "stream.c", 'gt_bridge_io_t gt__bridge_stream_write(gt_bridge_stream_t *s, const void *p, size_t n, int ms) {\n    if (!s || !p || !n || ms < 0) {\n        return error(GT_ERR_INVALID_ARG);\n    }\n    if (stopped(s)) {\n        return (gt_bridge_io_t){GT_BRIDGE_CLOSED, 0, GT_ERR_CANCELLED};\n    }\n    if (n > GT_BRIDGE_IO_QUANTUM) {\n',
             'gt_bridge_io_t gt__bridge_stream_write(gt_bridge_stream_t *s, const void *p, size_t n, int ms) {\n    if (!s || !p || !n || ms < 0) {\n        return error(GT_ERR_INVALID_ARG);\n    }\n    if (n > GT_BRIDGE_IO_QUANTUM) {\n', "lifecycle"),
            ("allocator-release", "frame.c", '    gt__plugin_memory_free(&a, d->bytes, d->limit);\n',
             '    /* missing payload release */\n', "allocation"),
        ]
        results = []
        for label, name, old, new, mode in cases:
            path = source / "src/extension" / name
            original = path.read_bytes()
            text = original.decode()
            if text.count(old) != 1:
                raise RuntimeError("Ambiguous mutation anchor: " + label)
            path.write_text(text.replace(old, new + " /* " + MARK + " */"))
            rebuild(label)
            run(label + "-target", [build / "test_bridge_stream", mode], expected=1, assertion=True)
            control(label)
            path.write_bytes(original)
            if digest(path) != hashlib.sha256(original).hexdigest():
                raise RuntimeError("Restore mismatch: " + label)
            rebuild(label + "-restored")
            suite(label + "-restored")
            results.append(label)
            print(label + ": compiled assertion red / same archive config green / restored 9 modes green / SHA256", flush=True)
        if any(digest(ROOT / name) != sha for name, sha in hashes.items()):
            raise RuntimeError("Worktree input changed during isolated mutations")
        result = {"accepted": True, "groups": results, "inputs_sha256": hashes,
                  "commands": commands, "sanitizers": ["address", "undefined"], "leak_sanitizer": False}
        (logs / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print("PLUGIN STREAM MUTATIONS PASS: " + str(len(results)) + " groups; logs: " + str(logs), flush=True)
    finally:
        # Preserve logs on failure; no binaries, descriptors, or working copies.
        shutil.rmtree(work / "source", ignore_errors=True)
        shutil.rmtree(work / "build", ignore_errors=True)
        if any((ROOT / name).read_bytes() != original for name, original in originals.items()):
            raise RuntimeError("Production source changed during mutations")


if __name__ == "__main__":
    def interrupted(signum, frame):
        raise KeyboardInterrupt("signal " + str(signum))
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
