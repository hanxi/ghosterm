#!/usr/bin/env python3
"""Compiled Apple jump-owner lifetime mutations against disposable local sshd."""
import hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    work=Path(tempfile.mkdtemp(prefix='rm-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir()
    for folder in ['core','scripts','patches','apple/GhostermCore']:
        shutil.copytree(ROOT/folder,work/folder,symlinks=True,ignore=shutil.ignore_patterns('build','.build','.swiftpm','__pycache__'))
    (work/'core/build').symlink_to(ROOT/'core/build',target_is_directory=True)
    (work/'build').mkdir();(work/'build/host-native').symlink_to((ROOT/'build/host-native').resolve(),target_is_directory=True)
    (work/'tmp').mkdir()
    env={**os.environ,'TMPDIR':str(work/'tmp'),'DEVELOPER_DIR':'/Applications/Xcode.app/Contents/Developer'}
    package=work/'apple/GhostermCore';build=work/'swift'
    def run(cmd,label,environment=env):
        p=subprocess.run(list(map(str,cmd)),cwd=work,env=environment,capture_output=True,text=True,timeout=120)
        (logs/(label+'.log')).write_text(p.stdout+p.stderr)
        assert p.returncode==0, (label,logs)
        if label.endswith('-control'):
            assert 'Executed 1 test, with 0 failures (0 unexpected)' in p.stdout+p.stderr
    def compile(label):run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',build],label+'-build')
    compile('baseline')
    bundles=list(build.rglob('GhostermCoreTests.xctest'));assert len(bundles)==1
    test=['xcrun','xctest','-XCTest','GhostermCoreTests.SSHRouteTests/testRouteControlledEndToEnd',str(bundles[0])]
    wrapper=work/'fixture'
    wrapper.write_text('#!'+sys.executable+'\nimport os,sys,subprocess\ncommand='+repr(test)+'''
os.environ.update(GHOSTERM_ROUTE_MODE=sys.argv[1],GHOSTERM_ROUTE_KNOWN=sys.argv[3],GHOSTERM_ROUTE_USER=sys.argv[4],GHOSTERM_ROUTE_PORTS=",".join(sys.argv[5:]))
p=subprocess.run(command,input=sys.stdin.buffer.read(),capture_output=True)
output=p.stdout+p.stderr
sys.stderr.buffer.write(output)
if os.environ.get('GHOSTERM_ROUTE_EXPECT_RED')=='1':
 valid=p.returncode==1 and output.count(b'error: -[')==1 and b'Executed 1 test, with 1 failure' in output and not any(x in output for x in [b'AddressSanitizer',b'runtime error:',b'UndefinedBehaviorSanitizer',b'Precondition failed',b'unexpected signal'])
 if valid: print('FAIL jump: normal Swift lifetime assertion',file=sys.stderr)
 sys.exit(1 if valid else 2)
sys.stdout.buffer.write(output)
sys.exit(p.returncode)
''');wrapper.chmod(0o700)
    def check(label,case,red=False):
        command=[sys.executable,work/'scripts/test-ssh-jump.py','--binary',wrapper,'--hops','2','--case',case]
        if red:command+=['--expect-assertion']
        before=set((work/'tmp').glob('sj-*'))
        run(command,label,{**env,'GHOSTERM_ROUTE_EXPECT_RED':'1' if red else '0'})
        made=set((work/'tmp').glob('sj-*'))-before;assert len(made)==1
        shutil.copytree(made.pop()/'logs',logs/label)
    names=['GhostermCore.swift','SSHAuthenticator.swift']
    paths={n:package/'Sources/GhostermCore'/n for n in names}
    originals={n:p.read_bytes() for n,p in paths.items()}
    hashes={n:hashlib.sha256(b).hexdigest() for n,b in originals.items()}
    cases=[
      ('explicit-close','GhostermCore.swift','if let release = routeRelease { routeRelease = nil; release() }','if let release = routeRelease { routeRelease = nil; _ = release }','pty'),
      ('session-deinit','GhostermCore.swift','if !closed, let release = routeRelease { release() }','if !closed, let release = routeRelease { _ = release }','pty-cancel'),
      ('cleanup-owner','SSHAuthenticator.swift','owner.async { cleanup.step() }','owner.async { _ = cleanup }','pty-cancel'),
    ]
    try:
        check('baseline-close','pty');check('baseline-deinit','pty-cancel')
        for label,name,old,new,case in cases:
            source=originals[name].decode();assert source.count(old)==1
            paths[name].write_text(source.replace(old,new));compile(label);check(label,case,True)
            run(['xcrun','xctest','-XCTest','GhostermCoreTests.GhostermCoreTests/testConfigRoundTripIsByteIdentical',bundles[0]],label+'-control')
            paths[name].write_bytes(originals[name]);compile(label+'-restored');check(label+'-restored',case)
            assert hashlib.sha256(paths[name].read_bytes()).hexdigest()==hashes[name]
            print(label+': assertion red / independent config green / restored / SHA256',flush=True)
    finally:
        for name,data in originals.items():
            paths[name].write_bytes(data)
            assert (ROOT/'apple/GhostermCore/Sources/GhostermCore'/name).read_bytes()==data
    (logs/'result.json').write_text(json.dumps({'accepted':True,'mutations':[c[0] for c in cases],'sources_sha256':hashes},indent=2)+'\n')
    for p in work.iterdir():
        if p==logs:continue
        if p.is_symlink() or p.is_file():p.unlink()
        else:shutil.rmtree(p)
    print('SWIFT ROUTE MUTATIONS PASS: '+str(logs))
if __name__=='__main__':main()
