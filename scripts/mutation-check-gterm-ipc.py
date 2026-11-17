#!/usr/bin/env python3
"""Compile isolated CLI IPC defects; normal target assertion / same Core control.

Only CLI objects are ASan/UBSan-instrumented here. The explicitly supplied Core
archive is immutable and shared with the native HOST and config control.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_GTERM_IPC_MUTATION'


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--build',type=Path,required=True);parser.add_argument('--group',action='append');args=parser.parse_args()
    build=args.build.resolve(strict=True)
    originals={name:(ROOT/'cli/gterm'/name).read_bytes() for name in ('ipc.c','main.c')}
    if any(MARK.encode() in original for original in originals.values()):raise RuntimeError('Residual IPC mutation')
    cases=[
        ('directory-link',[('open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)','open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)')],'directory-link'),
        ('directory-mode',[('(info.st_mode & 0777) != 0700','false')],'directory-mode'),
        ('permit-mode',[('(info.st_mode & 0777) == 0600','true')],'permit-mode'),
        ('permit-hardlink',[('info.st_nlink == 1','true')],'permit-hardlink'),
        ('permit-link',[('O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC','O_RDONLY | O_NONBLOCK | O_CLOEXEC'),('entry(e->directory, permit_name, &info)','true')],'permit-link'),
        ('socket-link',[('fstatat(e.directory, socket_name, &socket_info, AT_SYMLINK_NOFOLLOW)','fstatat(e.directory, socket_name, &socket_info, 0)'),('!entry(e.directory, socket_name, &socket_info)','false')],'socket-link'),
        ('permit-match',[('ok = ok && !difference &&','(void)difference; ok = ok &&')],'bad-permit'),
        ('one-shot',[('remove_entry(&e, permit_name, &e.permit, &e.has_permit);','true;'),('if (!remove_entry(&e, socket_name, &e.socket, &e.has_socket))','if (false)')],'one-shot'),
        ('cleanup-identity',[('!entry(e->directory, name, identity) || unlinkat','(void)identity, unlinkat')],'replacement-cleanup'),
        ('ack',[('memcmp(ack, acknowledged, sizeof(ack)) ||','false ||')],'bad-ack'),
        ('bytes',[('p->used = (size_t)n;','p->used = (size_t)n; p->bytes[0] ^= 1;')],'duplex'),
        ('half-close',[('shutdown(socket, SHUT_WR)','shutdown(socket, SHUT_RDWR)')],'half-close'),
        ('public-instance',[('!instance_matches(&permit, instance)','(!instance_matches(&permit, instance) && false)')],'instance'),
        ('invocation-count',[('number[3] != argc','false')],'invocation-count'),
        ('invocation-bytes',[('(!sending && memcmp(bytes, argv[i], size))','false')],'invocation-argument'),
        ('hangup',[('sigaction(SIGHUP, &action, NULL)','0')],'idle-hangup'),
    ]
    for label,replacements,_ in cases:
        for old,_ in replacements:
            original=originals['main.c' if label=='hangup' else 'ipc.c']
            if original.decode().count(old)!=1:raise RuntimeError('Ambiguous mutation: '+label+' '+old)
    if args.group:
        if set(args.group)-{label for label,_,_ in cases}:raise RuntimeError('Unknown IPC mutation group')
        cases=[case for case in cases if case[0] in args.group]
    inputs={str(p.relative_to(ROOT)):sha(p) for folder in ['cli/gterm','core/src','core/include'] for p in (ROOT/folder).rglob('*') if p.is_file()}
    for p in [Path(__file__),ROOT/'scripts/test-gterm-ipc.py',ROOT/'scripts/test-gterm.py']:inputs[str(p.relative_to(ROOT))]=sha(p)
    archive=build/'core/libghosterm_core.a';archive_digest=sha(archive)
    with tempfile.TemporaryDirectory(prefix='gterm-ipc-mutations-',dir=ROOT/'tmp') as temporary:
        work=Path(temporary);source=work/'source';shutil.copytree(ROOT/'cli/gterm',source)
        binary=work/'bin';binary.mkdir();logs=work/'logs';logs.mkdir()
        (binary/'test_gterm_stdio').symlink_to(build/'test_gterm_stdio')
        env={**os.environ,'TMPDIR':str(ROOT/'tmp'),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
        def run(label,command,red=False):
            # Keep timeout and its children in the private group created below;
            # timeout's default extra group would escape cancellation cleanup.
            shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require gterm-ipc; shift; exec "$gt_timeout_bin" --foreground 60 "$@"'
            p=subprocess.Popen(['bash','-c',shell,'gterm-ipc',str(ROOT),*map(str,command)],cwd=ROOT,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
            try:stdout,stderr=p.communicate(timeout=70)
            except BaseException:
                try:os.killpg(p.pid,signal.SIGTERM)
                except ProcessLookupError:pass
                try:p.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    try:os.killpg(p.pid,signal.SIGKILL)
                    except ProcessLookupError:pass
                    p.communicate(timeout=3)
                raise
            output=stdout+stderr;(logs/(label+'.log')).write_text(output)
            if red:
                if p.returncode!=1 or not re.search(r'^FAIL IPC ',output,re.M) or any(token in output for token in ['Traceback','AddressSanitizer','runtime error:','ABNORMAL']):raise RuntimeError(label+': expected normal single IPC assertion\n'+output)
                if len(re.findall(r'^FAIL IPC ',output,re.M))!=1:raise RuntimeError(label+': multiple target failures')
            elif p.returncode:raise RuntimeError(label+'\n'+output)
            return output
        compiler=shlex.split((build/'CMakeFiles/gterm.dir/link.txt').read_text())[0]
        flags=[compiler,'-std=c11','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-I'+str(ROOT/'core/include'),'-DGTERM_SDK_VERSION="1.0.0"','-DGTERM_PROTOCOL="1"']
        # Match every current production object; catalog / inspection helpers
        # are part of the CLI even when the target mutation is confined to IPC.
        helpers=[p.stem for p in sorted(source.glob('*.c')) if p.stem not in ('main','ipc')]
        for name in helpers:run('compile-'+name,[*flags,'-c',source/(name+'.c'),'-o',binary/(name+'.c.o')])
        link=shlex.split((build/'CMakeFiles/gterm.dir/link.txt').read_text());link.insert(1,'-fsanitize=address,undefined')
        at=link.index('-o');link[at+1]=str(binary/'gterm')
        for i,part in enumerate(link):
            if part.endswith('.o'):link[i]=str(binary/Path(part).name)
            elif part.endswith(('.a','.dylib','.so')) and not Path(part).is_absolute():link[i]=str(build/part)
        def compile(label,filename='ipc.c'):
            (binary/'ipc.c.o').unlink(missing_ok=True);(binary/'gterm').unlink(missing_ok=True)
            run(label+'-main',[*flags,'-c',source/'main.c','-o',binary/'main.c.o'])
            run(label+'-compile',[*flags,'-c',source/'ipc.c','-o',binary/'ipc.c.o'])
            run(label+'-link',link)
            return sha(binary/(filename+'.o')),sha(binary/'gterm')
        # Match the original CMake object names for the one unchanged link command.
        baseline=compile('baseline')
        run('baseline',['python3',ROOT/'scripts/test-gterm-ipc.py','--build',binary,*(['--case',cases[0][2]] if args.group else [])])
        results=[]
        for label,replacements,case in cases:
            filename='main.c' if label=='hangup' else 'ipc.c';original=originals[filename]
            if filename=='main.c':baseline=(sha(binary/'main.c.o'),sha(binary/'gterm'))
            text=original.decode()
            for old,new in replacements:text=text.replace(old,new)
            (source/filename).write_text(text+'\n/* '+MARK+' */\n')
            mutated=compile(label,filename)
            if any(a==b for a,b in zip(baseline,mutated)):raise RuntimeError('Actual IPC object/binary did not change: '+label)
            run(label+'-target',['python3',ROOT/'scripts/test-gterm-ipc.py','--build',binary,'--case',case],True)
            run(label+'-control',[build/'core/test_config',ROOT/'core/tests/fixtures'])
            (source/filename).write_bytes(original);restored=compile(label+'-restore',filename)
            run(label+'-green',['python3',ROOT/'scripts/test-gterm-ipc.py','--build',binary,'--case',case])
            if sha(source/filename)!=hashlib.sha256(original).hexdigest():raise RuntimeError('Source restoration failed')
            results.append(dict(group=label,case=case,objectChanged=True,binaryChanged=True,normalAssertionRed=True,restoredObject=restored[0]))
            print(label+': compiled normal IPC assertion red / same Core config green / restore green / SHA256',flush=True)
        run('final',['python3',ROOT/'scripts/test-gterm-ipc.py','--build',binary,*(['--case',cases[-1][2]] if args.group else [])])
        if sha(archive)!=archive_digest or any(sha(ROOT/name)!=digest for name,digest in inputs.items()):raise RuntimeError('Inputs changed during mutations')
        print('GTERM IPC MUTATIONS PASS: '+str(len(results))+' groups; CLI ASan/UBSan, immutable Core '+archive_digest,flush=True)


def stop(number,frame):raise KeyboardInterrupt(str(number))
if __name__=='__main__':
    signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
