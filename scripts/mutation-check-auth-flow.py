#!/usr/bin/env python3
"""The product's single authentication flow, exercised against the fixture sshd."""
import hashlib, os, pathlib, shutil, signal, subprocess, sys, tempfile
ROOT = pathlib.Path(__file__).resolve().parent.parent
MARK = 'GT_AUTH_FLOW_MUTATION_MARK'
FILES = ['AppModel.swift', 'SSHCredentialSelector.swift', 'SSHAuthenticationFlow.swift', '../../../GhostermCore/Sources/GhostermCore/SSHAuthenticator.swift']
PREFIX = pathlib.Path('apple/GhostermApp/Sources/GhostermAppModel')

def run(args):
    command=['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require auth-flow; shift; gt_timeout 240 "$@"','auth-flow',str(ROOT)]+list(map(str,args))
    result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    return result.returncode,result.stdout+result.stderr

def main():
    if len(sys.argv)!=6: raise SystemExit('fixture arguments: host port user key work')
    host,port,user,key,work=sys.argv[1:]
    if not pathlib.Path(key).is_file():raise SystemExit('fixture key unavailable')
    os.environ.update(DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',TMPDIR=str(ROOT/'tmp'),
        GHOSTERM_SSH_HOST=host,GHOSTERM_SSH_PORT=port,GHOSTERM_SSH_USER=user,GHOSTERM_SSH_KEY=key,GHOSTERM_SSH_KNOWN_HOSTS=str(pathlib.Path(work)/'app-known-hosts'))
    original={f:(ROOT/PREFIX/f).read_bytes() for f in FILES}
    sha={f:hashlib.sha256(b).digest() for f,b in original.items()}
    if any(MARK.encode() in b for b in original.values()):raise SystemExit(2)
    def interrupt(sig,frame):raise InterruptedError(sig)
    for sig in [signal.SIGINT,signal.SIGTERM]:signal.signal(sig,interrupt)
    with tempfile.TemporaryDirectory(prefix='auth-flow-',dir=ROOT/'tmp') as temp:
        folder=pathlib.Path(temp);(folder/'apple').mkdir()
        for package in ['GhostermApp','GhostermCore','GhostermTerminal']:
            shutil.copytree(ROOT/'apple'/package,folder/'apple'/package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
        for name in ['core','terminal','build','assets']:(folder/name).symlink_to(ROOT/name,target_is_directory=True)
        package=folder/'apple/GhostermApp'
        def check(red=False,control=False):
            code,out=run(['xcrun','swift','test','--package-path',package,'--filter','HostListTests' if control else 'SSHAuthTests'])
            valid=(code==1 and ('XCTAssert' in out or ': failed - ' in out) and 'Build complete' in out) if red else code==0 and 'Test Suite' in out and 'skipped' not in out
            if not valid:print(out,flush=True);raise RuntimeError('auth mutation expected '+('red' if red else 'green'))
        check();check(control=True)
        mutants=[
            ('tilde','AppModel.swift','home + p.dropFirst(1)','home + p.dropFirst(2)'),
            ('unencrypted','SSHCredentialSelector.swift','let requirement = Ghosterm.keyPassphraseRequirement(pem)','let requirement = PrivateKeyPassphraseRequirement.needed'),
            ('methods','SSHAuthenticationFlow.swift','self.prompts.methods(id, info.methods, info.error)','self.prompts.methods(id, 0, info.error)'),
            ('revoked-message','SSHAuthenticationFlow.swift','info.host == .revoked ?', 'false ?'),
            ('revoked-binding','../../../GhostermCore/Sources/GhostermCore/SSHAuthenticator.swift','mismatch, revoked }','mismatch, revoked = 99 }'),
        ]
        try:
            for name,file,old,new in mutants:
                path=folder/PREFIX/file;s=original[file].decode()
                if s.count(old)!=1:raise RuntimeError('mutation source missing: '+name)
                path.write_text(s.replace(old,new,1)+'\n// '+MARK+'\n')
                check(red=True);check(control=True)
                path.write_bytes(original[file]);assert hashlib.sha256(path.read_bytes()).digest()==sha[file]
                check();print('PASS auth '+name+': target red, control green, SHA rollback',flush=True)
        finally:
            for file,b in original.items():
                (folder/PREFIX/file).write_bytes(b)
                assert hashlib.sha256((ROOT/PREFIX/file).read_bytes()).digest()==sha[file]
    print('PASS shared product authentication mutations (5 groups)')
if __name__=='__main__':main()
