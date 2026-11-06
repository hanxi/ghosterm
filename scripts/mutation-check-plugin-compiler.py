#!/usr/bin/env python3
"""Compile isolated pinned engine mutations; observe unsafe switch backfills.

The boundary observation aborts before the destructive write, so a normal test
assertion can identify it. Actual package-corpus OOM sweeps run without this hook.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_PLUGIN_COMPILER_MUTATION'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True,
                        help='current Core build with JS ON, SSH OFF and ASan/UBSan')
    args = parser.parse_args()
    build = args.build.resolve()
    engine = build / 'quickjs-ng-0.18.0'
    test = ROOT / 'core/tests/test_plugin_compiler_oom.c'
    for path in [test, engine / 'quickjs.c', engine / 'cutils.h']:
        if MARK in path.read_text():
            raise SystemExit('residual compiler mutation: ' + str(path))
    cmake = (ROOT / 'core/cmake/PluginJS.cmake').read_text()
    for name, key in [('quickjs.c', '_gt_js_patched_sha'), ('cutils.h', '_gt_js_cutils_patched_sha')]:
        expected = re.search(r'set\(' + key + r' "([a-f0-9]{64})"\)', cmake).group(1)
        assert sha(engine / name) == expected, name
    cache = (build / 'CMakeCache.txt').read_text()
    assert 'GHOSTERM_WITH_LIBSSH2:BOOL=OFF' in cache
    assert 'GHOSTERM_WITH_PLUGIN_JS:BOOL=ON' in cache
    assert '-fsanitize=address,undefined' in cache
    inputs = [test, ROOT / 'core/cmake/PluginJS.cmake', Path(__file__),
              ROOT / 'core/tests/test_config.c', build / 'libghosterm_core.a']
    inputs += [p for p in engine.iterdir() if p.is_file()]
    inputs += [p for p in (ROOT / 'patches/quickjs-ng').rglob('*') if p.is_file()]
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    work = Path(tempfile.mkdtemp(prefix='plugin-compiler-', dir=ROOT / 'tmp'))
    source = work / 'source'
    shutil.copytree(engine, source)
    logs = work / 'logs'
    logs.mkdir()
    commands = []
    env = dict(os.environ, TMPDIR=str(work), ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

    def run(label, cmd, red=False, budget=180):
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require compiler; shift; gt_timeout "$@"'
        result = subprocess.run(['bash', '-c', shell, 'compiler', str(ROOT), str(budget),
                                 *map(str, cmd)], cwd=ROOT, env=env, capture_output=True,
                                text=True, timeout=budget + 10)
        out = result.stdout + result.stderr
        (logs / (label + '.log')).write_text(out)
        commands.append(dict(label=label, returncode=result.returncode, assertion=red))
        good = result.returncode == 0
        if red:
            good = result.returncode == 1 and result.stderr.startswith('FAIL ')
            good = good and not any(s in out for s in ['AddressSanitizer', 'runtime error:',
                                                       'UndefinedBehaviorSanitizer'])
        if not good:
            raise RuntimeError(label + '\n' + out)
        return out

    try:
        original = {name: (source / name).read_text() for name in ['quickjs.c', 'cutils.h']}
        anchor = '                /* Ugly patch for the `default` label, shameful and risky */'
        assert original['quickjs.c'].count(anchor) == 1
        observed = ('unsigned gt_test_switch_backfills, gt_test_force_switch_oom, '
                    'gt_test_switch_oom_injected;\n') + original['quickjs.c'].replace(
            anchor, '                if (dbuf_error(&s->cur_func->byte_code)) {\n'
            '                    gt_test_switch_backfills++;\n'
            '                    goto fail;\n                }\n' + anchor)
        emission = '                    default_label_pos = s->cur_func->byte_code.size - 4;'
        assert observed.count(emission) == 1
        observed = observed.replace(emission, emission + '\n'
            '                    if (gt_test_force_switch_oom) {\n'
            '                        dbuf_set_error(&s->cur_func->byte_code);\n'
            '                        gt_test_switch_oom_injected++;\n'
            '                        gt_test_force_switch_oom = 0;\n                    }')
        (source / 'quickjs.c').write_text(observed)
        flags = ['-O2', '-g', '-DNDEBUG', '-std=c11', '-D_GNU_SOURCE', '-fwrapv',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                 '-fno-omit-frame-pointer', '-I' + str(source)]
        for name in ['dtoa', 'libregexp', 'libunicode']:
            run('support-' + name, ['cc', *flags, '-c', source / (name + '.c'),
                                    '-o', work / (name + '.o')])

        def rebuild(label):
            # Actual object/archive/executable removal prevents stale timestamp reuse.
            for name in ['quickjs.o', 'libengine.a', 'test', 'control']:
                (work / name).unlink(missing_ok=True)
            run(label + '-compile', ['cc', *flags, '-c', source / 'quickjs.c',
                                     '-o', work / 'quickjs.o'])
            run(label + '-archive', ['ar', 'rcs', work / 'libengine.a',
                                     *[work / (n + '.o') for n in
                                       ['quickjs', 'dtoa', 'libregexp', 'libunicode']]])
            run(label + '-link', ['cc', *flags, '-DGT_COMPILER_SWITCH_BOUNDARY',
                                  test, work / 'libengine.a', '-lm', '-o', work / 'test'])
            run(label + '-control-link', ['cc', *flags, '-I' + str(ROOT / 'core/include'),
                                          ROOT / 'core/tests/test_config.c',
                                          build / 'libghosterm_core.a', work / 'libengine.a',
                                          '-lm', '-o', work / 'control'])

        rebuild('baseline')
        run('baseline-buffer', [work / 'test', 'buffer'])
        run('baseline-sweep', [work / 'test', 'sweep'])
        run('baseline-switch', [work / 'test', 'switch'])
        cases = []
        for signature in ['dbuf_putc(DynBuf *s, uint8_t val)', 'dbuf_put_u16(DynBuf *s, uint16_t val)',
                          'dbuf_put_u32(DynBuf *s, uint32_t val)', 'dbuf_put_u64(DynBuf *s, uint64_t val)',
                          'dbuf_claim(DynBuf *s, size_t len)', 'dbuf_put(DynBuf *s, const void *data, size_t len)',
                          'dbuf_put_self(DynBuf *s, size_t offset, size_t len)']:
            old = 'static inline int ' + signature + '\n{\n    if (unlikely(s->error))\n        return -1;\n'
            cases.append((signature.split('(')[0], 'cutils.h', old,
                          old.replace('if (unlikely(s->error))', 'if (false && s->error)'), 'buffer'))
        old = ('            /* A failed dummy-label emission has no operand to backfill. */\n'
               '            if (dbuf_error(&s->cur_func->byte_code)) {')
        cases.append(('switch-backfill', 'quickjs.c', old,
                      old.replace('if (dbuf_error', 'if (false && dbuf_error'), 'switch'))
        for label, name, old, new, mode in cases:
            path = source / name
            clean = path.read_bytes()
            assert clean.decode().count(old) == 1, label
            try:
                path.write_text(clean.decode().replace(old, new) + '\n/* ' + MARK + ' */\n')
                rebuild(label)
                run(label + '-target', [work / 'test', mode], red=True)
                run(label + '-control', [work / 'control', ROOT / 'core/tests/fixtures'])
            finally:
                path.write_bytes(clean)
                assert sha(path) == hashlib.sha256(clean).hexdigest()
            rebuild(label + '-restored')
            run(label + '-restored', [work / 'test', mode])
            print(label + ': compiled assertion red / same archive config green / restored / SHA256', flush=True)
        run('final-buffer', [work / 'test', 'buffer'])
        run('final-sweep', [work / 'test', 'sweep'])
        run('final-switch', [work / 'test', 'switch'])
        for name, digest in hashes.items():
            assert sha(ROOT / name) == digest, name
        (logs / 'result.json').write_text(json.dumps(dict(
            accepted=True, groups=[c[0] for c in cases], commands=commands,
            inputs_sha256=hashes, sanitizers=['address', 'undefined'], leak_sanitizer=False,
            scope='pinned compiler plus observed switch backfill boundary; no package/App activation'
        ), indent=2) + '\n')
        shutil.rmtree(source)
        print('COMPILER MUTATIONS PASS; logs: ' + str(logs), flush=True)
    except BaseException as error:
        (logs / 'failure.txt').write_text(repr(error) + '\n')
        raise


def stop(number, _frame):
    raise KeyboardInterrupt('signal ' + str(number))


if __name__ == '__main__':
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    main()
