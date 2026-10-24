#!/usr/bin/env python3
"""Experimental native full-surface resource resolver; P0 is still incomplete."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shlex
import signal
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
BASE=runpy.run_path(str(ROOT/'scripts/probe-session-surface.py'))
PATCHDIR=ROOT/'patches/libghostty/8159838/experimental'
MARK='GT_P0_FULL_RESOURCE_MUTATION_MARK'

def compile_probe(engine,work,source_quota=False,static_resources=False):
    binary=work/'surface-resources'
    flags=[x for name in BASE['FRAMEWORKS'] for x in ('-framework',name)]
    BASE['run']([*shlex.split(os.environ.get('CC','cc')),'-fobjc-arc','-Wall','-Wextra','-Werror',
        '-DGHOSTERM_P0_RESOURCE_HOOKS',*(['-DGHOSTERM_P0_SOURCE_QUOTA'] if source_quota else []),*(['-DGHOSTERM_P0_STATIC_RESOURCES'] if static_resources else []),'-I',str(engine/'include'),
        str(ROOT/'terminal/session-state/full-surface/probe.m'),
        str(engine/'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),'-lc++',*flags,'-o',str(binary)])
    return binary

def build(engine,work,source_quota=False,static_resources=False):
    BASE['build'](engine,work)
    return compile_probe(engine,work,source_quota,static_resources)

def fixtures(engine,work,source_quota=False,static_resources=False):
    binary=work/'resource-fixtures'
    BASE['run']([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-D_DEFAULT_SOURCE',
        '-DGHOSTERM_P0_BUILTIN_PNG',*(['-DGHOSTERM_P0_SOURCE_QUOTA'] if source_quota else []),*(['-DGHOSTERM_P0_STATIC_RESOURCES'] if static_resources else []),'-Wall','-Wextra','-Werror','-I',str(engine/'include'),
        '-I',str(ROOT/'terminal/session-state'),str(ROOT/'terminal/session-state/full-surface/resource-fixture.c'),
        str(work/'out/lib/libghostty-vt.a'),'-o',str(binary)])
    BASE['run']([str(binary),str(work)])

def mutations(engine,work,source_quota=False):
    source=engine/'src/terminal/kitty/graphics_image.zig';original=source.read_bytes();text=original.decode()
    old="result.addData(alloc, if (bytes) |p| p[0..len] else &.{})"
    if text.count(old)!=1:raise RuntimeError('nonunique resolved payload anchor')
    try:
        source.write_text(text.replace(old,"result.addData(alloc, &.{})",1)+f'\n// {MARK}\n')
        binary=build(engine,work,source_quota)
        BASE['probe'](binary,work,'resource-rgba',1)
        BASE['probe'](binary,work,'resource-failure')
        print('PASS mutation/resolved image payload lost: target red, control green',flush=True)
    finally:
        source.write_bytes(original)
        if hashlib.sha256(source.read_bytes()).digest()!=hashlib.sha256(original).digest():raise RuntimeError('resource source rollback mismatch')
    binary=build(engine,work,source_quota);BASE['probe'](binary,work)
    delta=work/'resource-rgba.delta';original=delta.read_bytes()
    try:
        if original.count(b'\x1b[3;5H')!=1:raise RuntimeError('nonunique placement mutation anchor')
        delta.write_bytes(original.replace(b'\x1b[3;5H',b'\x1b[3;6H'))
        BASE['probe'](binary,work,'resource-rgba',1)
        BASE['probe'](binary,work,'resource-failure')
        print('PASS mutation/actual image placement moved: target red, control green',flush=True)
    finally:
        delta.write_bytes(original)
        if hashlib.sha256(delta.read_bytes()).digest()!=hashlib.sha256(original).digest():raise RuntimeError('placement fixture rollback mismatch')
    BASE['probe'](binary,work)
    source=engine/'src/termio/Termio.zig';original=source.read_bytes();text=original.decode()
    old='if (stream.handler.semantic_failure) return error.ReplicaSemanticFailure;'
    if text.count(old)!=1:raise RuntimeError('nonunique semantic failure anchor')
    try:
        source.write_text(text.replace(old,'if (false) return error.ReplicaSemanticFailure;',1)+f'\n// {MARK}\n')
        binary=build(engine,work,source_quota)
        BASE['probe'](binary,work,'missing',1)
        BASE['probe'](binary,work,'resource-failure')
        print('PASS mutation/resource failure hidden from snapshot: target red, control green',flush=True)
    finally:
        source.write_bytes(original)
        if hashlib.sha256(source.read_bytes()).digest()!=hashlib.sha256(original).digest():raise RuntimeError('semantic failure rollback mismatch')
    binary=build(engine,work,source_quota)
    for case in ('all','missing','duplicate'):BASE['probe'](binary,work,case)

def quota_mutations(engine,work,only=None):
    variants=(('source quota absent during decode','src/termio/Termio.zig',
        '.kitty_image_storage_limit = opts.replica_image_storage_limit,','.kitty_image_storage_limit = null,','resource-reject'),
      ('GUI config overwrites source quota','src/termio/Termio.zig',
        'self.replica_image_storage_limit orelse config.image_storage_limit','config.image_storage_limit','resource-reject'),
      ('missing quota accepted','src/apprt/embedded.zig',
        'if (opts.p0_resource_resolve != null and !opts.p0_image_storage_limit_set) return error.MissingReplicaImageLimit;',
        'if (false) return error.MissingReplicaImageLimit;','no-limit'),
      ('resource resolver lost during config reload','src/termio/Termio.zig',
        'if (self.replica_resources.resolve != null) limits.resource_hooks = &self.replica_resources;',
        'if (false) limits.resource_hooks = &self.replica_resources;','resource-reject'))
    for label,path,old,new,target in variants:
        if only is not None and label!=only:continue
        source=engine/path;original=source.read_bytes();text=original.decode()
        if text.count(old)!=1:raise RuntimeError(f'nonunique quota anchor: {label}')
        try:
            source.write_text(text.replace(old,new,1)+f'\n// {MARK}\n')
            binary=build(engine,work,True)
            BASE['probe'](binary,work,target,1);BASE['probe'](binary,work,'resource-rgba')
            print(f'PASS mutation/{label}: target red, control green',flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest()!=hashlib.sha256(original).digest():raise RuntimeError('source quota rollback mismatch')
        binary=build(engine,work,True);BASE['probe'](binary,work,target)
    for case in ('all','missing','duplicate','no-limit'):BASE['probe'](binary,work,case)

def snapshot_mutations(engine,work,only=None):
    variants=(('checkpoint image import omitted','src/termio/Termio.zig',
      'try @import("../terminal/p0_resource_snapshot.zig").decodeWithLimit(alloc, &term, bytes, 16 * 1024 * 1024, opts.replica_image_storage_limit);',
      'if (false) try @import("../terminal/p0_resource_snapshot.zig").decodeWithLimit(alloc, &term, bytes, 16 * 1024 * 1024, opts.replica_image_storage_limit);','static-rgba'),
      ('resource snapshot quota mismatch accepted','src/terminal/p0_resource_snapshot.zig',
      'if (expected_limit) |expected| if (limit != expected) return error.InvalidSourceLimit;',
      '_ = expected_limit;','wrong-limit'),
      ('missing resource snapshot accepted','src/apprt/embedded.zig',
      'if (opts.p0_resources_snapshot == null and opts.p0_resources_snapshot_len != 0) return error.InvalidReplicaResourcesSnapshot;',
      'if (false) return error.InvalidReplicaResourcesSnapshot;','missing-bundle'),
      ('failed construction retains font reference','src/Surface.zig',
      'errdefer app.font_grid_set.deref(font_grid_key);','','wrong-limit'))
    for label,path,old,new,target in variants:
        if only is not None and label!=only:continue
        source=engine/path;original=source.read_bytes();text=original.decode()
        if text.count(old)!=1:raise RuntimeError(f'nonunique resource import anchor: {label}')
        try:
            source.write_text(text.replace(old,new,1)+f'\n// {MARK}\n')
            binary=build(engine,work,True,True)
            if label=='failed construction retains font reference':
                result=subprocess.run([str(binary),str(work),target],capture_output=True,text=True,timeout=30)
                sys.stdout.write(result.stdout);sys.stderr.write(result.stderr)
                if result.returncode!=-6 or 'self.font_grid_set.count() == 0' not in result.stderr:
                    raise RuntimeError('expected the specific font reference rollback assertion')
            else:BASE['probe'](binary,work,target,1)
            BASE['probe'](binary,work,'resource-rgba')
            print(f'PASS mutation/{label}: target red, control green',flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest()!=hashlib.sha256(original).digest():raise RuntimeError('static resource import rollback mismatch')
        binary=build(engine,work,True,True);BASE['probe'](binary,work,target)
    for case in ('all','missing','duplicate','no-limit','wrong-limit','missing-bundle'):BASE['probe'](binary,work,case)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--mutations',action='store_true')
    parser.add_argument('--source-quota',action='store_true');parser.add_argument('--quota-mutations',action='store_true')
    parser.add_argument('--static-resources',action='store_true');parser.add_argument('--snapshot-mutations',action='store_true')
    args=parser.parse_args()
    if sum((args.mutations,args.quota_mutations,args.snapshot_mutations))>1:parser.error('mutation modes are mutually exclusive')
    static_resources=args.static_resources or args.snapshot_mutations
    if static_resources and (args.mutations or args.quota_mutations):parser.error('static resources require --snapshot-mutations for mutation checks')
    source_quota=args.source_quota or args.quota_mutations or static_resources
    if sys.platform!='darwin' or os.uname().machine!='arm64':
        raise RuntimeError('full Metal resource prototype requires native Apple Silicon macOS')
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    files=[ROOT/'terminal/session-state/full-surface/probe.m',ROOT/'terminal/session-state/full-surface/resource-fixture.c',
           ROOT/'terminal/session-state/resource-hooks-probe.c',*[PATCHDIR/name for name in ('0005-resource-io-hooks.patch','0006-builtin-png.patch','0007-surface-resource-hooks.patch')]]
    if source_quota:files.append(PATCHDIR/'0008-source-image-quota.patch')
    if static_resources:files.extend(PATCHDIR/name for name in ('0009-static-resource-snapshot.patch','0010-surface-resource-snapshot.patch'))
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,BASE['interrupted'])
    try:
        runpy.run_path(str(ROOT/'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-full-resources-',dir=ROOT/'tmp') as tmp:
            work=Path(tmp);engine=BASE['prepare'](work)
            for patch in files[3:]:
                BASE['run'](['git','-C',str(engine),'apply','--check',str(patch)])
                BASE['run'](['git','-C',str(engine),'apply',str(patch)])
            binary=build(engine,work,source_quota,static_resources);fixtures(engine,work,source_quota,static_resources)
            for case in ('all','missing','duplicate')+(('no-limit',) if source_quota else ())+(('wrong-limit','missing-bundle') if static_resources else ()):BASE['probe'](binary,work,case)
            if args.snapshot_mutations:
                snapshot_mutations(engine,work);print('PASS 4 full static resource mutations and rollback; P0 remains incomplete');return 0
            if args.quota_mutations:
                quota_mutations(engine,work);print('PASS 4 source quota mutations and rollback; P0 remains incomplete');return 0
            if args.mutations:
                mutations(engine,work,source_quota);print('PASS 3 full resource mutations and rollback; P0 remains incomplete');return 0
        print('P0 INCOMPLETE: loading/animation snapshots, ordered IPC and live reconnect are absent');return 3
    finally:
        for p,digest in hashes.items():
            if hashlib.sha256(p.read_bytes()).digest()!=digest:raise RuntimeError(f'repository source changed: {p}')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,subprocess.SubprocessError,InterruptedError) as error:
        print(f'full resource probe failed: {error}; see tmp/session-surface-build.log',file=sys.stderr);sys.exit(1)
