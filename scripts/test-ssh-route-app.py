#!/usr/bin/env python3
"""Headless Ordinary terminal and SFTP routes through owned loopback SSH chains, memory keys."""
import argparse, hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--discovery-reset', type=Path, metavar='JS_ENABLED_XCTEST',
                        help='run the SSH reset regression with a prebuilt JS-enabled suite')
    modes.add_argument('--write-reset', type=Path, metavar='XCTEST',
                       help='run only the direct SSH write failure regression with a prebuilt suite')
    parser.add_argument('--discovery-hops', type=int, choices=(1, 2), default=1,
                        help='select a direct or two-hop route for --discovery-reset')
    args = parser.parse_args()
    if args.discovery_hops != 1 and not args.discovery_reset:
        parser.error('--discovery-hops 2 requires --discovery-reset')
    bundle = args.discovery_reset or args.write_reset
    if bundle:
        case = 'discovery-reset' if args.discovery_reset else 'write-reset'
        with tempfile.TemporaryDirectory(prefix='route-reset-', dir=os.environ['TMPDIR']) as folder:
            test = ['xcrun', 'xctest', '-XCTest',
                    'GhostermAppTests.SSHRouteAppTests/testConfiguredRouteProducts',
                    str(bundle.resolve(strict=True))]
            wrapper = Path(folder) / 'fixture'
            write_fixture(wrapper, test)
            subprocess.run([sys.executable, str(ROOT/'scripts/test-ssh-jump.py'),
                            '--binary', str(wrapper), '--case', case, '--hops', str(args.discovery_hops)],
                           cwd=ROOT, env={**os.environ, 'TMPDIR': folder, 'TMP': folder, 'TEMP': folder,
                                          'GHOSTERM_AUTOMATION_OFFSCREEN': '1'}, check=True)
        return
    work=Path(tempfile.mkdtemp(prefix='ra-',dir=ROOT/'tmp')); logs=work/'logs';logs.mkdir()
    env={**os.environ,'TMPDIR':str(work),'DEVELOPER_DIR':'/Applications/Xcode.app/Contents/Developer','GHOSTERM_AUTOMATION_OFFSCREEN':'1'}
    sources=[p for folder in ['apple/GhostermCore/Sources','apple/GhostermCore/Tests','apple/GhostermApp/Sources','apple/GhostermApp/Tests','core/src','core/include'] for p in (ROOT/folder).rglob('*') if p.is_file()]
    sources += [Path(__file__).resolve(), ROOT/'scripts/test-ssh-jump.py']
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    build=work/'swift';package=ROOT/'apple/GhostermApp'
    command=['xcrun','swift','build','--build-tests','--package-path',str(package),'--scratch-path',str(build)]
    with (logs/'build.log').open('w') as log:subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    bundles=list(build.rglob('GhostermAppTests.xctest'));assert len(bundles)==1
    for label,selection in [
        ('route-config','SSHConfiguredRouteTests'),
        ('target-change','SSHRouteAppTests/testChangedDestinationCannotKeepOldLabel'),
        ('dns-error','SSHRouteAppTests/testDNSFailureIsConnectionError'),
        ('cancel-dns','SSHRouteAppTests/testCancelledDNSCannotAllocateRoute'),
        ('deadline','SSHRouteAppTests/testRouteDeadlineIncludesDNS'),
        ('unsupported','SSHAuthTests/testUnsupportedConfigurationStopsBeforeResolverAndPrompts'),
        ('configuration','SSHAuthTests/testSFTPConfigurationAdmissionUsesAliasAndShowsSource'),
        ('credentials','SSHCredentialSelectorTests')]:
        result=subprocess.run(['xcrun','xctest','-XCTest','GhostermAppTests.'+selection,str(bundles[0])],cwd=ROOT,env=env,capture_output=True,text=True,timeout=60)
        output=result.stdout+result.stderr;(logs/(label+'.log')).write_text(output)
        assert result.returncode==0 and 'with 0 failures' in output and 'Executed 0 tests' not in output and 'skipped' not in output,(label,logs)
    test=['xcrun','xctest','-XCTest','GhostermAppTests.SSHRouteAppTests/testConfiguredRouteProducts',str(bundles[0])]
    wrapper=work/'fixture'
    write_fixture(wrapper, test)
    with (logs/'test.log').open('w') as log:
        subprocess.run([sys.executable,str(ROOT/'scripts/test-ssh-jump.py'),'--binary',str(wrapper),'--case','tmux','--case','pty','--case','sftp','--case','pty-echo','--case','pty-idle','--case','cancel','--case','open-cancel','--case','read-wait-error','--case','pty-cancel','--case','wait-error'],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    receipts=[line.removeprefix('SSH JUMP IO PASS: ') for line in (logs/'test.log').read_text().splitlines() if line.startswith('SSH JUMP IO PASS: ')]
    assert len(receipts)==1
    receipt=Path(receipts[0]).resolve();assert receipt.is_relative_to(ROOT/'tmp') and receipt.name=='logs'
    shutil.copytree(receipt,logs/'fixture')
    for name,digest in hashes.items():assert hashlib.sha256((ROOT/name).read_bytes()).hexdigest()==digest
    (logs/'result.json').write_text(json.dumps({'accepted':True,'cases':20,'sources_sha256':hashes},indent=2)+'\n')
    for child in work.iterdir():
        if child==logs:continue
        if child.is_dir():shutil.rmtree(child)
        else:child.unlink()
    print('APP ROUTE PASS: '+str(logs))
def write_fixture(wrapper, test):
    wrapper.write_text('#!'+sys.executable+'\nimport os,sys\ncommand='+repr(test)+'\nos.environ.update(GHOSTERM_ROUTE_MODE=sys.argv[1],GHOSTERM_ROUTE_KNOWN=sys.argv[3],GHOSTERM_ROUTE_USER=sys.argv[4],GHOSTERM_ROUTE_PORTS=",".join(sys.argv[5:]))\nos.execvpe(command[0],command,os.environ)\n')
    wrapper.chmod(0o700)

if __name__=='__main__':main()
