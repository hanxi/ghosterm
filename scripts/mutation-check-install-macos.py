#!/usr/bin/env python3
"""Install a real signed archive only into owned tmp/ targets; never /Applications."""
import argparse, hashlib, json, os, pathlib, plistlib, re, shutil, signal, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT/'scripts/install-macos.sh'
MARK = 'GT_INSTALL_MUTATION_MARK'

def run(args, env):
    p = subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require install-macos; shift; gt_timeout 90 "$@"','install-test',str(ROOT)]+list(map(str,args)),env=env,capture_output=True,text=True)
    return p.returncode,p.stdout+p.stderr

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--archive',required=True,type=pathlib.Path);a=parser.parse_args()
    if not a.archive.is_file():raise RuntimeError('clean signed ZIP fixture required')
    original=SOURCE.read_bytes();digest=hashlib.sha256(original).digest()
    if MARK.encode() in original:raise RuntimeError('leftover mutation marker: exit 2')
    for sig in [signal.SIGINT,signal.SIGTERM]:signal.signal(sig,lambda s,f:(_ for _ in ()).throw(InterruptedError(s)))
    with tempfile.TemporaryDirectory(prefix='install-tests-',dir=ROOT/'tmp') as directory:
        folder=pathlib.Path(directory);tools=folder/'bin';tools.mkdir();script=folder/'install-macos.sh';script.write_bytes(original)
        env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),GHOSTERM_INSTALL_WORK_DIR=str(folder/'downloads'),PATH=str(tools)+os.pathsep+os.environ['PATH'])
        def command(args):
            code,out=run(args,env)
            if code:print(out,flush=True);raise RuntimeError('installer fixture/tool failed')
        unpacked=folder/'unpacked';command(['/usr/bin/ditto','-x','-k',a.archive.resolve(),unpacked])
        app=unpacked/'Ghosterm.app';info=plistlib.loads((app/'Contents/Info.plist').read_bytes())
        assert info['CFBundleIdentifier']=='dev.ghosterm.mac'
        match=re.search(r'^源码:\s*git ([a-f0-9]+)$',(app/'Contents/Resources/BUILD-INFO.txt').read_text(),re.M)
        assert match,'fixture must have clean build metadata'
        commit=subprocess.check_output(['git','-C',ROOT,'rev-parse',match[1]+'^{commit}'],text=True).strip()
        zip_name='Ghosterm-test-arm64.zip';fixtures={}
        for mode in ['good','foreign','signature']:
            fixture=folder/('fixture-'+mode);fixture.mkdir();archive=fixture/zip_name
            if mode=='good':shutil.copy2(a.archive,archive)
            else:
                tree=folder/('source-'+mode);tree.mkdir();candidate=tree/'Ghosterm.app';shutil.copytree(app,candidate,symlinks=True)
                if mode=='foreign':
                    path=candidate/'Contents/Info.plist';data=plistlib.loads(path.read_bytes());data['CFBundleIdentifier']='test.other.app';path.write_bytes(plistlib.dumps(data))
                    command(['/usr/bin/codesign','--force','--deep','--sign','-',candidate])
                else:(candidate/'Contents/Resources/unsigned-extra').write_text('signature must reject this')
                command(['/usr/bin/ditto','-c','-k','--sequesterRsrc','--keepParent',candidate,archive])
            manifest={'channel':'dev','arch':'arm64','min_macos':info['LSMinimumSystemVersion'],'git_commit':commit,'assets':{'zip':zip_name}}
            (fixture/'version.json').write_text(json.dumps(manifest));(fixture/'SHA256SUMS').write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  '+zip_name+'\n');fixtures[mode]=fixture
        for mode in ['hash','old-os']:
            fixture=folder/('fixture-'+mode);shutil.copytree(fixtures['good'],fixture);fixtures[mode]=fixture
            if mode=='hash':(fixture/'SHA256SUMS').write_text('0'*64+'  '+zip_name+'\n')
            else:
                manifest=json.loads((fixture/'version.json').read_text());manifest['min_macos']='999.0';(fixture/'version.json').write_text(json.dumps(manifest))
        wrappers={
            'curl':'''url=; out=
while [ "$#" -gt 0 ]; do case "$1" in -o) out=$2; shift 2;; https://*) url=$1; shift;; *) shift;; esac; done
case "$url" in https://github.com/hanxi/ghosterm/releases/download/dev/*) ;; *) exit 98;; esac
exec /bin/cp "$GT_INSTALL_FIXTURE/${url##*/}" "$out"
''',
            'ditto':'''/usr/bin/ditto "$@" || exit $?
for last; do :; done
app=$last
[ ! -d "$last/Ghosterm.app" ] || app="$last/Ghosterm.app"
if [ -d "$app/Contents" ]; then
/usr/bin/xattr -w com.apple.quarantine '0081;fixture;test;' "$app"
/usr/bin/xattr -w com.apple.quarantine '0081;fixture;test;' "$app/Contents/MacOS/GhostermApp"
fi
''',
            'mv':'''if [ "$GT_INSTALL_MODE" = rollback ] && [ "${1##*/}" = new.app ]; then echo 'controlled promotion failure' >&2; exit 1; fi
exec /bin/mv "$@"
''',
            'pgrep':'''[ "$GT_INSTALL_MODE" = running ]
''',
            'uname':'''if [ "$GT_INSTALL_MODE" = intel ] && [ "$1" = -m ]; then echo x86_64; else exec /usr/bin/uname "$@"; fi
''',
            'sysctl':'''if [ "$GT_INSTALL_MODE" = intel ]; then echo 0; else exec /usr/sbin/sysctl "$@"; fi
''',
            'sudo':'''echo 'unexpected sudo in isolated user-writable fixture' >&2; exit 99
''',
        }
        for name,body in wrappers.items():p=tools/name;p.write_text('#!/bin/bash\n'+body);p.chmod(0o700)
        def check(mode):
            target_dir=folder/'Applications';target_dir.mkdir();target=target_dir/'Ghosterm.app';(target/'Contents').mkdir(parents=True)
            (target/'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier':'dev.ghosterm.mac'}));(target/'old-marker').write_text('previous app')
            fixture=fixtures.get(mode,fixtures['good'])
            try:
                code,out=run(['/bin/bash',script,'--app-dir',target_dir],dict(env,GT_INSTALL_FIXTURE=str(fixture),GT_INSTALL_MODE=mode))
                if code not in [0,1]:print(out,flush=True);raise RuntimeError('installer tool/syntax failure: '+str(code))
                if mode=='good':
                    valid=code==0 and not (target/'old-marker').exists() and (target/'Contents/MacOS/GhostermApp').is_file()
                    if valid:
                        for path in [target,target/'Contents/MacOS/GhostermApp']:
                            attr_code,attrs=run(['/usr/bin/xattr',path],env)
                            if attr_code:print(attrs,flush=True);raise RuntimeError('xattr observation failed')
                            valid=valid and 'com.apple.quarantine' not in attrs.splitlines()
                else:valid=code==1 and (target/'old-marker').is_file()
                return valid,out
            finally:
                shutil.rmtree(target_dir)
        modes=['good','hash','foreign','signature','old-os','intel','running','rollback']
        for mode in modes:
            valid,out=check(mode)
            if not valid:print(out,flush=True);raise AssertionError('installer baseline behavior: '+mode)
        print('PASS installer integration (real signatures, recursive xattr, preservation and rollback)',flush=True)
        cases=[
            ('checksum',[('[ "$ACTUAL" = "$EXPECTED" ]','true')],['hash'],'good'),
            ('identity',[('[ "$BUNDLE_ID" = dev.ghosterm.mac ]','true')],['foreign'],'good'),
            ('signature',[('codesign --verify --strict --deep "$CANDIDATE"','true'),('as_owner codesign --verify --strict --deep "$STAGE/new.app"','true')],['signature'],'good'),
            ('recursive-xattr',[('xattr -dr com.apple.quarantine','xattr -d com.apple.quarantine')],['good'],'hash'),
            ('rollback',[('as_owner mv "$STAGE/previous.app" "$TARGET"','false')],['rollback'],'good'),
            ('eligibility',[
                ('case "$HOST_ARCH:$TRANSLATED" in arm64:*|*:1) ;; *) fail \'当前包只支持 Apple Silicon（arm64）\' ;; esac','true'),
                ('version_at_least "$HOST_VERSION" "$MIN"','true'),
                ('if pgrep -x GhostermApp >/dev/null 2>&1; then','if false; then'),
            ],['old-os','intel','running'],'good'),
        ]
        try:
            for name,changes,targets,control in cases:
                text=original.decode()
                for old,new in changes:assert text.count(old)==1,(name,old);text=text.replace(old,new)
                script.write_text(text+'\n# '+MARK+'\n')
                for target in targets:
                    valid,out=check(target)
                    if valid:print(out,flush=True);raise AssertionError('installer mutation stayed green: '+name)
                valid,out=check(control)
                if not valid:print(out,flush=True);raise AssertionError('installer independent control failed: '+name)
                script.write_bytes(original);assert hashlib.sha256(script.read_bytes()).digest()==digest
                for target in targets:
                    valid,out=check(target)
                    if not valid:print(out,flush=True);raise AssertionError('installer rollback failed: '+name)
                print('PASS installer '+name+': target red, control green, SHA rollback',flush=True)
        finally:script.write_bytes(original);assert hashlib.sha256(SOURCE.read_bytes()).digest()==digest
    print('PASS macOS installer mutations (6 groups)')
if __name__=='__main__':main()
