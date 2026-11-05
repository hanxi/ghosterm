#!/usr/bin/env python3
"""P0 standard file-protocol comparison: pinned upstream trzsz peers, not SDK.

--fetch alone permits source/module downloads. All source, Go modules, caches,
home/config, executables and transferred fixture bytes stay in repository tmp/.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
URL = 'https://codeload.github.com/trzsz/trzsz-go/tar.gz/refs/tags/v1.2.0'
DIGEST = '6577fbab008264ff4678f60f56a0dea1e68763064a638eaf54e560198a5e6fd3'
TESTS = ('TestTransferAction', 'TestTransferConfig', 'TestBase64Reader', 'TestBase64Writer',
         'TestEscapeReader', 'TestEscapeWriter', 'TestZstdReaderWriter', 'TestPipelineEncodeAndDecode',
         'TestPipelineEscapeData', 'TestPipelineUnescapeData', 'TestRecvDataReader', 'TestSendDataWriter',
         'TestBufferReadLine', 'TestBufferReadBinary', 'TestBufferReadJunk')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, limit=300, ok=True, **kwargs):
    command = ['bash', '-c', 'source "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require; shift; gt_timeout "$@"',
               '_', str(ROOT), str(limit)] + [str(x) for x in args]
    result = subprocess.run(command, capture_output=True, **kwargs)
    if ok and result.returncode:
        raise RuntimeError('protocol probe failed: ' + repr(result))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    args = parser.parse_args()
    (ROOT / 'tmp').mkdir(exist_ok=True)
    cache = ROOT / 'tmp/plugin-protocol-sources'
    cache.mkdir(exist_ok=True)
    archive = cache / 'trzsz-go-v1.2.0.tar.gz'
    if not archive.exists():
        if not args.fetch:
            raise RuntimeError('pinned trzsz source missing; run with --fetch')
        partial = cache / 'trzsz.download'
        try:
            with urllib.request.urlopen(URL, timeout=30) as response, partial.open('wb') as out:
                shutil.copyfileobj(response, out)
            if sha(partial) != DIGEST:
                raise RuntimeError('trzsz download SHA256 mismatch')
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    if sha(archive) != DIGEST:
        raise RuntimeError('trzsz cached source SHA256 mismatch')
    source_hash = sha(Path(__file__))
    with tempfile.TemporaryDirectory(prefix='plugin-protocol-', dir=ROOT / 'tmp') as temp:
        work = Path(temp)
        with tarfile.open(archive) as package:
            package.extractall(work, filter='data')
        source = work / 'trzsz-go-1.2.0'
        original = {str(p.relative_to(source)): sha(p) for p in source.rglob('*') if p.is_file()}
        home = work / 'home'
        home.mkdir()
        destination = work / 'received'
        destination.mkdir()
        (home / '.trzsz.conf').write_text('DefaultDownloadPath = ' + str(destination) + '\n')
        go_tmp = work / 'go-tmp'
        go_tmp.mkdir()
        environment = dict(os.environ, HOME=str(home), TMPDIR=str(work), GOTMPDIR=str(go_tmp),
                           GOENV='off', GOTOOLCHAIN='local', CGO_ENABLED='0',
                           GOCACHE=str(ROOT / 'tmp/plugin-protocol-go-cache'),
                           GOMODCACHE=str(ROOT / 'tmp/plugin-protocol-go-modules'),
                           GOPATH=str(ROOT / 'tmp/plugin-protocol-go-path'), GOPROXY='off', GOSUMDB='off')
        for name in ('TMUX', 'TMUX_PANE', 'STY', 'TERM_PROGRAM', 'TERM_PROGRAM_VERSION',
                     'SSH_TTY', 'SSH_CONNECTION', 'SSH_CLIENT', 'TRZSZ-FORK-BACKGROUND'):
            environment.pop(name, None)
        environment['TERM'] = 'xterm-256color'
        tools = work / 'guard-bin'
        tools.mkdir()
        guard = tools / 'tmux'
        guard_hit = work / 'unexpected-tmux'
        guard.write_text('#!/bin/sh\nprintf blocked >> ' + shlex.quote(str(guard_hit)) + '\nexit 89\n')
        guard.chmod(0o700)
        environment['PATH'] = str(tools) + os.pathsep + environment['PATH']
        if args.fetch:
            run(['go', 'mod', 'download'], cwd=source,
                env=dict(environment, GOPROXY='https://proxy.golang.org', GOSUMDB='sum.golang.org'))
        result = run(['go', 'test', '-json', '-count=1', '-mod=readonly', '-run', '^(' + '|'.join(TESTS) + ')$', './trzsz'],
                     cwd=source, env=environment)
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith(b'{')]
        passed = {event['Test'] for event in events if event.get('Action') == 'pass' and event.get('Test') in TESTS}
        if passed != set(TESTS) or any(event.get('Action') in ('fail', 'skip') for event in events):
            raise RuntimeError('upstream named tests missing, skipped or failed: ' + repr(result))
        print('PASS trzsz 1.2.0: 15 named upstream codec/handshake/fragment/escape/compression tests', flush=True)
        binaries = {}
        for name in ('trzsz', 'tsz'):
            binary = work / name
            run(['go', 'build', '-mod=readonly', '-o', binary, './cmd/' + name], cwd=source, env=environment)
            binaries[name] = binary
        guard_input = work / 'guard.bin'
        guard_input.write_bytes(b'G')
        blocked = run([binaries['tsz'], guard_input], env=dict(environment, TMUX=str(work / 'forbidden-socket'), TMUX_PANE='%0'),
                      stdin=subprocess.DEVNULL, limit=10, ok=False)
        hits = guard_hit.read_text() if guard_hit.exists() else ''
        if blocked.returncode != 253 or b'exit status 89' not in blocked.stderr or not hits or hits.replace('blocked', ''):
            raise RuntimeError('tmux tripwire did not block inherited context: ' + repr((blocked, hits)))
        guard_hit.unlink()
        print('PASS inherited tmux marker blocked by private PATH tripwire before transfer;',
              hits.count('blocked'), 'calls rejected', flush=True)
        transfers = []
        for name, flags in (('base64', []), ('binary-escaped', ['-b', '-e'])):
            input_path = work / (name + '.bin')
            input_path.write_bytes(bytes(range(256)) * 1024)
            expected = sha(input_path)
            # Keep client stdin open without injecting input. EOF would tell
            # the upstream wrapper to close its owned PTY before handshake.
            read, write = os.pipe()
            try:
                result = run([binaries['trzsz'], binaries['tsz'], '-q', '-t', '2', '-B', '1K', '-c', 'no', *flags, input_path],
                             env=environment, cwd=home, stdin=read, limit=30)
            finally:
                os.close(read)
                os.close(write)
            output_path = destination / input_path.name
            if not output_path.is_file() or sha(output_path) != expected or sha(input_path) != expected:
                raise RuntimeError('real upstream CLI transfer failed integrity: ' + repr(result))
            transfers.append({'mode': name, 'bytes': input_path.stat().st_size, 'sha256': expected})
            print('PASS real trzsz wrapper <-> tsz PTY transfer', name, input_path.stat().st_size, 'bytes/SHA256', flush=True)
        if guard_hit.exists():
            raise RuntimeError('isolated file protocol unexpectedly invoked tmux')
        if any(sha(source / name) != digest for name, digest in original.items()):
            raise RuntimeError('upstream source changed during probe')
        report = {'production': False, 'implementation': 'upstream trzsz-go 1.2.0', 'sourceSHA256': DIGEST,
                  'licenseSHA256': sha(source / 'LICENSE'), 'goModSHA256': sha(source / 'go.mod'),
                  'goSumSHA256': sha(source / 'go.sum'), 'driverSHA256': source_hash,
                  'upstreamTests': sorted(passed), 'transfers': transfers,
                  'goVersion': run(['go', 'version'], env=environment).stdout.decode().strip(),
                  'ghostermTrzszCompatibility': False, 'nestedSshOrTmux': False}
        report['tmuxTripwireVerified'] = True
    if sha(Path(__file__)) != source_hash:
        raise RuntimeError('driver changed during run')
    print(json.dumps(report, sort_keys=True), flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise InterruptedError(number)
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    main()
