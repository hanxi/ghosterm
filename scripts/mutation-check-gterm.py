#!/usr/bin/env python3
"""Rebuild isolated CLI mutations; assertion red / same archive config / restore."""
import hashlib, json, os, shutil, signal, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_GTERM_MUTATION'
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    file = ROOT / 'cli/gterm/main.c'; original = file.read_bytes()
    assert MARK.encode() not in original
    cases = [
        ('host-nul', ' || memchr(text, 0, used)', '', 'host-nul'),
        ('host-link', 'O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK', 'O_RDONLY | O_CLOEXEC | O_NONBLOCK', 'host-link'),
        ('directory-link', 'O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC', 'O_RDONLY | O_DIRECTORY | O_CLOEXEC', 'directory-link'),
        ('unknown-exit', 'info.error == GT_EXT_PACKAGE_UNKNOWN ? 3', 'info.error == GT_EXT_PACKAGE_UNKNOWN ? 4', 'unknown'),
        ('package-digest', 'json_string(metadata.package_digest);', 'json_string(metadata.manifest_digest);', 'compatible'),
        ('terminal-escape', 'c < 32 || c >= 127', 'c < 32 || c > 0x10ffff', 'compatible'),
        ('activation-exit', '"COMMAND_ACTIVATION_UNAVAILABLE", 4', '"COMMAND_ACTIVATION_UNAVAILABLE", 0', 'no-activation'),
    ]
    for _, old, _, _ in cases: assert original.decode().count(old) == 1, old
    work = Path(tempfile.mkdtemp(prefix='gterm-mutations-', dir=ROOT / 'tmp'))
    source = work / 'source'; logs = work / 'logs'; logs.mkdir()
    (source / 'cli').mkdir(parents=True)
    shutil.copytree(ROOT / 'cli/gterm', source / 'cli/gterm')
    # Only CLI is mutated. Core/SDK/vendor are read-only inputs to this build.
    for name in ['core', 'vendor', 'patches', 'scripts', 'packages']: (source / name).symlink_to(ROOT / name, target_is_directory=True)
    (source / 'tmp').mkdir()
    build = work / 'build'; commands = []
    inputs = {str(p.relative_to(ROOT)): sha(p) for folder in ['cli/gterm', 'core/src', 'core/include', 'packages/plugin-sdk'] for p in (ROOT / folder).rglob('*') if p.is_file()}
    inputs.update({str(p.relative_to(ROOT)): sha(p) for p in [Path(__file__), ROOT / 'scripts/test-gterm.py', ROOT / 'core/CMakeLists.txt']})
    env = dict(os.environ, TMPDIR=str(work), ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    def run(label, command, red=False):
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require gterm; shift; gt_timeout 300 "$@"'
        p = subprocess.run(['bash', '-c', shell, 'gterm', str(ROOT), *map(str, command)], cwd=ROOT, env=env, capture_output=True, text=True, timeout=310)
        output = p.stdout + p.stderr; (logs / (label + '.log')).write_text(output)
        commands.append(dict(label=label, returncode=p.returncode))
        if red: assert p.returncode == 1 and p.stdout.startswith('FAIL ') and not any(x in output for x in ['AddressSanitizer', 'runtime error:', 'Traceback']), label + output
        else: assert p.returncode == 0, label + output
        return output
    try:
        tools = work / 'tools'; shutil.copytree(ROOT / 'scripts/fixtures/plugin-sdk-tools', tools)
        run('tools', ['npm', 'ci', '--prefix', tools, '--ignore-scripts', '--no-audit', '--no-fund', '--offline', '--cache', ROOT / 'tmp/plugin-sdk-npm-cache'])
        sdk = work / 'sdk'
        run('sdk', ['node', tools / 'node_modules/typescript/bin/tsc', '-p', ROOT / 'packages/plugin-sdk/tsconfig.json', '--outDir', sdk])
        run('configure', ['cmake', '-S', source / 'cli/gterm', '-B', build, '-DGHOSTERM_BUILD_TESTS=ON', '-DGHOSTERM_PLUGIN_SDK_DIR=' + str(sdk), '-DGHOSTERM_WITH_PLUGIN_JS=ON', '-DGHOSTERM_WITH_LIBSSH2=OFF', '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        def rebuild(label):
            (build / 'CMakeFiles/gterm.dir/main.c.o').unlink(missing_ok=True)
            (build / 'gterm').unlink(missing_ok=True)
            out = run(label, ['cmake', '--build', build, '--target', 'gterm', 'test_config', '-j', '4'])
            assert 'main.c.o' in out and 'Linking C executable gterm' in out
        def test(label, case=None, red=False):
            run(label, ['python3', ROOT / 'scripts/test-gterm.py', '--binary', build / 'gterm', *(['--case', case] if case else [])], red)
        rebuild('baseline-build'); test('baseline')
        for label, old, new, case in cases:
            path = source / 'cli/gterm/main.c'
            try:
                path.write_text(original.decode().replace(old, new) + '\n/* ' + MARK + ' */\n')
                rebuild(label + '-build'); test(label + '-red', case, True)
                run(label + '-control', [build / 'core/test_config', ROOT / 'core/tests/fixtures'])
            finally:
                path.write_bytes(original); assert sha(path) == sha(file)
            rebuild(label + '-restore-build'); test(label + '-restore', case)
            print(label + ': assertion red / same archive config green / restored / SHA256', flush=True)
        test('final')
        for name, digest in inputs.items(): assert sha(ROOT / name) == digest, name
        (logs / 'result.json').write_text(json.dumps(dict(accepted=True, groups=[c[0] for c in cases], inputs_sha256=inputs, commands=commands, sanitizers=['address', 'undefined'], leak_sanitizer=False), indent=2) + '\n')
        print('GTERM MUTATIONS PASS:', logs, flush=True)
    except BaseException as error:
        (logs / 'failure.txt').write_text(repr(error)); raise

def stop(number, frame): raise KeyboardInterrupt(str(number))
if __name__ == '__main__':
    signal.signal(signal.SIGINT, stop); signal.signal(signal.SIGTERM, stop); main()
