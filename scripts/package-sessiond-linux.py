#!/usr/bin/env python3
"""Build a self-contained user-space Linux sessiond bundle on a Linux builder."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
FILES=('ghosterm-sessiond','ghosterm-session-proxy','lib/libghostty-vt.so.0','LICENSE','SOURCE-INFO.json')

def run(args):
    return subprocess.check_output([str(a) for a in args],cwd=ROOT,text=True,stderr=subprocess.STDOUT)

def build(args):
    if platform.system()!='Linux':raise RuntimeError('Linux builder required; client machines consume the resulting bundle')
    arch=platform.machine()
    if arch not in ('aarch64','x86_64'):raise RuntimeError('unsupported Linux architecture: '+arch)
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp');os.umask(0o077)
    build_dir=args.build_dir.resolve();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if out==ROOT or out in ROOT.parents:raise RuntimeError('unsafe output directory')
    log=ROOT/'tmp/sessiond-package-build.log'
    with log.open('w') as stream:
        subprocess.run(['cmake','-S',str(ROOT/'daemon'),'-B',str(build_dir),'-DGHOSTERM_WITH_LIBSSH2=OFF','-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON','-DCMAKE_INSTALL_RPATH=$ORIGIN/lib',
            '-DGHOSTERM_VT_INCLUDE_DIR='+str(args.include.resolve()),'-DGHOSTERM_VT_LIBRARY='+str(args.engine.resolve())],check=True,stdout=stream,stderr=subprocess.STDOUT)
        subprocess.run(['cmake','--build',str(build_dir),'--target','ghosterm-sessiond','ghosterm-session-proxy','-j4'],check=True,stdout=stream,stderr=subprocess.STDOUT)
    stage=Path(tempfile.mkdtemp(prefix='.bundle-',dir=out))
    try:
        (stage/'lib').mkdir()
        for name in FILES[:2]:shutil.copyfile(build_dir/name,stage/name);(stage/name).chmod(0o700)
        shutil.copyfile(args.engine.resolve(),stage/FILES[2]);(stage/FILES[2]).chmod(0o600)
        # Release payloads are shipped, not debugged on the remote host. Strip
        # before hashing so manifest.tsv describes the exact uploaded bytes.
        if not args.no_strip:
            for name in FILES[:3]:subprocess.run(['strip','--strip-unneeded',str(stage/name)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT)
        shutil.copyfile(ROOT/'LICENSE',stage/'LICENSE')
        commit=run(['git','rev-parse','HEAD']).strip();upstream=run(['git','-C',ROOT/'vendor/ghostty','rev-parse','HEAD']).strip()
        info={'project':'Ghosterm','license':'MIT','source_commit':commit,'source_repository':'https://github.com/hanxi/ghosterm',
              'engine':{'repository':'https://github.com/ghostty-org/ghostty','commit':upstream,'license':'MIT'},
              'patches':'patches/libghostty/8159838','build_script':'scripts/package-sessiond-linux.py','zig':'0.16.0',
              'stripped':not args.no_strip}
        (stage/'SOURCE-INFO.json').write_text(json.dumps(info,indent=2)+'\n')
        versions=[];interpreter=None
        allowed={'libc.so.6','libm.so.6','librt.so.1','libdl.so.2','libpthread.so.0','libgcc_s.so.1','libghostty-vt.so.0','ld-linux-aarch64.so.1','ld-linux-x86-64.so.2'}
        for name in FILES[:3]:
            binary=stage/name;dynamic=run(['readelf','-d',binary]);needed=re.findall(r'Shared library: \[([^]]+)\]',dynamic)
            if set(needed)-allowed:raise RuntimeError('unbundled dependencies: '+repr(set(needed)-allowed))
            versions.extend(tuple(map(int,v.split('.'))) for v in re.findall(r'GLIBC_([0-9]+\.[0-9]+(?:\.[0-9]+)?)',run(['readelf','--version-info',binary])))
            if name=='ghosterm-sessiond':
                match=re.search(r'Requesting program interpreter: ([^]]+)',run(['readelf','-l',binary]));interpreter=match.group(1) if match else None
        if not interpreter or not versions:raise RuntimeError('glibc/loader evidence missing')
        version=run([stage/'ghosterm-sessiond','--version']).strip();contract=version.split()[-1]
        if not re.fullmatch('[0-9a-f]{64}',contract):raise RuntimeError('invalid session contract probe')
        run([stage/'ghosterm-session-proxy','--help'])
        manifest={'format':1,'os':'Linux','arch':arch,'libc':'glibc','min_glibc':'.'.join(map(str,max(versions))),
                  'interpreter':interpreter,'commit':commit,'contract':contract,'files':[]}
        for name in FILES:
            p=stage/name;manifest['files'].append({'name':name,'size':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'mode':0o700 if name in FILES[:2] else 0o600})
        payload=json.dumps(manifest,sort_keys=True,separators=(',',':'))+'\n';release=commit[:12]+'-'+arch+'-'+hashlib.sha256(payload.encode()).hexdigest()[:12]
        manifest['release']=release;(stage/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        catalog='GTB1\t'+release+'\t'+arch+'\t'+manifest['min_glibc']+'\t'+interpreter+'\t'+contract+'\n'
        for entry in manifest['files']:catalog+=entry['name']+'\t'+str(entry['size'])+'\t'+entry['sha256']+'\n'
        (stage/'manifest.tsv').write_text(catalog)
        final=out/release
        if final.exists():
            if (final/'manifest.json').read_bytes()!=(stage/'manifest.json').read_bytes():raise RuntimeError('existing release differs')
        else:stage.rename(final)
        print(final)
        return final
    finally:
        if stage.exists():shutil.rmtree(stage)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--include',type=Path,required=True);p.add_argument('--engine',type=Path,required=True)
    p.add_argument('--build-dir',type=Path,default=ROOT/'tmp/sessiond-package-build');p.add_argument('--out',type=Path,default=ROOT/'tmp/sessiond-bundles')
    p.add_argument('--no-strip',action='store_true',help='keep symbols for debugging; release bundles should be stripped')
    build(p.parse_args())
if __name__=='__main__':main()
