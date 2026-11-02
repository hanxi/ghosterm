#!/usr/bin/env python3
"""Removal guards: compiled assertion red, independent config green, restore SHA."""
import argparse,hashlib,json,os,re,shlex,shutil,signal,subprocess,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_SFTP_REMOVE_MUTATION'
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    mode=parser.add_mutually_exclusive_group();mode.add_argument('--core-only',action='store_true');mode.add_argument('--swift-only',action='store_true')
    args=parser.parse_args()
    originals={p:p.read_bytes() for p in [ROOT/'core/src/sftp.c',ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']}
    if any(MARK.encode() in s for s in originals.values()):raise RuntimeError('Residual removal mutation')
    logs=ROOT/'tmp/sftp-remove-mutations'/str(time.time_ns());logs.mkdir(parents=True)
    env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    accepted=[]
    def run(command,label,red=None):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require sftp-remove; shift; gt_timeout 240 "$@"'
        r=subprocess.run(['bash','-c',shell,'sftp-remove',str(ROOT),*map(str,command)],cwd=ROOT,env=env,capture_output=True,text=True)
        out=r.stdout+r.stderr;(logs/(label+'.log')).write_text(out)
        valid=(r.returncode==1 and red in out) if red else r.returncode==0 and not re.search(r'(?m)^SKIP|Test Case .* skipped',out)
        if not valid:print(out,flush=True);raise RuntimeError(label+': expected '+('normal assertion red' if red else 'green'))
        return out
    def record(scope,name):
        accepted.append(dict(scope=scope,name=name,compiled_assertion_red=True,independent_config_green=True,restored_green=True,sha256=True))
        (logs/'accepted-results.json').write_text(json.dumps(accepted,indent=2)+'\n')
        print('PASS removal '+scope+' '+name+': compiled assertion red / config green / restored green / SHA256',flush=True)
    try:
        with tempfile.TemporaryDirectory(prefix='sftp-remove-',dir=ROOT/'tmp') as folder:
            work=Path(folder);(work/'tmp').mkdir()
            shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
            source=work/'core/src/sftp.c';clean=source.read_text();build=work/'core/build'
            sdk=subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],env=env,text=True).strip()
            run(['cmake','-S',work/'core','-B',build,'-DCMAKE_OSX_SYSROOT='+sdk,'-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0'],'configure')
            def compile_core():run(['cmake','--build',build,'--clean-first','--target','test_sftp_remove','test_sftp_remove_stub','test_config'],'core-build')
            wrapper=work/'remove-case'
            def case(name,label,red=None):
                wrapper.write_text('#!/bin/bash\nexec '+shlex.quote(str(build/'test_sftp_remove'))+' "$@"'+(' '+shlex.quote(name) if name else '')+'\n');wrapper.chmod(0o700)
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',wrapper],label,red)
                if not red and 'SFTP REMOVE PASS' not in out:raise RuntimeError('Real removal fixture did not execute')
            compile_core()
            if not args.swift_only:
                case('','baseline');run([build/'test_sftp_remove_stub'],'stub-baseline')
                start=clean.index('static int remove_matches(');end=clean.index('int gt_sftp_stat(',start);piece=clean[start:end]
                variants=[
                    ('zero-budget','!out || !timeout_ms','!out','normal'),
                    ('jobs','if (has_jobs) return GT_ERR_INVALID_ARG;','if (has_jobs && false) return GT_ERR_INVALID_ARG;','jobs'),
                    ('reset','{GT_SFTP_REMOVE_NOT_ATTEMPTED, 0};','{GT_SFTP_REMOVE_REMOVED, 99};','normal'),
                    ('parent','!mkdir_parent(parent)','false','normal'),
                    ('leaf'," || strchr(name, '/')",'','normal'),
                    ('size-range','expected_size > INT64_MAX','(expected_size > INT64_MAX && false)','normal'),
                    ('directory-size','(directory && expected_size != 0)','(directory && expected_size != 0 && false)','normal'),
                    ('size','attributes->filesize != size','(attributes->filesize != size && false)','wrong-size'),
                    ('time','attributes->mtime != (unsigned long)mtime','(attributes->mtime != (unsigned long)mtime && false)','wrong-time'),
                    ('kind','(attributes->permissions & LIBSSH2_SFTP_S_IFMT) != kind','((attributes->permissions & LIBSSH2_SFTP_S_IFMT) != kind && false)','symlink'),
                    ('missing-type','!(attributes->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)','false','missing-type'),
                    ('missing-size','!(attributes->flags & LIBSSH2_SFTP_ATTR_SIZE)','false','missing-size'),
                    ('missing-time','!(attributes->flags & LIBSSH2_SFTP_ATTR_ACMODTIME)','false','missing-mtime'),
                    ('recheck','pass < 2','pass < 1','second-change'),
                    ('unknown','if (publish_rejected(out->remote_status))','if (out->remote_status || publish_rejected(out->remote_status))','unknown-code'),
                    ('rejected','out->state = GT_SFTP_REMOVE_REJECTED;','out->state = GT_SFTP_REMOVE_UNKNOWN;','unsupported'),
                    ('late-cancel','if (result == 0) { out->state = GT_SFTP_REMOVE_REMOVED;','if (result == 0) { status = open_status(deadline, cancelled, context); if(status != GT_OK) goto finished; out->state = GT_SFTP_REMOVE_REMOVED;','late-cancel'),
                    ('abort','gt__ssh_abort(sftp->ssh);','(void)sftp;','wrong-size'),
                    ('restore','libssh2_session_set_timeout(session, timeout);','libssh2_session_set_timeout(session, timeout + 1);','normal'),
                ]
                changes=[]
                for name,old,new,target in variants:
                    if piece.count(old)!=1:raise RuntimeError('Missing or ambiguous anchor: '+name)
                    changes.append((name,piece.replace(old,new),target))
                at=piece.index('  SFTP_LOCK(sftp);');changes.append(('cancel',piece[:at]+piece[at:].replace('cancelled, context','NULL, NULL'),'cancel-before-delete'))
                for name,changed,target in changes:
                    source.write_text(clean[:start]+changed+clean[end:]+'\n/* '+MARK+' */\n')
                    compile_core();case(target,name+'-red','FAIL: remove ')
                    run([build/'test_config',work/'core/tests/fixtures'],name+'-control')
                    source.write_text(clean);compile_core();case(target,name+'-restored')
                    assert source.read_bytes()==originals[ROOT/'core/src/sftp.c'];record('Core',name)
                old='gt__set_error("sftp remove controlled: libssh2 unavailable");\n  return GT_ERR_UNSUPPORTED;'
                assert clean.count(old)==1;source.write_text(clean.replace(old,old.replace('GT_ERR_UNSUPPORTED','GT_OK'))+'\n/* '+MARK+' */\n')
                compile_core();run([build/'test_sftp_remove_stub'],'stub-red','FAIL: remove unsupported capability');run([build/'test_config',work/'core/tests/fixtures'],'stub-control')
                source.write_text(clean);compile_core();run([build/'test_sftp_remove_stub'],'stub-restored');assert source.read_bytes()==originals[ROOT/'core/src/sftp.c'];record('Core','unavailable')
            if not args.core_only:
                package=work/'apple/GhostermCore';shutil.copytree(ROOT/'apple/GhostermCore',package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
                binding=package/'Sources/GhostermCore/GhostermCore.swift';swift_clean=binding.read_text()
                swift=['xcrun','swift','test','--package-path',package,'--scratch-path',work/'swift-build','--skip-build']
                fixture=work/'swift-case';fixture.write_text('#!/bin/bash\nset -euo pipefail\nexport GHOSTERM_SSH_HOST="$1" GHOSTERM_SSH_PORT="$2" GHOSTERM_SSH_USER="$3" GHOSTERM_SSH_KEY="$4" GHOSTERM_SSH_KNOWN_HOSTS="$5/known"\nexec '+shlex.join(map(str,[*swift,'--filter','GhostermCoreTests.testSFTPRemovalEndToEnd']))+'\n');fixture.chmod(0o700)
                def swift_build():run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',work/'swift-build'],'swift-build')
                def swift_test(label,red=None):
                    out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',fixture],label,red)
                    if 'Executed 1 test' not in out:raise RuntimeError('Swift test did not execute')
                swift_build();swift_test('swift-baseline')
                start=swift_clean.index('    public func removeReviewed(');end=swift_clean.index('    /// One empty remote directory;',start);piece=swift_clean[start:end]
                cases=[('cancel','return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled','return false'),
                    ('budget','timeoutMilliseconds, { pointer in','3000, { pointer in'),
                    ('paths','handle, path, leaf, directory','handle, leaf, path, directory'),
                    ('directory','handle, path, leaf, directory','handle, path, leaf, false'),
                    ('size','directory, expectedSize, expectedModifiedSeconds','directory, 0, expectedModifiedSeconds'),
                    ('time','directory, expectedSize, expectedModifiedSeconds','directory, expectedSize, expectedModifiedSeconds + 1'),
                    ('remote','remoteStatus: outcome.remote_status','remoteStatus: 0')]
                for name,old,new in cases:
                    assert piece.count(old)==1,name
                    binding.write_text(swift_clean[:start]+piece.replace(old,new)+swift_clean[end:]+'\n// '+MARK+'\n')
                    swift_build();swift_test('swift-'+name+'-red','XCTAssertEqual failed')
                    run([*swift,'--filter','GhostermCoreTests.testConfigRoundTripIsByteIdentical'],'swift-'+name+'-control')
                    binding.write_text(swift_clean);swift_build();swift_test('swift-'+name+'-restored')
                    assert binding.read_bytes()==originals[ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift'];record('Swift',name)
    finally:
        if any(p.read_bytes()!=s for p,s in originals.items()):raise RuntimeError('Production changed during removal mutations')
    print('SFTP REMOVE MUTATIONS PASS: '+str(len(accepted))+' groups; receipts '+str(logs),flush=True)
if __name__=='__main__':
    def interrupted(number,frame):raise KeyboardInterrupt(number)
    for value in (signal.SIGINT,signal.SIGTERM):signal.signal(value,interrupted)
    main()
