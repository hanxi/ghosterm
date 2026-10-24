#!/usr/bin/env python3
"""Real headless VT factory + common runtime; isolated ordinary-failure mutations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'tmp/daemon-vt-mutations'
MARK = 'GT_DAEMON_VT_MUTATION_MARK'
NAMES = ('daemon/src/vt-state.c', 'daemon/src/vt-state.h', 'daemon/tests/test-vt-state.c',
         'daemon/src/vt-snapshot.c', 'daemon/src/vt-snapshot.h', 'daemon/tests/test-checkpoint-wire.c',
         'terminal/state/checkpoint-wire.c', 'terminal/state/checkpoint-wire.h',
         *(f'terminal/session-state/{name}' for name in
           ('budget-posix.c', 'budget-posix.h', 'checkpoint-budget.c', 'checkpoint-budget.h',
            'frozen-budget.c', 'frozen-budget.h')))
MODES = {'state': ('authority', 'lifetime', 'policy', 'capacity', 'allocation', 'runtime', 'resources', 'snapshot', 'data-flow'),
         'wire': ('vectors', 'corruption', 'malformed', 'limits', 'policy')}


def run(args, timeout=30):
    process = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, start_new_session=True)
    try:
        out, _ = process.communicate(timeout=timeout)
    except BaseException:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait()
        raise
    return process.returncode, out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine-include', type=Path, required=True)
    parser.add_argument('--engine-library', type=Path, required=True)
    args = parser.parse_args()
    include, library = args.engine_include.resolve(), args.engine_library.resolve()
    if not (include / 'ghostty/vt.h').is_file() or not library.is_file():
        raise RuntimeError('Pinned patched engine headers/library are required')
    originals = {name: (ROOT / name).read_bytes() for name in NAMES}
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()) or any(
            MARK in path.read_text() for path in WORK.rglob('*.[ch]')):
        return 2

    def interrupted(signum, frame):
        raise RuntimeError('daemon VT mutation interrupted: ' + str(signum))

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    os.environ['TMPDIR'] = str(ROOT / 'tmp')
    os.umask(0o077)
    copies = {name: WORK / name for name in NAMES}
    for path in copies.values():
        path.parent.mkdir(parents=True, exist_ok=True)

    def restore():
        for name, path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).hexdigest() != hashes[name] or (ROOT / name).read_bytes() != originals[name]:
                raise RuntimeError('daemon VT SHA256 rollback failed/' + name)

    restore()
    rc, out = run(['bash', str(ROOT / 'scripts/build-core.sh')], 120)
    if rc:
        raise RuntimeError('core dependency build failed: ' + out)
    archive = ROOT / 'core/build/libghosterm_core.a'
    dependencies = [archive, library, *sorted(include.rglob('*.h'))]
    dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}

    def build(suite):
        sources = ([copies[name] for name in NAMES if name.endswith('.c') and name != 'daemon/tests/test-checkpoint-wire.c']
                   if suite == 'state' else [copies['terminal/state/checkpoint-wire.c'], copies['daemon/tests/test-checkpoint-wire.c']])
        rc, out = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
                       '-D_DARWIN_C_SOURCE' if sys.platform == 'darwin' else '-D_POSIX_C_SOURCE=200809L',
                       '-Wall', '-Wextra', '-Werror', '-I', str(WORK / 'daemon/src'),
                       '-I', str(WORK / 'terminal/session-state'), '-I', str(ROOT / 'core/src'),
                       '-I', str(WORK / 'terminal/state'),
                       '-I', str(ROOT / 'core/include'), '-I', str(include),
                       *map(str, sources), str(archive), str(library), '-pthread',
                       *(['-lutil'] if sys.platform.startswith('linux') else []),
                       '-Wl,-rpath,' + str(library.parent), '-o', str(WORK / suite)])
        if rc:
            raise RuntimeError('daemon VT compilation failed: ' + out)
        for p, digest in dependency_hashes.items():
            if hashlib.sha256(Path(p).read_bytes()).hexdigest() != digest:
                raise RuntimeError('daemon VT build dependency changed: ' + p)

    def probe(suite, mode, reject=False):
        rc, out = run([str(WORK / suite), mode] + ([str(ROOT)] if suite == 'state' else []))
        if reject:
            if rc != 1 or 'FAIL ' not in out:
                raise RuntimeError('daemon VT mutation needs ordinary FAIL/exit 1/' + mode + ': ' + out)
        elif rc:
            raise RuntimeError('daemon VT baseline/control failed/' + mode + ': ' + out)

    specs = (('PTY query sink disconnected',
      'daemon/src/vt-state.c',
      '(void)s->sink.write(s->sink.context,bytes,n);',
      '(void)s;(void)bytes;(void)n;',
      'state',
      'authority',
      'state',
      'allocation'),
     ('color query ignores current scheme',
      'daemon/src/vt-state.c',
      '*out=s->dark?GHOSTTY_COLOR_SCHEME_DARK:GHOSTTY_COLOR_SCHEME_LIGHT;',
      '*out=s->dark?GHOSTTY_COLOR_SCHEME_LIGHT:GHOSTTY_COLOR_SCHEME_DARK;',
      'state',
      'authority',
      'state',
      'allocation'),
     ('resize applies wrong cells',
      'daemon/src/vt-state.c',
      'size.cols,size.rows,c->cell_width,c->cell_height',
      'size.cols+1,size.rows,c->cell_width,c->cell_height',
      'state',
      'authority',
      'state',
      'allocation'),
     ('frozen color metadata lost',
      'daemon/src/vt-state.c',
      'v->dark=s->dark;',
      'v->dark=false;',
      'state',
      'authority',
      'state',
      'allocation'),
     ('history policy metadata lost',
      'daemon/src/vt-state.c',
      '.history_bytes=c->history_bytes,',
      '.history_bytes=0,',
      'state',
      'authority',
      'state',
      'allocation'),
     ('external resource request silently accepted',
      'daemon/src/vt-state.c',
      'if(c->external_resources)return GT_ERR_UNSUPPORTED;',
      'if(false && c->external_resources)return GT_ERR_UNSUPPORTED;',
      'state',
      'policy',
      'state',
      'authority'),
     ('continuation tracking can be disabled',
      'daemon/src/vt-state.c',
      ' || !c->continuation_bytes',
      '',
      'state',
      'policy',
      'state',
      'authority'),
     ('encoder failure retains caller garbage',
      'daemon/src/vt-state.c',
      'if(out)*out=NULL;\n  const vt_frozen *v=object;',
      'const vt_frozen *v=object;',
      'state',
      'policy',
      'state',
      'capacity'),
     ('state grant undercharged',
      'daemon/src/vt-state.c',
      '&f->budget,c->state_bytes)',
      '&f->budget,c->state_bytes/2)',
      'state',
      'capacity',
      'state',
      'authority'),
     ('encoded lifetime does not hold factory',
      'daemon/src/vt-state.c',
      'if(atomic_load(&f->objects))return GT_RUNTIME_BUSY;',
      'if(false && atomic_load(&f->objects))return GT_RUNTIME_BUSY;',
      'state',
      'lifetime',
      'state',
      'allocation'),
     ('resource segment omitted',
      'daemon/src/vt-state.c',
      '.resource_bytes=e->checkpoint.resources_len,',
      '.resource_bytes=0,',
      'state',
      'resources',
      'state',
      'capacity'),
     ('encoded record not refunded',
      'daemon/src/vt-state.c',
      'record_free(f,e,sizeof(*e));',
      '(void)f;',
      'state',
      'lifetime',
      'state',
      'capacity'),
     ('container magic ignored',
      'terminal/state/checkpoint-wire.c',
      'if(memcmp(p,"GTVC",4))',
      'if(false && memcmp(p,"GTVC",4))',
      'wire',
      'malformed',
      'wire',
      'policy'),
     ('container version accepted',
      'terminal/state/checkpoint-wire.c',
      'if(get(p+4,2)!=1)',
      'if(false && get(p+4,2)!=1)',
      'wire',
      'policy',
      'wire',
      'vectors'),
     ('container header extent ignored',
      'terminal/state/checkpoint-wire.c',
      'get(p+6,2)!=GT_VT_SNAPSHOT_HEADER || ',
      '',
      'wire',
      'malformed',
      'wire',
      'vectors'),
     ('unknown flags accepted',
      'terminal/state/checkpoint-wire.c',
      '(get(p+8,4)&~UINT64_C(3))',
      'false',
      'wire',
      'malformed',
      'wire',
      'vectors'),
     ('reserved bytes ignored',
      'terminal/state/checkpoint-wire.c',
      'if(p[i])return GT_ERR_PROTOCOL;',
      'if(false && p[i])return GT_ERR_PROTOCOL;',
      'wire',
      'malformed',
      'wire',
      'vectors'),
     ('unknown profile accepted',
      'terminal/state/checkpoint-wire.c',
      'if(get(p+12,4)!=GT_VT_PROFILE_INLINE)',
      'if(false && get(p+12,4)!=GT_VT_PROFILE_INLINE)',
      'wire',
      'policy',
      'wire',
      'vectors'),
     ('canonical length may be zero',
      'terminal/state/checkpoint-wire.c',
      '!canonical || ',
      '',
      'wire',
      'malformed',
      'wire',
      'vectors'),
     ('resource length may be zero',
      'terminal/state/checkpoint-wire.c',
      '!resources || ',
      '',
      'wire',
      'malformed',
      'wire',
      'vectors'),
     ('trailing payload accepted',
      'terminal/state/checkpoint-wire.c',
      'resources!=n-GT_VT_SNAPSHOT_HEADER-canonical',
      'resources>n-GT_VT_SNAPSHOT_HEADER-canonical',
      'wire',
      'malformed',
      'wire',
      'vectors'),
     ('CRC check omitted',
      'terminal/state/checkpoint-wire.c',
      ' || get(p+124,4)!=crc(p,n)',
      '',
      'wire',
      'corruption',
      'wire',
      'vectors'),
     ('byte order reversed',
      'terminal/state/checkpoint-wire.c',
      '(v<<8)|p[i]',
      '(v<<8)|p[n-1-i]',
      'wire',
      'vectors',
      'wire',
      'limits'),
     ('continuation policy omitted',
      'terminal/state/checkpoint-wire.c',
      ' && v->continuation_bytes',
      '',
      'wire',
      'policy',
      'wire',
      'vectors'),
     ('history policy omitted',
      'terminal/state/checkpoint-wire.c',
      'v->history_bytes && ',
      '',
      'wire',
      'malformed',
      'wire',
      'vectors'),
     ('container maximum drift',
      'terminal/state/checkpoint-wire.h',
      'GT_VT_SNAPSHOT_LIMIT=64*1024*1024',
      'GT_VT_SNAPSHOT_LIMIT=64*1024*1024+1',
      'wire',
      'limits',
      'wire',
      'vectors'),
     ('container header size drift',
      'terminal/state/checkpoint-wire.h',
      'GT_VT_SNAPSHOT_HEADER=128',
      'GT_VT_SNAPSHOT_HEADER=129',
      'wire',
      'vectors',
      'state',
      'allocation'),
     ('canonical extent may overflow',
      'terminal/state/checkpoint-wire.c',
      'v->canonical_bytes>GT_VT_SNAPSHOT_LIMIT-GT_VT_SNAPSHOT_HEADER ||',
      'false ||',
      'wire',
      'limits',
      'wire',
      'vectors'),
     ('resource bytes not copied',
      'terminal/state/checkpoint-wire.c',
      'memcpy(p+GT_VT_SNAPSHOT_HEADER+v->canonical_bytes,v->resources,v->resource_bytes);',
      '/* resource copy omitted */',
      'wire',
      'vectors',
      'wire',
      'policy'),
     ('adapter failure retains garbage output',
      'daemon/src/vt-snapshot.c',
      'if(out)*out=NULL;',
      '',
      'state',
      'snapshot',
      'state',
      'authority'),
     ('adapter blob not erased',
      'daemon/src/vt-snapshot.c',
      'for(size_t i=0;i<n;i++)p[i]=0;',
      'if(false)for(size_t i=0;i<n;i++)p[i]=0;',
      'state',
      'snapshot',
      'state',
      'authority'),
     ('adapter allocation charge differs',
      'daemon/src/vt-snapshot.c',
      'domain->allocate(domain->context,sizeof(*b)+n)',
      'domain->allocate(domain->context,sizeof(*b)+n+1)',
      'state',
      'snapshot',
      'state',
      'authority'),
     ('adapter blob ownership leaked',
      'daemon/src/vt-snapshot.c',
      'a.release(a.context,b,n);',
      '(void)a;',
      'state',
      'snapshot',
      'state',
      'authority'),
     ('container profile lost',
      'daemon/src/vt-state.c',
      '.profile=GT_VT_PROFILE_INLINE,',
      '.profile=0,',
      'state',
      'snapshot',
      'state',
      'allocation'),
     ('container continuation metadata lost',
      'daemon/src/vt-state.c',
      '.continuation_bytes=c->continuation_bytes,',
      '.continuation_bytes=0,',
      'state',
      'snapshot',
      'state',
      'authority'),
     ('DATA container corrupted after encoding', 'daemon/src/vt-snapshot.c',
      'if(rc==GT_OK && written!=n)', 'if(rc==GT_OK)b->bytes[128]^=1; if(rc==GT_OK && written!=n)',
      'state', 'data-flow', 'state', 'authority'),
     ('authority resize silently omitted', 'daemon/src/vt-state.c',
      'rc=p0_budget_resize_cells(&s->scope,s->terminal,size.cols,size.rows,c->cell_width,c->cell_height);',
      'rc=GHOSTTY_SUCCESS; (void)size; (void)c;', 'state', 'data-flow', 'state', 'allocation'))
    try:
        for suite, modes in MODES.items():
            build(suite)
            for mode in modes:
                probe(suite, mode)
        for label, name, old, new, suite, target, control_suite, control in specs:
            source = originals[name].decode()
            if source.count(old) != 1:
                raise RuntimeError('daemon VT mutation anchor drift/' + label)
            try:
                copies[name].write_text(source.replace(old, new) + '\n/* ' + MARK + ' */\n')
                build(suite)
                probe(suite, target, True)
                if control_suite != suite:
                    build(control_suite)
                probe(control_suite, control)
                print('PASS daemon VT mutation/' + label + ': ordinary red, same-mutant control green', flush=True)
            finally:
                restore()
        for suite, modes in MODES.items():
            build(suite)
            for mode in modes:
                probe(suite, mode)
        (ROOT / 'tmp/daemon-vt-proof.json').write_text(json.dumps(
            {'sources': hashes, 'dependencies': dependency_hashes, 'baselines': MODES,
             'mutations': len(specs)}, indent=2) + '\n')
        print(f'PASS {len(specs)} daemon VT mutations, {sum(map(len, MODES.values()))} baselines and SHA256 rollback', flush=True)
    finally:
        restore()
    return 0


if __name__ == '__main__':
    sys.exit(main())
