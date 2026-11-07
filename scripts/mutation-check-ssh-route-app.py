#!/usr/bin/env python3
"""Compiled ordinary App ProxyJump authentication mutations against disposable local sshd."""
import argparse, hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--file-operations',action='store_true');parser.add_argument('--case',action='append',default=[]);args=parser.parse_args()
    work=Path(tempfile.mkdtemp(prefix='ram-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir()
    for folder in ['core','scripts','patches','apple/GhostermCore','apple/GhostermTerminal','apple/GhostermApp']:
        shutil.copytree(ROOT/folder,work/folder,symlinks=True,ignore=shutil.ignore_patterns('build','.build','.swiftpm','__pycache__'))
    (work/'core/build').symlink_to(ROOT/'core/build',target_is_directory=True)
    (work/'build').mkdir();(work/'build/host-native').symlink_to((ROOT/'build/host-native').resolve(),target_is_directory=True)
    (work/'build/GhosttyKit.xcframework').symlink_to((ROOT/'build/GhosttyKit.xcframework').resolve(),target_is_directory=True)
    (work/'tmp').mkdir()
    env={**os.environ,'TMPDIR':str(work/'tmp'),'DEVELOPER_DIR':'/Applications/Xcode.app/Contents/Developer','GHOSTERM_AUTOMATION_OFFSCREEN':'1'}
    package=work/'apple/GhostermApp';build=work/'swift'
    def run(cmd,label,environment=env):
        p=subprocess.run(list(map(str,cmd)),cwd=work,env=environment,capture_output=True,text=True,timeout=120)
        (logs/(label+'.log')).write_text(p.stdout+p.stderr)
        assert p.returncode==0, (label,logs)
        if label.endswith('-control'):
            assert 'Executed 1 test, with 0 failures (0 unexpected)' in p.stdout+p.stderr
    def compile(label):run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',build],label+'-build')
    compile('baseline')
    bundles=list(build.rglob('GhostermAppTests.xctest'));assert len(bundles)==1
    test=['xcrun','xctest','-XCTest','GhostermAppTests.SSHRouteAppTests/testConfiguredRouteProducts',str(bundles[0])]
    wrapper=work/'fixture'
    wrapper.write_text('#!'+sys.executable+'\nimport os,sys,subprocess\ncommand='+repr(test)+'''
os.environ.update(GHOSTERM_ROUTE_MODE=sys.argv[1],GHOSTERM_ROUTE_KNOWN=sys.argv[3],GHOSTERM_ROUTE_USER=sys.argv[4],GHOSTERM_ROUTE_PORTS=",".join(sys.argv[5:]))
try: p=subprocess.run(command,input=sys.stdin.buffer.read(),capture_output=True,timeout=20)
except subprocess.TimeoutExpired as error:
 sys.stderr.buffer.write((error.stdout or b'')+(error.stderr or b''))
 print('Swift fixture exceeded its execution budget',file=sys.stderr)
 sys.exit(2)
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
        offline={'dns':'testCancelledDNSCannotAllocateRoute','deadline':'testRouteDeadlineIncludesDNS','dns-error':'testDNSFailureIsConnectionError','target-change':'testChangedDestinationCannotKeepOldLabel'}
        if case in offline:
            p=subprocess.run(['xcrun','xctest','-XCTest','GhostermAppTests.SSHRouteAppTests/'+offline[case],str(bundles[0])],cwd=work,env=env,capture_output=True,text=True,timeout=20)
            output=p.stdout+p.stderr;(logs/(label+'.log')).write_text(output)
            if red:
                assert p.returncode==1 and output.count('error: -[')==1 and 'Executed 1 test, with 1 failure' in output and not any(x in output for x in ['unexpected signal','Program crashed','Precondition failed','AddressSanitizer','runtime error:'])
            else:assert p.returncode==0 and 'Executed 1 test, with 0 failures' in output
            return
        command=[sys.executable,work/'scripts/test-ssh-jump.py','--binary',wrapper,'--hops','2','--case',case]
        if red:command+=['--expect-assertion']
        before=set((work/'tmp').glob('sj-*'))
        run(command,label,{**env,'GHOSTERM_ROUTE_EXPECT_RED':'1' if red else '0'})
        made=set((work/'tmp').glob('sj-*'))-before;assert len(made)==1
        shutil.copytree(made.pop()/'logs',logs/label)
    names=['AppModel.swift','SSHRouteAuthenticationFlow.swift','SFTPFileBackend.swift','TmuxChooser.swift']
    paths={n:package/'Sources/GhostermAppModel'/n for n in names}
    originals={n:p.read_bytes() for n,p in paths.items()}
    hashes={n:hashlib.sha256(b).hexdigest() for n,b in originals.items()}
    cases=[
      ('tmux-admission','TmuxChooser.swift','guard !sshTarget.connectionIssues.contains(where: { $0.keyword == "proxyjump" }) else {','guard true else {','pty'),
      ('target-snapshot','AppModel.swift','finalTarget.host == target.host','true','target-change'),
      ('dns-error','SSHRouteAuthenticationFlow.swift','if error is SSHAddressResolver.Failure { fail(3,','if error is SSHAddressResolver.Failure { fail(8,','dns-error'),
      ('route-entry','AppModel.swift','if target.connectionIssues.contains(where: { $0.keyword == "proxyjump" }) {','if false && target.connectionIssues.contains(where: { $0.keyword == "proxyjump" }) {','pty'),
      ('file-alias','AppModel.swift','connectionIssues: entry?.connectionIssues ?? [], alias: pattern)','connectionIssues: entry?.connectionIssues ?? [], alias: host)','sftp'),
      ('panel-alias','AppModel.swift','connectionIssues: target.connectionIssues, alias: target.pattern)','connectionIssues: target.connectionIssues, alias: target.host)','pty-echo'),
      ('controlled-sftp','AppModel.swift','try ssh.sftp(timeoutMilliseconds: 5000)','try ssh.sftp()','sftp'),
      ('hop-identity','SSHRouteAuthenticationFlow.swift','route.supply(options: self.options[index],','route.supply(options: self.options[0],','pty'),
      ('reply-once','SSHRouteAuthenticationFlow.swift','&& prompting && !scope.isCancelled','&& true','read-wait-error'),
      ('late-dns','SSHRouteAuthenticationFlow.swift','guard let self, !self.finished else { return }; self.resolver = nil','guard let self else { return }; self.resolver = nil','dns'),
      ('total-deadline','SSHRouteAuthenticationFlow.swift','guard now() < deadline else','guard true else','deadline'),
      ('first-hop-dns','SSHRouteAuthenticationFlow.swift','lookup(options[0].host, false, owner)','lookup(options[1].host, false, owner)','dns'),
      ('reload-cancel','AppModel.swift','cancelledConnects.add(id); owner.async { flow.cancel() }','_ = id; _ = owner; _ = flow','open-cancel'),
    ]
    if args.file_operations:
        cases=[
          ('mkdir','SFTPFileBackend.swift','try queue.sync { try sftp.mkdir(path: path, mode: mode) }','_ = path; _ = mode','wait-error'),
          ('rename','SFTPFileBackend.swift','try queue.sync { try sftp.rename(from: from, to: to) }','_ = from; _ = to','wait-error'),
          ('chmod','SFTPFileBackend.swift','try queue.sync { try sftp.chmod(path: path, mode: mode) }','_ = path; _ = mode','wait-error'),
          ('upload','SFTPFileBackend.swift','try queue.sync { try sftp.upload(local: local, remote: remote) }','return 0','wait-error'),
          ('download','SFTPFileBackend.swift','try sftp.download(remote: remote, local: local)','try sftp.download(remote: remote, local: local + ".wrong")','wait-error'),
          ('remove','SFTPFileBackend.swift','try recursive ? sftp.removeAll(path: path) : sftp.remove(path: path)','try recursive ? sftp.removeAll(path: path) : ()','wait-error'),
        ]
    if args.case:
        assert set(args.case) <= {c[0] for c in cases}
        cases=[c for c in cases if c[0] in args.case]
    try:
        if args.file_operations: check('baseline-operations','wait-error')
        else:
            check('baseline-terminal','pty');check('baseline-files','sftp');check('baseline-replies','read-wait-error');check('baseline-dns','dns');check('baseline-deadline','deadline');check('baseline-dns-error','dns-error');check('baseline-target-change','target-change')
        for label,name,old,new,case in cases:
            source=originals[name].decode();assert source.count(old)==1
            paths[name].write_text(source.replace(old,new));compile(label);check(label,case,True)
            run(['xcrun','xctest','-XCTest','GhostermAppTests.HostListTests/testReadingDoesNotMutateConfig',bundles[0]],label+'-control')
            paths[name].write_bytes(originals[name]);compile(label+'-restored');check(label+'-restored',case)
            assert hashlib.sha256(paths[name].read_bytes()).hexdigest()==hashes[name]
            print(label+': assertion red / independent config green / restored / SHA256',flush=True)
    finally:
        for name,data in originals.items():
            paths[name].write_bytes(data)
            assert (ROOT/'apple/GhostermApp/Sources/GhostermAppModel'/name).read_bytes()==data
    (logs/'result.json').write_text(json.dumps({'accepted':True,'mutations':[c[0] for c in cases],'sources_sha256':hashes},indent=2)+'\n')
    for p in work.iterdir():
        if p==logs:continue
        if p.is_symlink() or p.is_file():p.unlink()
        else:shutil.rmtree(p)
    print('APP ROUTE MUTATIONS PASS: '+str(logs))
if __name__=='__main__':main()
