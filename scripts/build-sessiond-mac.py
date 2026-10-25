#!/usr/bin/env python3
"""Build relocatable, SSH-free macOS helpers from the matching patched VT SDK."""
import argparse, hashlib, json, os, platform, re, shutil, signal, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

def run(args, **kwargs):
    p = subprocess.Popen([str(x) for x in args], start_new_session=True, **kwargs)
    try:
        out, _ = p.communicate(timeout=180)
        if p.returncode: raise RuntimeError('macOS helper command failed: ' + str(args[0]))
        return out
    finally:
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            try: p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL); p.wait()

def write_notice(stage):
    commit=run(['git','-C',ROOT,'rev-parse','HEAD'],stdout=subprocess.PIPE,text=True).strip()
    upstream=run(['git','-C',ROOT/'vendor/ghostty','rev-parse','HEAD'],stdout=subprocess.PIPE,text=True).strip()
    shutil.copyfile(ROOT/'LICENSE',stage/'LICENSE')
    info=dict(project='Ghosterm',license='MIT',source_commit=commit,
        source_repository='https://github.com/hanxi/ghosterm',
        engine=dict(repository='https://github.com/ghostty-org/ghostty',commit=upstream,license='MIT'),
        patches='patches/libghostty/8159838',build_script='scripts/build-sessiond-mac.py',zig='0.16.0')
    (stage/'SOURCE-INFO.json').write_text(json.dumps(info,sort_keys=True,indent=2)+'\n')

def sign_label(identity):
    """签名档必须来自证书本身的类：Apple Development（免费账号，不可公证）不是 developer-id。"""
    if identity == '-':
        return 'ad-hoc'
    if identity.startswith('Developer ID Application: '):
        return 'developer-id'
    if identity.startswith('Apple Development: '):
        return 'apple-development'
    raise RuntimeError('unrecognized codesign identity (只认 "Developer ID Application: …" 或 "Apple Development: …"): '+identity)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, default=ROOT/'tmp/worker-surface/out/lib/libghostty-vt.dylib')
    parser.add_argument('--sign', default='-', help='codesign identity, default ad-hoc')
    parser.add_argument('--out', type=Path, default=ROOT/'build/runtime/macos-arm64')
    args = parser.parse_args()
    if sys.platform != 'darwin' or platform.machine() != 'arm64': raise RuntimeError('macOS arm64 builder required')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda sig, frame: (_ for _ in ()).throw(RuntimeError('helper build interrupted: '+str(sig))))
    os.umask(0o077); (ROOT/'tmp').mkdir(exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT/'tmp')
    os.environ.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    engine = args.engine.resolve(strict=True)
    headers = ROOT/'build/GhosttyKit.xcframework/macos-arm64/Headers'
    # Headless runtime and GUI must consume the same patched public ABI tree.
    engine_headers=list((ROOT/'tmp/worker-surface/engine/include').rglob('*.h'))
    if not engine_headers:raise RuntimeError('validated headless source headers missing')
    for header in engine_headers:
        if header.read_bytes() != (headers/header.relative_to(ROOT/'tmp/worker-surface/engine/include')).read_bytes():
            raise RuntimeError('headless/native SDK header mismatch')
    work = ROOT/'tmp'/('package-sessiond-mac-'+hashlib.sha256(engine.read_bytes()).hexdigest()[:16]); work.mkdir(exist_ok=True)
    run(['cmake', '-S', ROOT/'daemon', '-B', work, '-DCMAKE_BUILD_TYPE=Release',
         '-DGHOSTERM_WITH_LIBSSH2=OFF', '-DGHOSTERM_VT_INCLUDE_DIR='+str(headers),
         '-DGHOSTERM_VT_LIBRARY='+str(engine), '-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON',
         '-DCMAKE_INSTALL_RPATH=@loader_path/lib', '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0'])
    run(['cmake', '--build', work, '--target', 'ghosterm-sessiond', 'ghosterm-session-proxy', '-j', '4'])
    contract = re.search(r'GT_SESSION_CONTRACT_HEX "([0-9a-f]{64})"', (work/'generated/session-contract.h').read_text()).group(1)
    staging = Path(tempfile.mkdtemp(prefix='mac-session-payload-', dir=ROOT/'tmp'))
    try:
        (staging/'lib').mkdir(mode=0o755);(staging/'lib').chmod(0o755);staging.chmod(0o755)
        for name in ('ghosterm-sessiond', 'ghosterm-session-proxy'):
            shutil.copy2(work/name, staging/name); (staging/name).chmod(0o755)
        shutil.copy2(engine, staging/'lib/libghostty-vt.dylib')
        (staging/'lib/libghostty-vt.dylib').chmod(0o644)
        # Reject build-machine paths and accidental Homebrew/SSH dependencies.
        for name in ('ghosterm-sessiond', 'ghosterm-session-proxy', 'lib/libghostty-vt.dylib'):
            binary = staging/name
            deps = run(['otool', '-L', binary], stdout=subprocess.PIPE, text=True)
            for line in deps.splitlines()[1:]:
                dep = line.strip().split(' (', 1)[0]
                if not (dep.startswith('/usr/lib/') or dep == '@rpath/libghostty-vt.dylib'):
                    raise RuntimeError('helper dependency is not self contained: '+dep)
            run(['codesign', '--force', '--sign', args.sign]+([] if args.sign=='-' else ['--options','runtime','--timestamp'])+[binary], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        write_notice(staging)
        for name in ('LICENSE','SOURCE-INFO.json'):(staging/name).chmod(0o644)
        files = {name: hashlib.sha256((staging/name).read_bytes()).hexdigest()
                 for name in ('ghosterm-sessiond', 'ghosterm-session-proxy', 'lib/libghostty-vt.dylib','LICENSE','SOURCE-INFO.json')}
        # Immutable content-addressed releases keep existing processes usable.
        release = contract[:12]+'-'+hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()[:16]
        metadata = dict(format=1, platform='macos', arch='arm64', contract=contract, release=release, files=files, signature=sign_label(args.sign))
        (staging/'runtime.json').write_text(json.dumps(metadata, indent=2)+'\n')
        for name in ('ghosterm-sessiond', 'ghosterm-session-proxy'):
            run([staging/name, '--help'], stdout=subprocess.DEVNULL)
        destination = args.out.resolve(); destination.mkdir(parents=True, exist_ok=True)
        target = destination/release
        if target.exists():
            if target.is_symlink() or (target/'runtime.json').read_bytes() != (staging/'runtime.json').read_bytes():
                raise RuntimeError('existing release differs')
            for name, digest in files.items():
                if (target/name).is_symlink() or hashlib.sha256((target/name).read_bytes()).hexdigest() != digest:
                    raise RuntimeError('existing helper changed')
        else: staging.rename(target)
        pointer = ROOT/'tmp'/('.current-'+str(os.getpid()))
        try:
            pointer.write_text(json.dumps(dict(format=1, release=release))+'\n')
            pointer.replace(destination/'current.json')
        finally:
            if pointer.exists(): pointer.unlink()
        print('PASS self-contained macOS session helpers: '+str(target), flush=True)
    finally:
        if staging.exists(): shutil.rmtree(staging)
    return 0
if __name__ == '__main__': sys.exit(main())
