#!/usr/bin/env python3
"""Compiled ProxyJump regressions; dedicated test device, owned SSH fixtures only."""
import argparse, hashlib, json, os, signal, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_PROXYJUMP_MUTATION'
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--case',action='append');args=parser.parse_args()
    app=ROOT/'android/app/src/main/java/com/hanxi/ghosterm'
    bridge=ROOT/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge'
    native=ROOT/'android/bridge/src/main/cpp'
    product='productResolvesOnlyFirstHopAndSeparatesAuthentication';real='realRouteTerminalFilesAndTmux'
    cases=[
        ('username-prompt',app/'SessionsViewModel.kt','!failed && missingUsernameTarget == null &&','!failed && username.isNotBlank() &&','missingJumpUsernamePromptsBeforeDnsWithoutChangingTheRequest'),
        ('dns',app/'SessionsViewModel.kt','resolver.resolve(attempt.target.jumps.firstOrNull()?.host ?: attempt.target.host)','resolver.resolve(attempt.target.host)',product),
        ('identity',app/'SessionsViewModel.kt','target.jumps.getOrNull(auth?.hop ?: 0) ?: target','target',product),
        ('hop-reset',app/'SessionsViewModel.kt','attempt.triedIdentityIds.clear(); attempt.automaticKeyRequests.clear()','attempt.automaticKeyRequests.clear()',product),
        ('hop-index',native/'ssh_session.c','snprintf(number[6], sizeof number[6], "%u", hop);','snprintf(number[6], sizeof number[6], "%u", 0u);\n    (void)hop;',product),
        ('user-override',native/'config_jni.c','if (user) {','if (false && user) {','configurationPreservesRouteAndRejectsMalformedOrUnsupportedHops'),
        ('terminal-route',bridge/'RemoteSession.kt','timeoutMillis, route())','timeoutMillis, emptyArray())',real),
        ('files-route',bridge/'RemoteSession.kt','timeoutMillis, completionFd, route())','timeoutMillis, completionFd, emptyArray())',real),
        ('tmux-attach-route',bridge/'RemoteTmux.kt','knownHosts,listOf(target.address),timeoutMillis,jumps)','knownHosts,listOf(target.address),timeoutMillis)',real),
    ]
    if args.case:
        assert set(args.case)<={c[0] for c in cases};cases=[c for c in cases if c[0] in args.case]
    originals={path:path.read_bytes() for _,path,_,_,_ in cases}
    assert all(MARK.encode() not in value for value in originals.values())
    for name,path,old,new,method in cases: assert old in originals[path].decode(),name
    work=Path(tempfile.mkdtemp(prefix='ajm-',dir=ROOT/'tmp'));env={**os.environ,'TMPDIR':str(work)}
    env.setdefault('ZIG',str(ROOT/'build/android-toolchain/zig-aarch64-macos-0.16.0/zig'))
    def run(command,label):
        with (work/(label+'.log')).open('w') as log:
            result=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require android-proxyjump; shift; gt_timeout "$@"','android-proxyjump',str(ROOT),'900',*map(str,command)],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
        if result.returncode: raise RuntimeError((label,(work/(label+'.log')).read_text()))
    def build(label): run(['bash','scripts/build-android.sh'],label+'-build')
    def check(label,method,red=False):
        run([sys.executable,'scripts/test-android-proxyjump.py','--hops','2','--method',method]+(['--expect-assertion'] if red else []),label)
    def restore():
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes(): path.write_bytes(data)
            assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(data).digest()
    def interrupt(signum,frame): raise KeyboardInterrupt(signum)
    for sig in [signal.SIGINT,signal.SIGTERM]:signal.signal(sig,interrupt)
    results=[]
    try:
        build('baseline')
        for method in sorted({c[4] for c in cases}):check('baseline-'+method,method)
        for name,path,old,new,method in cases:
            path.write_text(originals[path].decode().replace(old,new)+'\n// '+MARK+'\n')
            build(name);check(name,method,True)
            restore();build(name+'-restored');check(name+'-restored',method)
            results.append(name);(work/'accepted.json').write_text(json.dumps(results)+'\n')
            print(name+': compiled assertion red / same APK Core green / restored green / SHA256',flush=True)
    finally:restore()
    print('ANDROID PROXYJUMP MUTATIONS PASS: '+str(work))
if __name__=='__main__':main()
