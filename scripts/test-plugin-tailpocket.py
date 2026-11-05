#!/usr/bin/env python3
"""P0 reference reproduction, NOT production Tailcat or a C networking port.

Unmodified pinned TailPocket supplies pairing/networking in a separate Go
process, using an isolated local DERP. C11 endpoints exchange bounded candidate
JSON frames across the tunnel. Two real loopback SSH servers and two isolated
tmux servers reproduce the pane routing boundary. No user keys or tmux touched.
"""
import argparse
from contextlib import ExitStack
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import select
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SELF = Path(__file__).resolve()
COMMIT = '47594c958dcff9a9194a1efe893b67de8ef83145'
ARCHIVE_SHA = '3c3725ba77b6cd637d4a720a9dbbe4c923b0059d6cf58db49efa535c052486e6'
REFERENCE = ROOT / 'scripts/probes/plugin-tailpocket-reference.go'
STREAM = ROOT / 'scripts/probes/plugin-tailpocket-stream.c'
TOOLS = ROOT / 'scripts/fixtures/plugin-tailpocket-tools'
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('plugin_ssh_reference', ROOT / 'scripts/test-plugin-ssh.py')
route = importlib.util.module_from_spec(spec)
spec.loader.exec_module(route)
bridge = route.bridge
require = route.require


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(args, env, cwd=None, limit=300):
    if args[0] == 'go' and env.get('PLUGIN_REFERENCE_GO'):
        args = [env['PLUGIN_REFERENCE_GO'], *args[1:]]
    result = bridge.timed([str(x) for x in args], env=env, cwd=cwd, limit=limit, capture_output=True)
    require(result.returncode == 0, 'build/reference command failed: ' + result.stderr.decode(errors='replace')[-5000:])
    return result


class Lines:
    def __init__(self, process):
        self.process = process
        self.buffer = b''

    def read(self, timeout=20):
        until = time.monotonic() + timeout
        while b'\n' not in self.buffer:
            require(time.monotonic() < until, 'process reply deadline')
            if select.select([self.process.stdout], [], [], 0.05)[0]:
                data = os.read(self.process.stdout.fileno(), 65536)
                require(bool(data), 'process closed before reply')
                self.buffer += data
        line, self.buffer = self.buffer.split(b'\n', 1)
        return line.decode()

    def send(self, text):
        require(self.process.poll() is None, 'command to dead probe process')
        self.process.stdin.write((text + '\n').encode())
        self.process.stdin.flush()

    def event(self, timeout=40):
        until = time.monotonic() + timeout
        while True:
            result = json.loads(self.read(max(0, until - time.monotonic())))
            if result['event'] != 'stage': return result
            print('REFERENCE', result['phase'], flush=True)


def fixtures(work):
    directory = work / 'frames'
    directory.mkdir()
    # Fixed synthetic identifiers and payloads, never credentials. These are
    # candidate envelope instances, not a negotiated production session.
    base = dict(wireVersion=1, connectionId='a'*32, instanceId='b'*32, epoch='1', streamId='control')
    hashes = {}

    def write(name, seq, kind, payload):
        data = json.dumps(dict(base, seq=str(seq), kind=kind, payload=payload),
                          ensure_ascii=False, separators=(',', ':')).encode()
        require(0 < len(data) <= 65536, 'fixture message budget')
        path = directory / (name + '.json')
        path.write_bytes(data)
        hashes[path.name] = sha(path)

    labels = ['baseline', 'inner-pane', 'inner-window', 'outer-pane']
    for i, label in enumerate(labels):
        request = f'{i+1:032x}'
        prompt = f'{i+101:032x}'
        write(f'request-{i}', 2*i, 'request', dict(requestId=request, method='task.step',
              input=dict(phase=label, text='中文🙂\x00\x1b\\\n', bytes=list(range(256))*3)))
        write(f'prompt-{i}', 2*i, 'request', dict(requestId=prompt, method='form.submit', input=dict(phase=label)))
        write(f'approval-{i}', 2*i+1, 'response', dict(requestId=prompt, outcome=dict(kind='success', value=dict(allow=True))))
        write(f'response-{i}', 2*i+1, 'response', dict(requestId=request, outcome=dict(kind='success', value=dict(phase=label))))
    write('exit', 8, 'request', dict(requestId='f'*32, method='task.exit', input={}))
    return directory, hashes


def prepare(work, fetch, env):
    cache = ROOT / 'tmp/plugin-tailpocket-sources'
    cache.mkdir(exist_ok=True)
    archive = cache / ('tailpocket-' + COMMIT + '.tar.gz')
    if not archive.exists():
        require(fetch, 'pinned reference source missing; use --fetch')
        partial = archive.with_suffix('.download')
        try:
            with urllib.request.urlopen('https://codeload.github.com/jt-wang/tailpocket/tar.gz/' + COMMIT,
                                        timeout=30) as response, partial.open('wb') as out:
                shutil.copyfileobj(response, out)
            require(sha(partial) == ARCHIVE_SHA, 'reference download SHA256 mismatch')
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    require(sha(archive) == ARCHIVE_SHA, 'cached reference source SHA256 mismatch')
    with tarfile.open(archive) as package:
        package.extractall(work, filter='data')
    source = work / ('tailpocket-' + COMMIT)
    originals = {str(p.relative_to(source)): sha(p) for p in source.rglob('*') if p.is_file()}
    build = work / 'reference'
    build.mkdir()
    module = (TOOLS / 'go.mod').read_text()
    module += '\nreplace github.com/jt-wang/tailpocket => ' + str(source) + '\n'
    (build / 'go.mod').write_text(module)
    shutil.copyfile(TOOLS / 'go.sum', build / 'go.sum')
    lock_hashes = {name: sha(build / name) for name in ('go.mod', 'go.sum')}
    shutil.copyfile(REFERENCE, build / 'main.go')
    if fetch:
        online = dict(env, GOPROXY='https://proxy.golang.org', GOSUMDB='sum.golang.org')
        print('Fetching pinned reference dependencies/toolchain into project tmp/', flush=True)
        command(['go', 'mod', 'download'], online, cwd=build, limit=600)
    toolchains = list(Path(env['GOMODCACHE']).glob('golang.org/toolchain@v0.0.1-go1.27.3.*/bin/go'))
    require(len(toolchains) == 1, 'verified pinned Go toolchain missing; use --fetch')
    # Invoke the already downloaded toolchain directly. The auto-selector tries
    # to reverify even cached toolchains with GOSUMDB=off during offline builds.
    env['PLUGIN_REFERENCE_GO'] = str(toolchains[0])
    env['GOTOOLCHAIN'] = 'local'
    command(['go', 'mod', 'verify'], env, cwd=build)
    binary = work / 'reference-net'
    print('Building isolated upstream Go reference and C11 endpoints', flush=True)
    command(['go', 'build', '-mod=readonly', '-o', binary, '.'], env, cwd=build, limit=600)
    require(all(sha(build / name) == digest for name, digest in lock_hashes.items()), 'reference module lock changed')
    stream = work / 'stream'
    command(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', STREAM, '-o', stream], env)
    return source, originals, binary, stream


def reproduce(work, env, reference, stream, directory):
    results = []
    endpoint = work / 'executor.sock'
    receipt = work / 'exit-receipt'
    status = work / 'executor-status'
    with ExitStack() as stack:
        net_log = stack.enter_context((work / 'network.log').open('wb'))
        network = subprocess.Popen([str(reference), str(endpoint)], env=env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=net_log)
        stack.callback(network.stdout.close)
        stack.callback(network.stdin.close)
        stack.callback(route.stop, network)
        net_lines = Lines(network)
        require(net_lines.event()['event'] == 'ready', 'reference ready')
        config = stack.enter_context(route.ssh_fixture(work))
        inner_cmd, inner = stack.enter_context(route.tmux_server(work, 'inner'))
        outer_cmd, outer = stack.enter_context(route.tmux_server(work, 'outer'))
        clients = []
        def close_clients():
            for client in reversed(clients): client.close()
        stack.callback(close_clients)
        after = work / 'after-executor'
        # After the real C child exits, this safe input recorder stands in for
        # the shell. A late plugin write must reach neither it nor other panes.
        executor_command = (shlex.join([str(stream), 'executor', str(endpoint), str(directory), str(receipt)]) +
            '; code=$?; printf "%s" "$code" > ' + shlex.quote(str(status)) + '; exec ' +
            shlex.join(route.peer_command(after, 0)))
        a = inner('new-session', '-d', '-x', '100', '-y', '32', '-s', 'probe', '-P', '-F', '#{pane_id}', executor_command)
        other = work / 'inner-other'
        b = inner('split-window', '-h', '-t', a, '-P', '-F', '#{pane_id}', shlex.join(route.peer_command(other, 0)))
        window = work / 'inner-window'
        c = inner('new-window', '-d', '-t', 'probe', '-P', '-F', '#{pane_id}', shlex.join(route.peer_command(window, 0)))
        inner('select-pane', '-t', a)
        attach_b = route.ssh(config, 'b', inner_cmd + ['attach-session', '-t', 'probe'], True)
        oa = outer('new-session', '-d', '-x', '100', '-y', '32', '-s', 'probe', '-P', '-F', '#{pane_id}', shlex.join(attach_b))
        outer_other = work / 'outer-other'
        ob = outer('split-window', '-h', '-t', oa, '-P', '-F', '#{pane_id}', shlex.join(route.peer_command(outer_other, 0)))
        outer('select-pane', '-t', oa)
        clients.append(bridge.Client(route.ssh(config, 'a', outer_cmd + ['attach-session', '-t', 'probe'], True)))
        bridge.wait_for(lambda: endpoint.exists() and len(inner('list-clients').splitlines()) == 1,
                        clients, 'nested C executor ready')
        net_lines.send('pair')
        paired = net_lines.event()
        require(paired['event'] == 'paired' and paired['allowed'] == 1, 'real pairing failed')
        print('PASS pairing: encoded invite, one-time window, peer key binding, memory-only state', flush=True)
        results.append('pairing')
        app_log = stack.enter_context((work / 'client.log').open('wb'))
        application = subprocess.Popen([str(stream), 'client', str(paired['port']), str(directory)], env=env,
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=app_log)
        stack.callback(application.stdout.close)
        stack.callback(application.stdin.close)
        stack.callback(route.stop, application)
        app_lines = Lines(application)
        require(app_lines.read() == 'READY C client', 'C client ready')
        def roundtrip(i, label):
            app_lines.send('round')
            require(app_lines.read() == f'PASS C round {i}', 'C duplex frame check/' + label)
            print('PASS C duplex candidate frames:', label, flush=True)
            results.append(label)
        roundtrip(0, 'baseline')
        net_lines.send('unpaired')
        require(net_lines.event()['event'] == 'unpaired-rejected', 'unpaired key reached application')
        print('PASS unpaired client key cannot reach the live service', flush=True)
        results.append('unpaired-key-rejected')
        clients.append(bridge.Client(route.ssh(config, 'a', attach_b, True)))
        bridge.wait_for(lambda: len(inner('list-clients').splitlines()) == 2, clients, 'second tmux client')
        os.write(clients[1].fd, b'\x02\x1b[C')
        bridge.wait_for(lambda: inner('display-message', '-p', '-t', 'probe', '#{pane_id}') == b,
                        clients, 'external pane switch')
        os.write(clients[0].fd, b'P')
        bridge.wait_for(lambda: route.received(other) == b'P', clients, 'TTY pane route control')
        roundtrip(1, 'inner-pane')
        os.write(clients[1].fd, b'\x02n')
        bridge.wait_for(lambda: inner('display-message', '-p', '-t', 'probe', '#{pane_id}') == c,
                        clients, 'external window switch')
        os.write(clients[0].fd, b'W')
        bridge.wait_for(lambda: route.received(window) == b'W', clients, 'TTY window route control')
        roundtrip(2, 'inner-window')
        outer('select-pane', '-t', ob)
        bridge.wait_for(lambda: Path(str(outer_other) + '.ready').exists(), clients, 'outer pane ready')
        os.write(clients[0].fd, b'O')
        bridge.wait_for(lambda: route.received(outer_other) == b'O', clients, 'TTY outer route control')
        roundtrip(3, 'outer-pane')
        net_lines.send('stats')
        stats = net_lines.event()
        require(stats['accepted'] == 1 and stats['derpRequests'] >= 2, 'network route accounting')
        app_lines.send('exit')
        require(app_lines.read() == 'PASS C outcome unknown; route closed', 'executor exit not closed/unknown')
        bridge.wait_for(lambda: status.exists() and Path(str(after) + '.ready').exists(), clients, 'executor exited')
        require(status.read_text() == '23' and receipt.read_text() == 'received-exit-without-response\n', 'actual executor exit boundary')
        app_lines.send('late')
        require(app_lines.read() == 'PASS C late request rejected before write', 'late write')
        require(route.received(after) == b'' and route.received(other) == b'P' and
                route.received(window) == b'W' and route.received(outer_other) == b'O', 'plugin data leaked into terminal input')
        print('PASS executor exit 23: request received/no response => unknown; EOF closes route; late send refused', flush=True)
        results.append('executor-exit-unknown-and-late-write-refused')
        net_lines.send('probe-after-exit')
        control = net_lines.event()
        require(control['event'] == 'route-live-executor-gone' and control['accepted'] == 2, 'exit/revoke control missing')
        print('PASS before revoke: network service still reachable; exited C executor is unavailable', flush=True)
        results.append('exit-versus-revoke-control')
        net_lines.send('revoke')
        revoked = net_lines.event()
        require(revoked['event'] == 'revoked' and revoked['freshDialRejected'] and revoked['accepted'] == 2,
                'revocation failed')
        print('PASS revoke: old link/fresh client cannot reach service; no executor restart', flush=True)
        results.append('revoke-old-link-fresh-client')
        application.stdin.close()
        require(application.wait(timeout=10) == 0, 'C client exit')
        net_lines.send('stop')
        require(net_lines.event()['event'] == 'stopped', 'reference stop ack')
        network.stdin.close()
        require(network.wait(timeout=10) == 0, 'reference process clean exit')
    for name in ('client.log', 'network.log'):
        require(not (work / name).read_bytes(), 'probe stderr not empty/' + name)
    counters = json.loads(stats['derpCounters'])
    require(counters.get('packets_sent', 0) > 0 and counters.get('bytes_sent', 0) > 0, 'DERP carried no packets')
    # Report metrics only, not addresses, keys, invites, links, or identity state.
    return results, {k: v for k, v in counters.items() if isinstance(v, (int, float)) and
                     any(word in k for word in ('packet', 'byte', 'client'))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true', help='explicit source/module/toolchain downloads')
    args = parser.parse_args()
    inputs = [SELF, REFERENCE, STREAM, Path(route.__file__), Path(bridge.__file__), TOOLS / 'go.mod', TOOLS / 'go.sum']
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='tp-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        home = work / 'home'; home.mkdir()
        env = dict(os.environ, HOME=str(home), ZDOTDIR=str(home), TMPDIR=str(work), GOENV='off',
                   GOTOOLCHAIN='go1.27.3', CGO_ENABLED='0', GOPROXY='off', GOSUMDB='off', TERM='xterm-256color',
                   TS_DEBUG_ALWAYS_USE_DERP='true', ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1')
        for variable, name in [('GOTMPDIR', 'go-tmp'), ('GOCACHE', 'go-cache'),
                               ('GOMODCACHE', 'go-modules'), ('GOPATH', 'go-path')]:
            path = ROOT / ('tmp/plugin-tailpocket-' + name); path.mkdir(exist_ok=True)
            env[variable] = str(path)
        for name in ('TMUX', 'TMUX_PANE', 'STY', 'TERM_PROGRAM', 'TERM_PROGRAM_VERSION',
                     'SSH_TTY', 'SSH_CONNECTION', 'SSH_CLIENT', 'SSH_AUTH_SOCK', 'TAILCAT_ADDR_FILE'):
            env.pop(name, None)
        # Imported SSH/tmux helpers inherit os.environ. Restore it even on failure.
        saved = dict(os.environ)
        try:
            os.environ.clear(); os.environ.update(env)
            source, originals, reference, stream = prepare(work, args.fetch, env)
            directory, frame_hashes = fixtures(work)
            outcomes, counters = reproduce(work, env, reference, stream, directory)
            require(all(sha(source / name) == digest for name, digest in originals.items()), 'reference source changed')
            for path in work.rglob('*'):
                if path.is_file() and source not in path.parents and 'reference' not in path.parts and path.suffix not in ('.dSYM',):
                    # Source/docs naturally describe private-key formats; runtime
                    # artifacts must never contain serialized fixture secrets.
                    if path in (reference, stream) or '.dSYM' in str(path): continue
                    data = path.read_bytes()
                    require(b'privkey:' not in data and b'PRIVATE KEY-----' not in data and b'openssh-key-v1\0' not in data,
                            'private key serialization in runtime artifact: ' + path.name)
            version = command(['go', 'version'], env).stdout.decode().strip()
        finally:
            os.environ.clear(); os.environ.update(saved)
    require(all(sha(p) == hashes[str(p.relative_to(ROOT))] for p in inputs), 'probe inputs changed')
    print(json.dumps(dict(production=False, cTailcatImplementation=False,
          scope='one macOS host; two loopback sshd/tmux servers; local DERP TLS fixture only',
          upstreamCommit=COMMIT, upstreamArchiveSHA256=ARCHIVE_SHA, inputSHA256=hashes,
          frameSHA256=frame_hashes, outcomes=outcomes, derpCounters=counters, goVersion=version,
          privateFixtureKeysOnDisk=False, automaticReconnect=False), sort_keys=True), flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise InterruptedError(number)
    for number in (signal.SIGINT, signal.SIGTERM): signal.signal(number, interrupted)
    main()
