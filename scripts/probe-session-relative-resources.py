#!/usr/bin/env python3
"""Headless relative-placement resource codec prototype. Default exit 3."""
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
FULL=runpy.run_path(str(ROOT/'scripts/probe-session-surface-resources.py'))
VT=runpy.run_path(str(ROOT/'scripts/probe-session-resource-hooks.py'))
SOURCE=ROOT/'terminal/session-state/relative-resource-probe.c'
PATCHDIR=FULL['PATCHDIR']
PATCHES=tuple(PATCHDIR/name for name in ('0005-resource-io-hooks.patch','0006-builtin-png.patch','0007-surface-resource-hooks.patch','0008-source-image-quota.patch','0009-static-resource-snapshot.patch','0010-surface-resource-snapshot.patch','0011-relative-resource-snapshot.patch'))
MARK='GT_P0_RELATIVE_RESOURCE_MUTATION_MARK'
CASES=('static','relative','chain','forward','internal','missing-parent','cycle')

def build(engine,work):
    VT['build'](engine,work,True)
    binary=work/'relative-resources'
    VT['run']([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-D_DEFAULT_SOURCE','-DGHOSTERM_P0_BUILTIN_PNG',
        '-Wall','-Wextra','-Werror','-I',str(engine/'include'),'-I',str(ROOT/'terminal/session-state'),str(SOURCE),
        str(work/'out/lib/libghostty-vt.a'),*(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(binary)])
    return binary

def probe(binary,case,expected=0):
    p=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=15)
    sys.stdout.write(p.stdout);sys.stderr.write(p.stderr)
    if p.returncode!=expected:raise RuntimeError(f'relative/{case}: expected {expected}, got {p.returncode}')

def mutations(engine,work):
    source=engine/'src/terminal/p0_resource_snapshot.zig';original=source.read_bytes();text=original.decode()
    flatten='''                const chain = storage.resolveChain(p.location.relative) orelse return error.InvalidData;
                if (chain.root.location != .pin) return error.InvalidData;
                const origin = (s.pages.pointFromPin(.screen, chain.root.location.pin.*) orelse return error.InvalidData).screen;
                const point: @import("point.zig").Coordinate = .{
                    .x = std.math.cast(u16, @as(i64, origin.x) + chain.horizontal_offset) orelse return error.InvalidData,
                    .y = std.math.cast(u32, @as(i64, origin.y) + chain.vertical_offset) orelse return error.InvalidData,
                };
                const pin = s.pages.pin(.{ .screen = point }) orelse return error.InvalidData;
                p.location = .{ .pin = try s.pages.trackPin(pin) };
'''
    variants=(
      ('horizontal offset lost',[(
        '.horizontal_offset = @bitCast(try u32get(&reader)),',
        '.horizontal_offset = zero: { _ = try u32get(&reader); break :zero 0; },')],('relative',),'static'),
      ('internal parent namespace lost',[(
        '.parent = .{ .image_id = x, .placement_id = .{ .tag = if (parent_tag == 0) .internal else .external, .id = y } },',
        '.parent = .{ .image_id = x, .placement_id = .{ .tag = .external, .id = y } },')],('internal',),'relative'),
      ('ancestry validation omitted',[(
        'for (parents[0..placements], 0..) |maybe_parent, i| if (maybe_parent) |parent| {',
        'if (false) for (parents[0..placements], 0..) |maybe_parent, i| if (maybe_parent) |parent| {')],('missing-parent','cycle'),'static'),
      ('relative relationship flattened to matching pixels',[(
        '            try storage.placements.putNoClobber(alloc, key, p);',
        '            if (p.location == .relative) {\n'+flatten+'            }\n            try storage.placements.putNoClobber(alloc, key, p);')],('relative',),'static'),
    )
    if MARK in text:raise RuntimeError('residual relative mutation marker')
    for label,edits,targets,control in variants:
        changed=text
        for old,new in edits:
            if changed.count(old)!=1:raise RuntimeError(f'nonunique relative anchor: {label}')
            changed=changed.replace(old,new,1)
        try:
            source.write_text(changed+f'\n// {MARK}\n');binary=build(engine,work)
            for case in targets:probe(binary,case,1)
            probe(binary,control);print(f'PASS mutation/{label}: targets red, control green',flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest()!=hashlib.sha256(original).digest():raise RuntimeError('relative codec rollback mismatch')
        binary=build(engine,work)
        for case in targets:probe(binary,case)
    for case in CASES:probe(binary,case)
    print('PASS 4 relative resource mutations and rollback; P0 remains incomplete',flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--case',choices=CASES);p.add_argument('--mutations',action='store_true');args=p.parse_args()
    if args.case and args.mutations:p.error('--case and --mutations are mutually exclusive')
    files=(SOURCE,*PATCHES,ROOT/'terminal/session-state/resource-hooks-probe.c',ROOT/'terminal/session-state/resource-io-probe.c',ROOT/'terminal/session-state/probe.c')
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,VT['interrupted'])
    try:
        runpy.run_path(str(ROOT/'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-relative-resources-',dir=ROOT/'tmp') as tmp:
            work=Path(tmp);engine=FULL['BASE']['prepare'](work)
            for patch in PATCHES:
                VT['run'](['git','-C',str(engine),'apply','--check',str(patch)]);VT['run'](['git','-C',str(engine),'apply',str(patch)])
            binary=build(engine,work)
            for case in (args.case,) if args.case else CASES:probe(binary,case)
            if args.mutations:mutations(engine,work);return 0
        print('P0 INCOMPLETE: relative codec lacks full surface evidence, loading/animation and production recovery')
        return 0 if args.case else 3
    finally:
        for p,digest in hashes.items():
            if hashlib.sha256(p.read_bytes()).digest()!=digest:raise RuntimeError(f'repository source changed: {p}')
if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,subprocess.SubprocessError,InterruptedError) as error:
        print(f'relative resources probe failed: {error}; see tmp/session-resource-hooks-build.log',file=sys.stderr);sys.exit(1)
