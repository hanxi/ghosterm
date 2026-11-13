#!/usr/bin/env python3
"""Candidate Node SDK/stdio adapter: real C Sessions plus isolated fault tests.

Use --mutate for forced TS emission and real worker faults; every target must
fail by an ordinary assertion, never a compiler error/crash/sanitizer/timeout.
Only an explicitly supplied installed Node and locked developer tools are used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
PORT = 'packages/plugin-sdk/node/port.ts'
MESSAGE = 'packages/plugin-sdk/node/message.ts'
PIPE = 'packages/plugin-sdk/node/runtime/pipe.mjs'
WORKER = 'packages/plugin-sdk/node/runtime/pipe-worker.mjs'
MARK = 'GT_NODE_PORT_MUTATION'
CASES = [
    ('reply-id', PORT, 'reply.id !== id', 'false', 'unit:replyIdentity'),
    ('reply-version', PORT, 'reply.v !== 1', 'false', 'unit:replyIdentity'),
    ('reply-fields', PORT, 'Object.keys(value).sort().join(",") !== [...expected].sort().join(",")', 'Object.keys(value).length < 0 && expected.length < 0', 'unit:replyIdentity'),
    ('live-access', PORT, 'return reply.access as CapabilityAccess;', 'return "allowed";', 'unit:access'),
    ('reference-reuse', PORT, 'compareUint64(ref, this.#reference) <= 0', 'compareUint64(ref, this.#reference) < 0', 'unit:reuse'),
    ('capacity-boundary', PORT, 'if (this.#handles.size >= 64)', 'if (this.#handles.size >= 63)', 'unit:capacity'),
    ('forget', PORT, 'if (s === "ok") this.#handles.delete(handle);', 'if (false) this.#handles.delete(handle);', 'unit:roundtrip'),
    ('respond', PORT, 'if (s === "ok") this.#tickets.delete(ticket);', 'if (false) this.#tickets.delete(ticket);', 'unit:interrupted'),
    ('interrupted-field', PORT, 'op === "respond" && reply.interrupted !== undefined', 'false', 'unit:interrupted'),
    ('confirmation', PORT, '(state === "succeeded" && !r.confirmed)', 'false', 'unit:resultSchema'),
    ('cancel', PORT, 'this.#call("cancel", { ref: handle })', 'this.#call("check", { ref: handle })', 'unit:cancel'),
    ('reference-domain', PORT, 'if (!this.#handles.has(handle)) return fail("CHILD_HANDLE");', 'if (false) return fail("CHILD_HANDLE");', 'unit:references'),
    ('no-replay-after-failure', PORT, 'this.#broken || this.#sequence ===', 'false || this.#sequence ===', 'unit:ambiguous'),
    ('reentry', PORT, 'if (this.#broken) fail("CHILD_LINK_CLOSED");', 'if (false) fail("CHILD_LINK_CLOSED");', 'unit:reentry'),
    ('decoded-duplicates', MESSAGE, 'if (seen.has(key))', 'if (false && seen.has(key))', 'unit:duplicate'),
    ('message-limit', MESSAGE, 'utf8Bytes(text) > 8192', 'utf8Bytes(text) > 8193', 'unit:lexical'),
    ('strict-utf8', PIPE, 'fatal: true, ignoreBOM: true', 'fatal: false, ignoreBOM: true', 'pipe:utf8'),
    ('bom', PIPE, 'fatal: true, ignoreBOM: true', 'fatal: true, ignoreBOM: false', 'pipe:bom'),
    ('eof-wakeup', WORKER, "input.on('end', fail);", "input.on('end', () => {});", 'pipe:eof-body'),
]
PROBES = ['fragmented', 'utf8', 'bom', 'duplicate', 'prefix-zero', 'prefix-limit', 'eof-header', 'eof-body', 'timeout']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--node', type=Path, required=True)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--case', action='append')
    args = parser.parse_args()
    if args.case and (not args.mutate or set(args.case) - {c[0] for c in CASES}):
        parser.error('--case requires --mutate and valid group names')
    node, tools = args.node.resolve(strict=True), args.tools.resolve(strict=True)
    cases = [c for c in CASES if not args.case or c[0] in args.case]
    for label, name, old, _, _ in cases:
        text = (ROOT / name).read_text()
        assert MARK not in text and text.count(old) == 1, label
    work = Path(tempfile.mkdtemp(prefix='node-port-', dir=ROOT / 'tmp'))
    source, build, logs = work / 'source', work / 'core-build', work / 'logs'
    logs.mkdir()
    commands = []
    env = {**os.environ, 'TMPDIR': str(work), 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    env.pop('NODE_OPTIONS', None)

    def record(label, p, red):
        output = p.stdout + p.stderr
        (logs / (label + '.log')).write_text(output)
        commands.append(dict(label=label, returncode=p.returncode, assertion=red))
        if red:
            assert p.returncode == 1 and re.search(r'^ASSERT ', p.stderr, re.M) and not any(
                t in output for t in ['AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:', 'ERROR ']), label + '\n' + output
        else:
            assert p.returncode == 0, label + '\n' + output
        return output

    def run(label, command, red=False):
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require node-port; shift; gt_timeout 180 "$@"'
        p = subprocess.run(['bash', '-c', shell, 'node-port', str(ROOT), *map(str, command)],
                           cwd=ROOT, env=env, capture_output=True, text=True, timeout=190)
        return record(label, p, red)

    try:
        def ignore(directory, names):
            return [name for name in names if name == '__pycache__' or
                    ((Path(directory) / name).is_dir() and (name == 'build' or name.startswith('build-')))]

        for folder in ['core', 'packages/plugin-sdk', 'scripts']:
            shutil.copytree(ROOT / folder, source / folder, ignore=ignore)
        shutil.copyfile(ROOT / 'LICENSE', source / 'LICENSE')
        (source / 'tmp').mkdir()
        inputs = {str(p.relative_to(source)): sha(p) for folder in ['core', 'packages/plugin-sdk', 'scripts']
                  for p in (source / folder).rglob('*') if p.is_file()}
        inputs['LICENSE'] = sha(source / 'LICENSE')
        node_digest = sha(node)
        version = run('node-version', [node, '--version']).strip()
        run('configure', ['cmake', '-S', source / 'core', '-B', build,
                          '-DGHOSTERM_WITH_PLUGIN_JS=OFF', '-DGHOSTERM_WITH_LIBSSH2=OFF',
                          '-DGHOSTERM_BUILD_TESTS=ON', '-DCMAKE_BUILD_TYPE=Debug',
                          '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        run('native-build', ['cmake', '--build', build, '--target', 'test_process_rpc', 'test_config', '-j', '4'])
        bundle = source / 'tmp/sdk'

        def compile_sdk(label):
            if bundle.exists():
                shutil.rmtree(bundle)
            run(label + '-compile', [node, source / 'scripts/build-plugin-node-sdk.mjs',
                                     '--tools', tools, '--out', bundle])
            meta = json.loads((bundle / 'node-sdk-build.json').read_text())
            assert meta['candidate'] and meta['sdkApiVersion'] == '1.0.0'
            for member in meta['members']:
                p = bundle / member['name']
                assert p.stat().st_size == member['bytes'] and sha(p) == member['sha256']
            for name in ['pipe.mjs', 'pipe-worker.mjs']:
                assert (bundle / 'node/runtime' / name).read_bytes() == (source / 'packages/plugin-sdk/node/runtime' / name).read_bytes()
                run(label + '-syntax-' + name, [node, '--check', bundle / 'node/runtime' / name])

        def pipe_probe(label, mode, red=False):
            process = subprocess.Popen([str(node), str(source / 'scripts/fixtures/plugin-node-pipe-probe.mjs'), str(bundle), mode],
                cwd=source / 'tmp', env={'PATH': '/usr/bin:/bin', 'TMPDIR': str(source / 'tmp')},
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0, start_new_session=True)
            deadline = time.monotonic() + 10

            def exact(n):
                data = b''
                while len(data) < n:
                    left = deadline - time.monotonic()
                    if left <= 0 or not select.select([process.stdout], [], [], left)[0]:
                        raise RuntimeError(label + ': peer request timeout')
                    part = os.read(process.stdout.fileno(), n - len(data))
                    if not part:
                        raise RuntimeError(label + ': missing peer request')
                    data += part
                return data
            try:
                size = struct.unpack('>I', exact(4))[0]
                assert 0 < size <= 8192
                request = json.loads(exact(size))
                assert request == dict(v=1, id='1', op='access', capability='ui.form')
                body = b'{"v":1,"id":"1","status":"ok","access":"allowed"}'
                if mode == 'utf8':
                    body = b'{"text":"\xc0\x80"}'
                elif mode == 'bom':
                    body = b'\xef\xbb\xbf' + body
                elif mode == 'duplicate':
                    body = b'{"x":1,"\\u0078":2}'
                packet = struct.pack('>I', len(body)) + body
                if mode == 'prefix-zero':
                    packet = b'\0\0\0\0'
                elif mode == 'prefix-limit':
                    packet = struct.pack('>I', 8193)
                elif mode == 'eof-header':
                    packet = packet[:2]
                elif mode == 'eof-body':
                    packet = packet[:-2]
                if mode != 'timeout':
                    if mode == 'fragmented':
                        for byte in packet:
                            os.write(process.stdin.fileno(), bytes([byte]))
                    else:
                        os.write(process.stdin.fileno(), packet)
                    if mode.startswith('eof'):
                        process.stdin.close()
                process.wait(timeout=12)
                tail, errors = process.stdout.read(), process.stderr.read()
                assert not tail, label + ': unexpected replay/output'
                return record(label, subprocess.CompletedProcess([], process.returncode, '', errors.decode()), red)
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                for handle in [process.stdin, process.stdout, process.stderr]:
                    handle.close()

        def unit(label, case='all', red=False):
            run(label, [node, source / 'scripts/fixtures/plugin-node-port-cases.mjs', bundle, case], red)

        def integration(label):
            entry = source / 'tmp/entry.mjs'
            entry.write_text('import * as sdk from ' + json.dumps((bundle / 'portable/index.js').as_uri()) + ';\n' +
                'import {NodeNativePort} from ' + json.dumps((bundle / 'node/port.js').as_uri()) + ';\n' +
                'import {NodeStdioPipe} from ' + json.dumps((bundle / 'node/runtime/pipe.mjs').as_uri()) + ';\n' +
                'import {run} from ' + json.dumps((source / 'scripts/fixtures/plugin-node-sdk-peer.mjs').as_uri()) + ';\n' +
                'await run({sdk,NodeNativePort,NodeStdioPipe});\n')
            for mode in ['pipe-node-sdk', 'pipe-node-sdk-cancel']:
                run(label + '-' + mode, [build / 'test_process_rpc', mode, source / 'tmp', node, entry])

        def suite(label):
            unit(label + '-unit')
            for mode in PROBES:
                pipe_probe(label + '-pipe-' + mode, mode)
            integration(label)
            run(label + '-core', ['ctest', '--test-dir', build, '-R', '^plugin_process_rpc_', '--output-on-failure'])

        compile_sdk('baseline')
        suite('baseline')
        if args.mutate:
            for label, name, old, new, target in cases:
                p = source / name
                original = p.read_bytes()
                kind, case = target.split(':')
                try:
                    p.write_text(original.decode().replace(old, new) + '\n/* ' + MARK + ' */\n')
                    compile_sdk(label)
                    if kind == 'unit':
                        unit(label + '-red', case, True)
                    else:
                        pipe_probe(label + '-red', case, True)
                    run(label + '-control', [build / 'test_config', source / 'core/tests/fixtures'])
                finally:
                    p.write_bytes(original)
                    assert sha(p) == hashlib.sha256(original).hexdigest()
                compile_sdk(label + '-restored')
                if kind == 'unit':
                    unit(label + '-restored', case)
                else:
                    pipe_probe(label + '-restored', case)
                print(label + ': emitted TS/actual worker assertion red / native config green / restored green / SHA256', flush=True)
        suite('final')
        for name, digest in inputs.items():
            assert sha(source / name) == digest and sha(ROOT / name) == digest, name
        assert sha(node) == node_digest
        (logs / 'result.json').write_text(json.dumps(dict(accepted=True, inputs_sha256=inputs,
            commands=commands, node_version=version, node_sha256=node_digest,
            groups=[c[0] for c in cases] if args.mutate else [], unit_groups=16, transport_groups=PROBES,
            real_native_routes=['normal', 'cancel-before-respond'], sanitizers=['address', 'undefined'],
            leak_sanitizer=False), indent=2) + '\n')
        print('NODE PORT PASS: ' + str(logs), flush=True)
    except BaseException as error:
        (logs / 'failure.txt').write_text(repr(error))
        raise


def stop(number, frame):
    raise KeyboardInterrupt('signal ' + str(number))


if __name__ == '__main__':
    for number in [signal.SIGINT, signal.SIGTERM]:
        signal.signal(number, stop)
    main()
