#!/usr/bin/env python3
"""Per-call SFTP byte observations: compiled mutations in an isolated Core copy."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_SFTP_PROGRESS_MUTATION'


def main():
    originals = {p: p.read_bytes() for p in (ROOT/'core').rglob('*') if p.is_file() and 'build' not in p.relative_to(ROOT/'core').parts}
    if any(MARK.encode() in value for value in originals.values()):
        raise RuntimeError('Residual progress mutation')
    logs = ROOT/'tmp/sftp-progress-mutations'
    logs.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(ROOT/'tmp'), PYTHONPYCACHEPREFIX=str(ROOT/'tmp/progress-python-cache'))

    def run(args, label, red=None, extra=None):
        result = subprocess.run(['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require progress; shift; gt_timeout 240 "$@"',
                                 'progress', str(ROOT), *map(str,args)], cwd=ROOT, env=dict(env, **(extra or {})), capture_output=True, text=True)
        output = result.stdout + result.stderr
        (logs/(label+'.log')).write_text(output)
        okay = (result.returncode==1 and red in output) if red else result.returncode==0 and not re.search(r'(?m)^SKIP', output)
        if not okay:
            print(output, flush=True)
            raise RuntimeError(label+': expected '+('specific assertion red' if red else 'green'))
        return output

    with tempfile.TemporaryDirectory(prefix='sftp-progress-', dir=ROOT/'tmp') as folder:
        work = Path(folder)
        shutil.copytree(ROOT/'core', work/'core', ignore=shutil.ignore_patterns('build', 'build-*'))
        source = work/'core/src/sftp.c'
        clean = source.read_text()
        build = work/'core/build'
        run(['cmake', '-S', work/'core', '-B', build], 'configure')
        def compile_core(label):
            run(['cmake', '--build', build, '--clean-first', '--target', 'test_sftp_receive', 'test_sftp_create', 'test_config'], label+'-build')
        wrappers = {}
        for kind in ('receive','create'):
            wrapper = work/('protocol-'+kind)
            wrapper.write_text('#!/bin/bash\nexec '+shlex.join([str(ROOT/'build/ssh-channel-toolchain/bin/python'), str(ROOT/('scripts/test-sftp-'+kind+'.py'))])+' "$@"\n')
            wrapper.chmod(0o700)
            wrappers[kind] = wrapper
        def protocol(kind, case, label, red=None):
            output = run(['bash', ROOT/'core/tests/scripts/ssh_e2e.sh', wrappers[kind]], label, red,
                         {'SFTP_'+kind.upper()+'_CASE': case, 'GHOSTERM_SFTP_'+kind.upper()+'_BIN': str(build/('test_sftp_'+kind))})
            if not red and not any(marker in output for marker in ('SFTP '+kind.upper()+' PASS', 'SFTP '+kind.upper()+' PROTOCOL PASS')):
                raise RuntimeError('Protocol tests did not execute')
        compile_core('baseline')
        protocol('receive', '', 'receive-protocol-baseline')
        protocol('create', '', 'create-protocol-baseline')
        cases = [
            ('receive-initial','RECEIVE, 0, total_bytes, total_known','RECEIVE, 1, total_bytes, total_known','receive','normal','progress initial zero'),
            ('receive-kind','RECEIVE, count, total_bytes, total_known','CREATE, count, total_bytes, total_known','receive','normal','progress receive kind'),
            ('receive-count','RECEIVE, count, total_bytes, total_known','RECEIVE, count + 1, total_bytes, total_known','receive','normal','progress follows real destination writes'),
            ('receive-delivery','observer(&value, observer_context);','(void)value; (void)observer_context;','receive','normal','progress successful final bytes'),
            ('receive-unknown','total_known = (initial.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0','total_known = true','receive','unknown-size','progress known and unknown totals distinct'),
            ('receive-unknown-size','total_bytes = total_known ? initial.filesize : 0','total_bytes = total_known ? initial.filesize : 1','receive','unknown-size','progress unknown total has no fabricated size'),
            ('receive-publish','count += (uint64_t)length;','count += (uint64_t)length; *out_count = count;','receive','normal','progress is not publication'),
            ('create-initial','CREATE, 0, expected_size, true','CREATE, 1, expected_size, true','create','normal','create progress initial zero'),
            ('create-kind','CREATE, out->bytes_acked, expected_size, true','RECEIVE, out->bytes_acked, expected_size, true','create','normal','create progress kind'),
            ('create-count','CREATE, out->bytes_acked, expected_size, true','CREATE, out->bytes_acked + 1, expected_size, true','create','normal','create progress acknowledged bytes'),
            ('create-total','CREATE, out->bytes_acked, expected_size, true','CREATE, out->bytes_acked, expected_size + 1, true','create','normal','create progress exact known source total'),
            ('create-delivery','observer(&value, observer_context);','(void)value; (void)observer_context;','create','normal','create progress successful final bytes'),
            ('create-completion','out->bytes_acked += (uint64_t)written;','out->bytes_acked += (uint64_t)written; out->state = GT_SFTP_CREATE_COMPLETE;','create','normal','create progress is not completion'),
        ]
        for name, old, new, kind, case, red in cases:
            start = clean.index('int gt_sftp_'+kind+'_observed(')
            end = clean.index('int gt_sftp_create_observed(', start) if kind=='receive' else clean.index('int gt_sftp_receive_controlled(', start)
            piece = clean[start:end]
            expected = 2 if name.endswith('delivery') else 1
            if piece.count(old)!=expected:
                raise RuntimeError((name, piece.count(old)))
            changed = clean[:start]+piece.replace(old,new)+clean[end:]
            source.write_text(changed+'\n/* '+MARK+' */\n')
            compile_core(name)
            protocol(kind, case, name+'-red', 'FAIL: '+('receive ' if kind=='receive' else '')+red)
            run([build/'test_config', work/'core/tests/fixtures'], name+'-control')
            source.write_text(clean)
            compile_core(name+'-restored')
            protocol(kind, case, name+'-restored')
            if hashlib.sha256(source.read_bytes()).digest()!=hashlib.sha256(clean.encode()).digest():
                raise RuntimeError('Restore SHA256 mismatch')
            print('PASS '+name+': compiled target assertion red / independent config green / restored green / SHA256', flush=True)
    for path, value in originals.items():
        if path.read_bytes()!=value:
            raise RuntimeError('Production Core changed during isolation: '+str(path))
    print('SFTP PROGRESS MUTATIONS PASS',flush=True)


if __name__=='__main__':
    def interrupted(signum,frame): raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
