#!/usr/bin/env python3
"""Experimental bounded static resource codec. Default exit 3: P0 remains incomplete."""
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
BASE=runpy.run_path(str(ROOT/'scripts/probe-session-resource-hooks.py'))
PATCH=ROOT/'patches/libghostty/8159838/experimental/0009-static-resource-snapshot.patch'
SOURCE=ROOT/'terminal/session-state/resource-snapshot-probe.c'
MARK='GT_P0_RESOURCE_SNAPSHOT_MUTATION_MARK'
CASES=('static','rgb','screens','virtual','placement-counter','ids','age','transient','loading','animation','relative','crc','late-error','oom','limit','oom-sweep')

def build(engine,work):
    BASE['build'](engine,work,True)
    binary=work/'resource-snapshot'
    BASE['run']([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-D_DEFAULT_SOURCE','-DGHOSTERM_P0_BUILTIN_PNG',
        '-Wall','-Wextra','-Werror','-I',str(engine/'include'),'-I',str(ROOT/'terminal/session-state'),str(SOURCE),
        str(work/'out/lib/libghostty-vt.a'),*(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(binary)])
    return binary

def probe(binary,case,expected=0):
    p=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=15)
    sys.stdout.write(p.stdout);sys.stderr.write(p.stderr)
    if p.returncode!=expected:raise RuntimeError(f'resource-snapshot/{case}: expected {expected}, got {p.returncode}')

def mutations(engine,work,start=0):
    module='src/terminal/p0_resource_snapshot.zig'
    variants=(
      ('decoded pixels corrupted',module,[(
        'img.data = .{ .complete = try alloc.dupe(u8, borrowed) };',
        'img.data = .{ .complete = try alloc.dupe(u8, borrowed) };\n            @memset(@constCast(img.data.complete), 0);')],'static','loading'),
      ('pin position moved',module,[(
        'const pin = s.pages.pin(.{ .screen = .{ .x = @intCast(x), .y = y } })',
        'const pin = s.pages.pin(.{ .screen = .{ .x = @intCast(x + 1), .y = y } })')],'static','virtual'),
      ('implicit ID counter lost',module,[('storage.next_image_id = next_image_id;','_ = next_image_id;')],'ids','static'),
      ('internal placement counter lost',module,[('storage.next_internal_placement_id = next_placement_id;','_ = next_placement_id;')],'placement-counter','static'),
      ('eviction age reversed',module,[('return a.generation < b.generation;','return a.generation > b.generation;')],'age','static'),
      ('transient hint lost',module,[('img.metadata.transient = flags & 1 != 0;','img.metadata.transient = false;')],'transient','age'),
      ('virtual placement flattened to pin',module,[('try put(w, if (p.location == .pin) 0 else 1);','try put(w, 0);')],'virtual','static'),
      ('RGB interpreted as RGBA',module,[('try put(w, if (img.format == .rgba) 4 else 3);','try put(w, 4);')],'rgb','static'),
      ('CRC ignored',module,[(' or std.mem.readInt(u32, data[12..16], .little) != std.hash.crc.Crc32Iscsi.hash(data[16..])','')],'crc','static'),
      ('preflight omitted before pin allocations',module,[('try preflight(t, data[16..], max_bytes);','// preflight omitted')],'late-error','static'),
      ('loading state silently omitted',module,[('if (storage.loading != null) return error.Unsupported;','if (false) return error.Unsupported;')],'loading','static'),
      ('relative parent silently lost',module,[('if (p.location == .relative) return error.Unsupported;','if (false) return error.Unsupported;')],'relative','static'),
      ('animation flattened to base image',module,[
        ('img.animation != null or img.data == .pending','img.data == .pending'),
        ('try put(w, storage.total_bytes);','try put(w, bytes: { var sum: usize = 0; var values = storage.images.valueIterator(); while (values.next()) |image| sum += image.data.len(); break :bytes sum; });'),
        ('total != storage.total_bytes or total > storage.total_limit','total > storage.total_limit')],'animation','static'),
      ('wire limit bypassed',module,[('if (data.len > max_bytes) return error.LimitExceeded;','if (false) return error.LimitExceeded;')],'limit','static'),
      ('decode allocation failure reports success','src/terminal/c/terminal.zig',[(
        '@import("../p0_resource_snapshot.zig").decode(wrapper.terminal.gpa(), wrapper.terminal, ptr.?[0..len], max_bytes) catch |err| return p0ResourceError(err);',
        '@import("../p0_resource_snapshot.zig").decode(wrapper.terminal.gpa(), wrapper.terminal, ptr.?[0..len], max_bytes) catch return 0;')],'oom','static'),
      ('staged resources not freed after allocation failure',module,[(
        'defer for (&slots, 0..) |*slot, index| if (slot.*) |*storage| storage.deinit(alloc, t.screens.get(if (index == 0) .primary else .alternate).?);',
        'if (false) for (&slots, 0..) |*slot, index| if (slot.*) |*storage| storage.deinit(alloc, t.screens.get(if (index == 0) .primary else .alternate).?);')],'oom-sweep','static'),
    )
    originals={engine/path:(engine/path).read_bytes() for _,path,_,_,_ in variants}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('residual resource snapshot mutation marker')
    for index,(label,path,edits,target,control) in enumerate(variants):
        if index<start:continue
        source=engine/path;original=originals[source];text=original.decode()
        for old,new in edits:
            if text.count(old)!=1:raise RuntimeError(f'nonunique mutation anchor: {label}')
            text=text.replace(old,new,1)
        try:
            source.write_text(text+f'\n// {MARK}\n');binary=build(engine,work)
            probe(binary,target,1);probe(binary,control)
            print(f'PASS mutation/{label}: target red, control green',flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest()!=hashlib.sha256(original).digest():raise RuntimeError('resource codec rollback mismatch')
        binary=build(engine,work);probe(binary,target)
    for case in CASES:probe(binary,case)
    print(f'PASS {len(variants)-start} selected resource snapshot mutations and rollback; P0 remains incomplete',flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--case',choices=CASES);p.add_argument('--mutations',action='store_true');args=p.parse_args()
    if args.case and args.mutations:p.error('--case and --mutations are mutually exclusive')
    files=(SOURCE,PATCH,BASE['PATCH'],BASE['PNG_PATCH'],ROOT/'terminal/session-state/resource-hooks-probe.c',ROOT/'terminal/session-state/resource-io-probe.c',ROOT/'terminal/session-state/probe.c')
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,BASE['interrupted'])
    try:
        runpy.run_path(str(ROOT/'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-resource-snapshot-',dir=ROOT/'tmp') as tmp:
            work=Path(tmp);engine=BASE['prepare'](work,True)
            BASE['run'](['git','-C',str(engine),'apply','--check',str(PATCH)]);BASE['run'](['git','-C',str(engine),'apply',str(PATCH)])
            binary=build(engine,work)
            for case in (args.case,) if args.case else CASES:probe(binary,case)
            if args.mutations:mutations(engine,work);return 0
        print('P0 INCOMPLETE: static resource codec is not full surface import, loading/animation or production recovery')
        return 0 if args.case else 3
    finally:
        for p,digest in hashes.items():
            if hashlib.sha256(p.read_bytes()).digest()!=digest:raise RuntimeError(f'repository source changed: {p}')
if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,subprocess.SubprocessError,InterruptedError) as error:
        print(f'resource snapshot probe failed: {error}; see tmp/session-resource-hooks-build.log',file=sys.stderr);sys.exit(1)
