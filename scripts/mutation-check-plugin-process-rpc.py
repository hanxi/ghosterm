#!/usr/bin/env python3
"""Compile child-RPC mutations only in an isolated repository tmp/ copy.

Exercises C Sessions and native child pipes with the Core fixtures.
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
SOURCE = 'core/src/extension/process-rpc.c'
MARK = 'GT_PROCESS_RPC_MUTATION'
CASES = [
    ('version', 'version == 1 &&', 'true &&', 'version'),
    ('sequence', 'sequence == p->sequence + 1 &&', 'sequence >= p->sequence &&', 'replay'),
    ('canonical-id', "|| text[0] == '0'", '|| false', 'leading-zero'),
    ('unknown-field', '|| d->nodes[1].count != count', '|| false', 'unknown-field'),
    ('backpressure', 'if (p->reply_size) {', 'if (false) {', 'backpressure'),
    ('message-limit', 'size <= GT_PROCESS_RPC_MESSAGE_MAX', 'true', 'oversize'),
    ('message-boundary', 'size <= GT_PROCESS_RPC_MESSAGE_MAX', 'size < GT_PROCESS_RPC_MESSAGE_MAX', 'message-boundary'),
    ('body-limit', 'd->nodes[id].bytes > GT_EXT_RPC_BODY_MAX', 'false', 'body-limit'),
    ('body-boundary', 'd->nodes[id].bytes > GT_EXT_RPC_BODY_MAX', 'd->nodes[id].bytes >= GT_EXT_RPC_BODY_MAX', 'body-boundary'),
    ('confirmation', 'result.confirmed ? "true" : "false"', '"false"', 'roundtrip'),
    ('unknown-state', '"failed", "cancelled", "unknown"', '"failed", "cancelled", "cancelled"', 'timeout'),
    ('cancel', 'status = gt_ext_session_cancel_request(p->session, &p->lease, slot->native);',
     'status = GT_EXT_RPC_OK;', 'cancel'),
    ('response-check', 'if (response) {\n        status = gt_ext_session_check_request(p->session, &p->lease, slot->native);',
     'if (response) {\n        status = GT_EXT_RPC_OK;', 'incoming-cancel'),
    ('interrupted-outcome', '"{\\\"kind\\\":\\\"unknown\\\",\\\"reason\\\":\\\"HANDLER_INTERRUPTED\\\"}";',
     '"{\\\"kind\\\":\\\"success\\\",\\\"value\\\":{}}";', 'incoming-cancel'),
    ('reference-domain', 'process_find(incoming ? p->incoming : p->outgoing, local)',
     'process_find(p->outgoing, local)', 'roundtrip'),
    ('reference-uniqueness', '++p->reference', '(p->reference = 1)', 'roundtrip'),
    ('method-filter', 'p->session, &p->lease, methods, count, &q',
     'p->session, &p->lease, NULL, 0, &q', 'method-filter'),
    ('live-access', 'gt_ext_access_t status = gt_ext_session_access(p->session, &p->lease, capability);',
     'gt_ext_access_t status = GT_EXT_ACCESS_ALLOWED;', 'access'),
    ('source-exclusivity', '|| session->sources ||', '|| false ||', 'lifetime'),
    ('prior-owner', '!gt__bridge_session_runtime_admissible(session->inner)', 'false', 'adoption'),
    ('immediate-revoke', 'p->failed = true;\n        gt__process_rpc_revoke(p);',
     'p->failed = true;', 'version'),
    ('forget-slot', 'status == GT_EXT_RPC_OK && (response || !strcmp(op, "forget"))',
     'status == GT_EXT_RPC_OK && response', 'reuse'),
    ('reservation-release', 'p->session->sources--;', '(void)p->session;', 'lifetime'),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append')
    args = parser.parse_args()
    if args.case and set(args.case) - {c[0] for c in CASES}:
        parser.error('unknown mutation group')
    cases = [c for c in CASES if not args.case or c[0] in args.case]
    data = (ROOT / SOURCE).read_text()
    assert MARK not in data
    for label, old, _, _ in cases:
        assert data.count(old) == (2 if label == 'reference-uniqueness' else 1), label
    work = Path(tempfile.mkdtemp(prefix='process-rpc-mutation-', dir=os.environ['TMPDIR']))
    source, build, logs = work / 'source', work / 'build', work / 'logs'
    logs.mkdir()
    env = {**os.environ, 'TMPDIR': str(work), 'TMP': str(work), 'TEMP': str(work), 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    commands = []

    def run(label, command, red=False):
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require process-rpc; shift; gt_timeout 180 "$@"'
        p = subprocess.run(['bash', '-c', shell, 'process-rpc', str(ROOT), *map(str, command)],
                           cwd=ROOT, env=env, capture_output=True, text=True, timeout=190)
        output = p.stdout + p.stderr
        (logs / (label + '.log')).write_text(output)
        commands.append(dict(label=label, returncode=p.returncode, assertion=red))
        if red:
            assert p.returncode == 1 and re.search(r'^FAIL ', p.stderr, re.M) and not any(
                text in output for text in ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer')), label + '\n' + output
        else:
            assert p.returncode == 0, label + '\n' + output
        return output

    try:
        for folder in ('core',):
            shutil.copytree(ROOT / folder, source / folder,
                            ignore=shutil.ignore_patterns('build', 'build-*', '__pycache__'))
        (source / 'tmp').mkdir()
        inputs = {str(p.relative_to(source)): sha(p) for folder in ('core',)
                  for p in (source / folder).rglob('*') if p.is_file()}
        run('configure', ['cmake', '-S', source / 'core', '-B', build,
                          '-DGHOSTERM_WITH_LIBSSH2=OFF', '-DGHOSTERM_WITH_PLUGIN_LUA=OFF',
                          '-DGHOSTERM_BUILD_TESTS=ON', '-DCMAKE_BUILD_TYPE=Debug',
                          '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])

        def rebuild(label):
            (build / 'CMakeFiles/ghosterm_core.dir/src/extension/process-rpc.c.o').unlink(missing_ok=True)
            for target in ('test_process_rpc', 'test_config', 'libghosterm_core.a'):
                (build / target).unlink(missing_ok=True)
            output = run(label + '-build', ['cmake', '--build', build, '--target',
                                           'test_process_rpc', 'test_config', '-j', '4'])
            assert 'src/extension/process-rpc.c.o' in output and 'Linking C static library' in output
            assert 'Linking C executable test_process_rpc' in output

        def suite(label):
            run(label, ['ctest', '--test-dir', build, '-R', '^plugin_process_rpc_', '--output-on-failure'])


        rebuild('baseline')
        suite('baseline')
        for label, old, new, mode in cases:
            path = source / SOURCE
            original = path.read_bytes()
            try:
                path.write_text(original.decode().replace(old, new) + '\n/* ' + MARK + ' */\n')
                rebuild(label)
                run(label + '-red', [build / 'test_process_rpc', mode, source / 'tmp'], True)
                run(label + '-control', [build / 'test_config', source / 'core/tests/fixtures'])
            finally:
                path.write_bytes(original)
                assert sha(path) == hashlib.sha256(original).hexdigest()
            rebuild(label + '-restored')
            run(label + '-restored', [build / 'test_process_rpc', mode, source / 'tmp'])
            print(label + ': compiled assertion red / same archive configuration green / restored green / SHA256', flush=True)
        suite('final')
        for name, digest in inputs.items():
            assert sha(source / name) == digest and sha(ROOT / name) == digest, name
        (logs / 'result.json').write_text(json.dumps(dict(accepted=True, groups=[c[0] for c in cases],
            inputs_sha256=inputs, commands=commands,
            sanitizers=['address', 'undefined'], leak_sanitizer=False), indent=2) + '\n')
        print('PROCESS RPC MUTATIONS PASS: ' + str(logs), flush=True)
    except BaseException as error:
        (logs / 'failure.txt').write_text(repr(error))
        raise


def stop(number, frame):
    raise KeyboardInterrupt('signal ' + str(number))


if __name__ == '__main__':
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, stop)
    main()
