#!/usr/bin/env python3
"""Isolated compiled SSH jump IO mutations, independent config, restored SHA256."""
import argparse, hashlib, json, os, shutil, signal, subprocess, sys, tempfile
from pathlib import Path
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--public-route-owner', action='store_true')
    parser.add_argument('--route-owner', action='store_true')
    args=parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix='jm-', dir=ROOT/'tmp')); logs = work/'logs'; logs.mkdir()
    for folder in ['core', 'scripts', 'patches']:
        shutil.copytree(ROOT/folder, work/folder, ignore=shutil.ignore_patterns('build', '__pycache__'))
    (work/'tmp').mkdir(); (work/'build').mkdir()
    (work/'build/host-native').symlink_to((ROOT/'build/host-native').resolve(), target_is_directory=True)
    build = work/'core/build'
    env = {**os.environ, 'TMPDIR': str(work/'tmp'), 'DEVELOPER_DIR': '/Applications/Xcode.app/Contents/Developer', 'PYTHONDONTWRITEBYTECODE': '1'}
    def run(command, label, timeout=120):
        result = subprocess.run(list(map(str, command)), cwd=work, env=env, capture_output=True, text=True, timeout=timeout)
        (logs/(label+'.log')).write_text(result.stdout + result.stderr)
        if result.returncode: raise RuntimeError(label+' failed: '+str(logs))
    run(['cmake', '-S', work/'core', '-B', build, '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0'], 'configure')
    def compile(label):
        # This test includes ssh.c to constrain its connect boundary. Force both
        # library and including TU rebuilds, even on second-granularity make.
        for name in ['ssh.c.o', 'sftp.c.o', 'test_ssh_jump.c.o', 'route.c.o', 'route-api.c.o', 'test_ssh_route_api.c.o']:
            for path in build.rglob(name): path.unlink()
        for name in ['libghosterm_core.a', 'test_ssh_jump', 'test_config']: (build/name).unlink(missing_ok=True)
        run(['cmake', '--build', build, '--target', 'test_ssh_jump', 'test_ssh_route_api', 'test_config', '-j', '6'], label+'-build')
    def check(label, case=None, red=False):
        if case == 'api':
            result=subprocess.run([str(build/'test_ssh_route_api')],cwd=work,env=env,capture_output=True,text=True,timeout=10)
            (logs/(label+'.log')).write_text(result.stdout+result.stderr)
            if red:
                assert result.returncode==1 and result.stderr.count('FAIL route API:')==1 and not any(x in result.stderr for x in ['AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer'])
            else: assert result.returncode==0
            return
        command = [sys.executable, work/'scripts/test-ssh-jump.py', '--binary', build/'test_ssh_jump']
        if args.route_owner: command += ['--route-owner']
        if args.public_route_owner: command += ['--public-route-owner']
        if case: command += ['--hops', '2', '--case', case]
        if red: command += ['--expect-assertion']
        before = set((work/'tmp').glob('sj-*'))
        run(command, label)
        created = set((work/'tmp').glob('sj-*')) - before
        assert len(created) == 1
        source = created.pop()/'logs'
        shutil.copytree(source, logs/label)
        receipt = json.loads((source/'result.json').read_text())
        assert receipt['accepted'] and receipt['expected_assertion'] == red
    cases = [('open-errno', 'sftp.c', '        if ((rc < 0 && (gt__ssh_wire_depth(ssh) > 1 || errno != EINTR)) ||\n            (rc > 0 && (wait.revents & (POLLERR | POLLNVAL)))) {\n', '        if ((rc < 0 && errno != EINTR) || (rc > 0 && (wait.revents & (POLLERR | POLLNVAL)))) {\n', 'wait-error'),
        ('read-errno', 'sftp.c', '    if ((result < 0 && (gt__ssh_wire_depth(ssh) > 1 || errno != EINTR)) ||\n        (result > 0 && (wait.revents & (POLLERR | POLLNVAL)))) {\n', '    if ((result < 0 && errno != EINTR) || (result > 0 && (wait.revents & (POLLERR | POLLNVAL)))) {\n', 'read-wait-error'),
        ('borrow', 'ssh.c', '        return GT_ERR_INVALID_ARG;\n    }\n    ssh->channel_started = true;\n', '        return GT_ERR_INVALID_ARG;\n    }\n    ssh->channel_started = true;\n    if (ssh->wire) {\n        return GT_ERR_UNSUPPORTED;\n    }\n', 'sftp'),
        ('pty-admit', 'ssh.c', '        *out_status = GT_ERR_INVALID_ARG;\n    }\n', '        *out_status = GT_ERR_INVALID_ARG;\n    }\n    if (ssh && ssh->wire) {\n        return NULL;\n    }\n', 'pty'),
        ('pty-wait', 'ssh.c', '        return gt__ssh_wire_wait(ssh, events ? events : POLLIN, 10, NULL, 0) < 0 ? GT_ERR_IO\n                                                                                 : GT_OK;\n', '        return GT_ERR_IO;\n', 'pty'),
        ('sftp-open-wait', 'sftp.c', '        int rc = gt__ssh_wire_depth(ssh) > 1 ? gt__ssh_wire_wait(ssh, wait.events, 10, NULL, 0)\n                                             : poll(&wait, 1, 10);\n', '        int rc = gt__ssh_wire_depth(ssh) > 1 ? -1 : poll(&wait, 1, 10);\n', 'sftp'),
        ('sftp-read-wait', 'sftp.c', '    int result = gt__ssh_wire_depth(ssh) > 1 ? gt__ssh_wire_wait(ssh, wait.events, 10, NULL, 0)\n                                             : poll(&wait, 1, 10);\n', '    int result = gt__ssh_wire_depth(ssh) > 1 ? -1 : poll(&wait, 1, 10);\n', 'sftp'),
        ('sftp-cancel', 'sftp.c', '    if (cancelled && cancelled(context)) {\n', '    if (false && cancelled && cancelled(context)) {\n', 'open-cancel'),
        ('release', 'ssh.c', '        if (gt__bridge_stream_destroy(ssh->wire) != GT_OK) {\n', '        if (false && gt__bridge_stream_destroy(ssh->wire) != GT_OK) {\n', 'cancel'),
        ('timeout-state', 'ssh.c', '        return GT_TRANSPORT_READ_TIMEOUT;\n', '        return GT_TRANSPORT_READ_EOF;\n', 'pty-idle')]
    if args.route_owner:
        cases = [('session-ready', 'extension/route.c', '            } else if (r->session_mode) {\n', '            } else if (false && r->session_mode) {\n', 'sftp'),
        ('borrow-once', 'extension/route.c', '    if (base->closed || stopping(r) || r->info.phase != GT_BRIDGE_ROUTE_READY || r->borrowed ||\n        !r->ready) {\n', '    if (base->closed || stopping(r) || r->info.phase != GT_BRIDGE_ROUTE_READY || !r->ready) {\n', 'sftp'),
        ('session-read', 'extension/route.c', '        return (gt_bridge_io_t){GT_BRIDGE_ERROR, 0, GT_ERR_UNSUPPORTED};\n', '        return (gt_bridge_io_t){GT_BRIDGE_ERROR, 0, GT_ERR_IO};\n', 'sftp'),
        ('owner-release', 'extension/route.c', '    close(r->wake[0]);\n    close(r->wake[1]);\n', '    (void)r;\n', 'cancel')]
    if args.public_route_owner:
        cases = [('hop-address', 'ssh/route-api.c', '            (gt_ssh_connector_config_t){.family = o->family,\n', '            (gt_ssh_connector_config_t){.family = 4,\n', 'sftp'),
        ('defer-credentials', 'ssh/route-api.c', '                                        .defer_credentials = o->defer_credentials};\n', '                                        .defer_credentials = false};\n', 'sftp'),
        ('info-hop', 'ssh/route-api.c', '                                 .hop = (uint32_t)info.hop,\n', '                                 .hop = 0,\n', 'api'),
        ('info-deadline', 'ssh/route-api.c', '                                 .deadline = info.deadline || a->deadline,\n', '                                 .deadline = a->deadline,\n', 'api'),
        ('dispose-pending', 'ssh/route-api.c', '    if (result != GT_BRIDGE_RELEASED) {\n', '    if (result != GT_BRIDGE_RELEASED) {\n        *released = true;\n', 'api'),
        ('dispose-error', 'ssh/route-api.c', '    if (result == GT_BRIDGE_CLOSE_ERROR) {\n        return GT_ERR_IO;\n', '    if (result == GT_BRIDGE_CLOSE_ERROR) {\n        return GT_OK;\n', 'api')]
    originals = {name: (work/'core/src'/name).read_bytes() for name in {case[1] for case in cases}}
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in originals.items()}
    try:
        compile('baseline'); check('baseline')
        for label, name, old, new, case in cases:
            path = work/'core/src'/name; original = originals[name]
            assert original.decode().count(old) == 1, label
            path.write_text(original.decode().replace(old, new))
            compile(label); check(label, case, True)
            run([build/'test_config', work/'core/tests/fixtures'], label+'-control')
            path.write_bytes(original); assert hashlib.sha256(path.read_bytes()).hexdigest() == hashes[name]
            compile(label+'-restored'); check(label+'-restored', case)
            print(label+': assertion red / independent config green / restored / SHA256', flush=True)
    finally:
        for name, data in originals.items():
            (work/'core/src'/name).write_bytes(data)
            assert hashlib.sha256((ROOT/'core/src'/name).read_bytes()).hexdigest() == hashes[name]
    (logs/'result.json').write_text(json.dumps({'accepted': True, 'mutations': [case[0] for case in cases], 'sources_sha256': hashes}, indent=2)+'\n')
    for child in work.iterdir():
        if child == logs: continue
        if child.is_symlink() or child.is_file(): child.unlink()
        else: shutil.rmtree(child)
    print('SSH JUMP MUTATIONS PASS: '+str(logs))

if __name__ == '__main__':
    def interrupt(signum, frame): raise KeyboardInterrupt(signum)
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupt)
    main()
