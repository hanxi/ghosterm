#!/usr/bin/env python3
"""Pinned regexp atom OOM: actual compiled boundary red, literal control green."""
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
MARK = 'GT_PLUGIN_REGEXP_MUTATION'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    engine = args.build.resolve() / 'quickjs-ng-0.18.0'
    test = ROOT / 'core/tests/test_plugin_regexp_oom.c'
    cmake = ROOT / 'core/cmake/PluginJS.cmake'
    for name, key in [('libregexp.c', '_gt_js_regexp_patched_sha'),
                      ('cutils.h', '_gt_js_cutils_patched_sha')]:
        expected = re.search(r'set\(' + key + r' "([a-f0-9]{64})"\)', cmake.read_text()).group(1)
        assert sha(engine / name) == expected, name
    inputs = [test, cmake, Path(__file__), *[p for p in engine.iterdir() if p.is_file()],
              *[p for p in (ROOT/'patches/quickjs-ng').rglob('*') if p.is_file()]]
    for p in [test, engine/'libregexp.c', engine/'cutils.h']:
        if MARK in p.read_text():
            raise SystemExit('residual regexp mutation: ' + str(p))
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    work = Path(tempfile.mkdtemp(prefix='plugin-regexp-',dir=ROOT/'tmp'))
    source = work/'source'
    shutil.copytree(engine, source)
    logs = work/'logs'
    logs.mkdir()
    commands = []
    env = dict(os.environ,TMPDIR=str(work),ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

    def run(label,cmd,red=False):
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require regexp; shift; gt_timeout "$@"'
        p = subprocess.run(['bash','-c',shell,'regexp',str(ROOT),'120',*map(str,cmd)],
                           cwd=ROOT,env=env,capture_output=True,text=True,timeout=130)
        out = p.stdout+p.stderr
        (logs/(label+'.log')).write_text(out)
        commands.append(dict(label=label,returncode=p.returncode,assertion=red))
        ok = p.returncode == 0
        if red:
            ok = p.returncode == 1 and re.search(r'^FAIL plugin-regexp:\d+ ', p.stderr, re.M)
            ok = ok and not any(s in out for s in ['AddressSanitizer','runtime error:',
                                                  'UndefinedBehaviorSanitizer'])
        if not ok:
            raise RuntimeError(label+'\n'+out)

    try:
        path = source/'libregexp.c'
        original = path.read_text()
        guard = '    /* Do not inspect an atom whose emission failed or was truncated. */'
        group_guard = '        parse_capture:\n'
        scan = '                add_zero_advance_check =\n'
        assert original.count(guard) == original.count(scan) == 1
        observed = ('unsigned gt_test_force_regexp_oom, gt_test_force_group_oom, gt_test_regexp_injected, '
                    'gt_test_regexp_scans;\n') + original.replace(guard,
                    '    if (gt_test_force_regexp_oom) {\n'
                    '        dbuf_set_error(&s->byte_code);\n'
                    '        gt_test_regexp_injected++;\n'
                    '        gt_test_force_regexp_oom = 0;\n    }\n'+guard)
        assert observed.count(group_guard) == 1
        observed = observed.replace(group_guard, group_guard +
                    '            if (gt_test_force_group_oom) {\n'
                    '                dbuf_set_error(&s->group_names);\n'
                    '                gt_test_regexp_injected++;\n'
                    '                gt_test_force_group_oom = 0;\n            }\n')
        observed = observed.replace(scan,
                    '                if (dbuf_error(&s->byte_code)) {\n'
                    '                    gt_test_regexp_scans++;\n'
                    '                    goto out_of_memory;\n                }\n'+scan)
        flags = ['-std=c11','-O2','-g','-DNDEBUG','-D_GNU_SOURCE','-fwrapv',
                 '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                 '-fno-omit-frame-pointer','-I'+str(source)]
        run('unicode-build',['cc',*flags,'-c',source/'libunicode.c','-o',work/'unicode.o'])

        def rebuild(label):
            for name in ['regexp.o','libregexp.a','test']:
                (work/name).unlink(missing_ok=True)
            run(label+'-compile',['cc',*flags,'-c',path,'-o',work/'regexp.o'])
            run(label+'-archive',['ar','rcs',work/'libregexp.a',work/'regexp.o',work/'unicode.o'])
            run(label+'-link',['cc',*flags,'-DGT_REGEXP_BOUNDARY',test,work/'libregexp.a',
                              '-lm','-o',work/'test'])

        path.write_text(observed)
        rebuild('baseline')
        for mode in ['sweep','boundary','group-boundary','control']:
            run('baseline-'+mode,[work/'test',mode])
        old = guard+'\n    if (dbuf_error(&s->byte_code))'
        assert observed.count(old) == 1
        try:
            path.write_text(observed.replace(old,guard+'\n    if (false && dbuf_error(&s->byte_code))')
                            +'\n/* '+MARK+' */\n')
            rebuild('atom-error')
            run('atom-error-target',[work/'test','boundary'],red=True)
            run('atom-error-literal-control',[work/'test','control'])
        finally:
            path.write_text(observed)
            assert sha(path) == hashlib.sha256(observed.encode()).hexdigest()
        old_group = '            if (dbuf_error(&s->group_names))'
        assert observed.count(old_group) == 1
        try:
            path.write_text(observed.replace(old_group,
                            '            if (false && dbuf_error(&s->group_names))')
                            +'\n/* '+MARK+' */\n')
            rebuild('group-error')
            run('group-error-target',[work/'test','group-boundary'],red=True)
            run('group-error-literal-control',[work/'test','control'])
        finally:
            path.write_text(observed)
            assert sha(path) == hashlib.sha256(observed.encode()).hexdigest()
        rebuild('restored')
        for mode in ['sweep','boundary','group-boundary','control']:
            run('restored-'+mode,[work/'test',mode])
        for name,digest in hashes.items():
            assert sha(ROOT/name) == digest, name
        (logs/'result.json').write_text(json.dumps(dict(
            accepted=True,groups=['atom-error-before-quantifier','group-error-before-capture'],commands=commands,
            inputs_sha256=hashes,sanitizers=['address','undefined'],leak_sanitizer=False,
            scope='raw pinned regexp compiler; deterministic emission error/scan observation; '
                  'same archive independent literal configuration; no JS/package/App activation'
        ),indent=2)+'\n')
        shutil.rmtree(source)
        print('REGEXP MUTATION PASS; logs: '+str(logs),flush=True)
    except BaseException as error:
        (logs/'failure.txt').write_text(repr(error)+'\n')
        raise


def stop(number,_frame):
    raise KeyboardInterrupt('signal '+str(number))


if __name__ == '__main__':
    signal.signal(signal.SIGINT,stop)
    signal.signal(signal.SIGTERM,stop)
    main()
